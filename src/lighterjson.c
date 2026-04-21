/**
 * @file      lighterjson.c
 * @brief     JSON minifier
 * @author    Aaron Kaluszka
 * @version   2.0.0
 * @date      16 Apr 2026
 * @copyright Copyright 2017-2026 Aaron Kaluszka
 *            Licensed under the Apache License, Version 2.0 (the "License");
 *            you may not use this file except in compliance with the License.
 *            You may obtain a copy of the License at
 *                http://www.apache.org/licenses/LICENSE-2.0
 *            Unless required by applicable law or agreed to in writing, software
 *            distributed under the License is distributed on an "AS IS" BASIS,
 *            WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *            See the License for the specific language governing permissions and
 *            limitations under the License.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64) || defined(WIN32)
  #define LIGHTER_PLATFORM_WIN 1
  #include <windows.h>
#else
  #include <dirent.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#include "lighter_bitfield.h"
#include "lighter_common.h"
#include "lighter_cpu.h"
#include "lighter_memmap.h"
#include "lighter_number.h"
#include "lighter_string.h"
#include "lighter_transcode.h"

#if defined(_OPENMP)
  #include <omp.h>
#endif

typedef struct Context {
  int64_t precision;
  int quiet;
  int newlines;
  int disable_nfc;
  int force_utf8_output;
  int async_io;
  int preserve_neg_zero;
  int has_avx2;
  int has_neon;
  int has_rvv;
} Context;

typedef struct PathBuffer {
  char* buf;
  size_t cap;
} PathBuffer;

#define LIGHTER_PATH_BUFFER_INITIAL_CAPACITY 4096u
#define LIGHTER_UTF16_TO_UTF8_GROW_NUMERATOR 3u
#define LIGHTER_UTF16_TO_UTF8_GROW_DENOMINATOR 2u
#define LIGHTER_TRANSCODE_SLACK_BYTES 4u

#if LIGHTER_PLATFORM_X86
/** Advance run past a contiguous whitespace span with AVX2. */
LIGHTER_TARGET_AVX2
static uint8_t* skip_whitespace_avx2(uint8_t* run, uint8_t* end, int include_newline) {
  __m256i spaces = _mm256_set1_epi8(' ');
  __m256i tabs = _mm256_set1_epi8('\t');
  __m256i crs = _mm256_set1_epi8('\r');
  __m256i lfs = _mm256_set1_epi8('\n');
  while (run + 32 <= end) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)run);
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(chunk, spaces), _mm256_or_si256(_mm256_cmpeq_epi8(chunk, tabs), _mm256_cmpeq_epi8(chunk, crs)));
    if (include_newline) {
      m = _mm256_or_si256(m, _mm256_cmpeq_epi8(chunk, lfs));
    }
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(m);
    if (mask != 0xFFFFFFFF) {
  #if defined(_MSC_VER)
      unsigned long offset;
      _BitScanForward(&offset, ~mask);
      return run + offset;
  #else
      return run + __builtin_ctz(~mask);
  #endif
    }
    run += 32;
  }
  return run;
}
#endif /* LIGHTER_PLATFORM_X86 */

/** Advance run past a contiguous whitespace span, using SIMD when worthwhile. */
static inline uint8_t* skip_whitespace_impl(uint8_t* run, uint8_t* end, int include_newline, int has_avx2, int has_neon, int has_rvv) {
  (void)has_avx2;
  (void)has_neon;
  (void)has_rvv;
  /* Use a short scalar prefix before the SIMD scan so brief whitespace runs are
   * handled without the full vector setup cost. */
  const uint64_t ws_mask =
      include_newline ? ((1ULL << ' ') | (1ULL << '\t') | (1ULL << '\n') | (1ULL << '\r')) : ((1ULL << ' ') | (1ULL << '\t') | (1ULL << '\r'));
  uint8_t* fast_end = run + 2;
  if (fast_end > end) {
    fast_end = end;
  }
  while (run < fast_end) {
    uint8_t c = *run;
    if (c >= 64 || ((ws_mask >> c) & 1ULL) == 0) {
      return run;
    }
    ++run;
  }
  if (run >= end) {
    return run;
  }
#if LIGHTER_PLATFORM_X86
  if (has_avx2 && LIGHTER_SIMD_SITE_ENABLED("whitespace")) {
    run = skip_whitespace_avx2(run, end, include_newline);
  }
#elif LIGHTER_PLATFORM_ARM64
  if (has_neon && LIGHTER_SIMD_SITE_ENABLED("whitespace")) {
    uint8x16_t spaces = vdupq_n_u8(' ');
    uint8x16_t tabs = vdupq_n_u8('\t');
    uint8x16_t crs = vdupq_n_u8('\r');
    uint8x16_t lfs = vdupq_n_u8('\n');
    while (run + 16 <= end) {
      uint8x16_t chunk = vld1q_u8(run);
      uint8x16_t m = vorrq_u8(vceqq_u8(chunk, spaces), vorrq_u8(vceqq_u8(chunk, tabs), vceqq_u8(chunk, crs)));
      if (include_newline) {
        m = vorrq_u8(m, vceqq_u8(chunk, lfs));
      }
      uint64x2_t u64 = vreinterpretq_u64_u8(m);
      uint64_t low = vgetq_lane_u64(u64, 0);
      uint64_t high = vgetq_lane_u64(u64, 1);
      if (low != 0xFFFFFFFFFFFFFFFFULL) {
        return run + (__builtin_ctzll(~low) >> 3);
      }
      if (high != 0xFFFFFFFFFFFFFFFFULL) {
        return run + (__builtin_ctzll(~high) >> 3) + 8;
      }
      run += 16;
    }
  }
#endif
  /* Scalar tail (remainder after SIMD, or when SIMD isn't available) */
  while (run < end) {
    uint8_t c = *run;
    if (c >= 64 || ((ws_mask >> c) & 1ULL) == 0) {
      break;
    }
    ++run;
  }
  return run;
}

/** Skip the whitespace run at rindex and flush any preceding output. */
void skip_whitespace_run(LighterData* data, int include_newline, Context* ctx) {
  uint8_t* run = skip_whitespace_impl(data->rindex, data->data_end, include_newline, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
  lighter_write_data(data, run - data->rindex);
}

/** Consume literal when it matches at rindex, or copy one byte through on mismatch. */
void do_literal(LighterData* data, const char* literal, size_t length) {
  if (data->rindex + length <= data->data_end && memcmp(data->rindex, literal, length) == 0) {
    data->rindex += length;
  } else {
    lighter_write_data(data, 1);
  }
}

/** Dispatch JSON string parsing with the current context flags. */
static void do_string(LighterData* data, Context* ctx) {
  lighter_do_string(data, ctx->disable_nfc, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
}

/** Dispatch JSON number parsing with the current precision and CPU flags. */
static void do_number(LighterData* data, Context* ctx) {
  lighter_do_number_impl(data, ctx->precision, ctx->preserve_neg_zero, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
}

/** Parse an object key or detect the end of the current object. */
static int do_object_label(LighterData* data, Context* ctx, int line_start) {
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case '"':
        do_string(data, ctx);
        return 0;
      case '}':
        return 1;
      case '\n':
        if (line_start) {
          return 1;
        }
        skip_whitespace_run(data, 1, ctx);
        break;
      case ' ':
      case '\t':
      case '\r':
        skip_whitespace_run(data, 1, ctx);
        break;
      default:
        /* Drop invalid byte before key. */
        lighter_write_data(data, 0);
        ++(data->rindex);
        data->lindex = data->rindex;
    }
  }
  return 1;
}

/** Parse object punctuation after a key and position the reader at the value. */
static void do_object(LighterData* data, Context* ctx, int line_start) {
  if (do_object_label(data, ctx, line_start)) {
    return;
  }
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case ':':
        ++(data->rindex);
        lighter_write_data(data, 0);
        return;
      case '\n':
        if (line_start) {
          return;
        }
        skip_whitespace_run(data, 1, ctx);
        break;
      case ' ':
      case '\t':
      case '\r':
        skip_whitespace_run(data, 1, ctx);
        break;
      default:
        /* Drop invalid byte between key and ':'. */
        lighter_write_data(data, 0);
        ++(data->rindex);
        data->lindex = data->rindex;
    }
  }
}

/** Parse values from the current position with structural tracking and garbage
 * recovery. The caller owns parent_types (must call init_bits before, free heap
 * storage after). On return, parent_types holds the open-container stack at EOF
 * and *out_comma_ok indicates whether the parser was expecting a separator (1)
 * or a value (0). */
static void do_value(LighterData* data, Context* ctx, int line_start, Bitfield* parent_types, int* out_comma_ok) {
  int comma_ok = 0;
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case '"':
        do_string(data, ctx);
        comma_ok = 1;
        break;
      case '{':
        ++(data->rindex);
        push_set_bit(parent_types);
        do_object(data, ctx, line_start);
        comma_ok = 0;
        break;
      case '}':
        if (parent_types->current == Object) {
          ++(data->rindex);
          pop_bit(parent_types);
          comma_ok = 1;
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case '[':
        ++(data->rindex);
        push_clear_bit(parent_types);
        comma_ok = 0;
        break;
      case ']':
        if (parent_types->current == Array) {
          ++(data->rindex);
          pop_bit(parent_types);
          comma_ok = 1;
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case ',':
        if (comma_ok && parent_types->current != None) {
          ++(data->rindex);
          comma_ok = 0;
          if (parent_types->current == Object) {
            do_object(data, ctx, line_start);
          }
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case 't':
        do_literal(data, "true", 4);
        comma_ok = 1;
        break;
      case 'f':
        do_literal(data, "false", 5);
        comma_ok = 1;
        break;
      case 'n':
        do_literal(data, "null", 4);
        comma_ok = 1;
        break;
      case '-':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        do_number(data, ctx);
        comma_ok = 1;
        break;
      case '\n':
        /* NDJSON record separator handling. -N preserves every '\n' (blank lines
         * round-trip, including leading); -n preserves one '\n' once a record has
         * been emitted, collapsing runs. Inside a value or in compact mode the
         * newline is dropped along with surrounding whitespace. */
        if (parent_types->current == None && ctx->newlines == 2) {
          ++(data->rindex);
        } else if (parent_types->current == None && ctx->newlines == 1 && data->windex > data->data_start) {
          ++(data->rindex);
          skip_whitespace_run(data, 1, ctx);
        } else {
          skip_whitespace_run(data, 1, ctx);
        }
        break;
      case ' ':
      case '\t':
      case '\r':
        skip_whitespace_run(data, line_start ? 0 : 1, ctx);
        break;
      default:
        /* Invalid byte outside any value: drop it. Flush anything pending first
         * so legitimate prior bytes aren't lost, then advance past the garbage
         * without including it in the next pending segment. */
        lighter_write_data(data, 0);
        ++(data->rindex);
        data->lindex = data->rindex;
    }
  }
  /* Flush any trailing pending bytes before EOF closure handling. */
  lighter_write_data(data, 0);
  *out_comma_ok = comma_ok;
}

/** Append synthetic close braces / null-padding for truncated input. The caller
 * must ensure data->buffer_end has room for the worst case: 1 byte for an
 * unterminated string's closing quote, 4 bytes for an optional dangling ':' ->
 * ":null" expansion, and one byte per open container. */
static void finalize_closures(LighterData* data, Bitfield* parent_types, int comma_ok) {
  if (data->needs_quote && data->windex < data->buffer_end) {
    *data->windex++ = '"';
    data->needs_quote = 0;
  }
  if (parent_types->current != (size_t)-1 && comma_ok == 0 && data->windex > data->data_start) {
    /* If we ended on a dangling ':' inside an object, the key has no value yet.
     * Replace ':' with ":null" so the result is valid JSON instead of {"k"}. */
    if (data->windex[-1] == ':' && data->windex + 4 <= data->buffer_end) {
      memcpy(data->windex, "null", 4);
      data->windex += 4;
    } else if (data->windex[-1] == ',') {
      --data->windex;
    }
  }
  while (parent_types->current != (size_t)-1 && data->windex < data->buffer_end) {
    *data->windex++ = (parent_types->current == Object) ? '}' : ']';
    pop_bit(parent_types);
  }
}

/** Return non-zero when name should be processed for the current mode. */
static int dir_should_process(Context* ctx, const char* name) {
  size_t len = strlen(name);
  return (len >= 5 && strcmp(name + len - 5, ".json") == 0) ||
         (ctx->newlines && ((len >= 6 && strcmp(name + len - 6, ".jsonl") == 0) || (len >= 7 && strcmp(name + len - 7, ".ndjson") == 0)));
}

/** Open, minify, and rewrite one JSON file in place. */
static int do_file(Context* ctx, char filename[]) {
  LighterMap map = {0};
  size_t map_capacity;
  if (lighter_map_open(&map, filename, 0) != 0) {
    return EXIT_FAILURE;
  }
  map_capacity = map.size;
  LighterEncoding orig_encoding = lighter_detect_encoding(map.data, map.size);
  size_t bom_size = lighter_encoding_bom_size(orig_encoding, map.data, map.size);
  size_t actual_size = map.size - bom_size;
  size_t orig_file_size = map.size; /* preserve for final savings report */

  if (orig_encoding != LIGHTER_ENC_UTF8) {
    size_t utf8_size;
    if (orig_encoding == LIGHTER_ENC_UTF32LE || orig_encoding == LIGHTER_ENC_UTF32BE) {
      /* UTF-32 → UTF-8 is always strictly smaller (4 source bytes per codepoint
       * become at most 4 UTF-8 bytes, usually fewer). Output never overtakes
       * input, so we can transcode in place without expanding the mapping. */
      if (lighter_utf32_is_ascii_only(map.data + bom_size, actual_size, orig_encoding == LIGHTER_ENC_UTF32LE)) {
        /* All codepoints < 0x80: one UTF-8 byte each. Pack low byte of each
         * 32-bit codepoint into a compact UTF-8 byte stream. */
        lighter_transcode_utf32_ascii_to_utf8(map.data + bom_size, actual_size, map.data, orig_encoding == LIGHTER_ENC_UTF32LE, &utf8_size);
      } else {
        lighter_transcode_to_utf8(map.data + bom_size, actual_size, map.data, orig_encoding, &utf8_size);
      }
      map.size = utf8_size;
    } else if (lighter_utf16_is_ascii_only(map.data + bom_size, actual_size, orig_encoding == LIGHTER_ENC_UTF16LE)) {
      /* UTF-16 ASCII-only: every codepoint < 0x80, so each pair of source bytes
       * becomes a single UTF-8 byte. Output is half the input size and never
       * overtakes the read pointer; transcode in place via a tight pack loop. */
      lighter_transcode_utf16_ascii_to_utf8(map.data + bom_size, actual_size, map.data, orig_encoding == LIGHTER_ENC_UTF16LE, &utf8_size);
      map.size = utf8_size;
    } else {
      /* UTF-16 can grow: a BMP non-Latin codepoint occupies 2 UTF-16 bytes but
       * 3 UTF-8 bytes. Worst case is 1.5x. Expand the mapping first, then
       * transcode using the relocated source mapping. */
      size_t max_utf8 = (actual_size * LIGHTER_UTF16_TO_UTF8_GROW_NUMERATOR / LIGHTER_UTF16_TO_UTF8_GROW_DENOMINATOR) + LIGHTER_TRANSCODE_SLACK_BYTES;
      uint8_t* old_data = NULL;
      size_t old_size_full = 0;
      if (lighter_map_expand(&map, max_utf8, &old_data, &old_size_full) != 0) {
        fprintf(stderr, "Could not expand file for transcoding\n");
        lighter_map_close(&map);
        return EXIT_FAILURE;
      }

      uint8_t* transcode_src;
      if (old_data) {
        /* Two distinct mappings: read from the old, write into the new. */
        transcode_src = old_data + bom_size;
      } else {
        /* In-place grow gave us one mapping covering both source and destination.
         * Slide the source to the tail so the forward write can't overrun it. */
        memmove(map.data + max_utf8 - actual_size, map.data + bom_size, actual_size);
        transcode_src = map.data + max_utf8 - actual_size;
      }

      lighter_transcode_to_utf8(transcode_src, actual_size, map.data, orig_encoding, &utf8_size);
      map.size = utf8_size;
      map_capacity = max_utf8;

      if (old_data) {
        lighter_map_unmap(old_data, old_size_full);
      }
    }
    bom_size = 0; /* BOM was already stripped during transcoding */
  }
  /* For UTF-8 with a BOM, leave map.size alone; rindex starts past the BOM so the
   * source range is [map.data + bom_size, map.data + map.size). data_start stays at
   * map.data so windex can overwrite the BOM with minified content. */

  LighterData data;
  data.data_start = map.data;
  data.windex = map.data;
  data.data_end = map.data + map.size;
  data.buffer_end = map.data + map_capacity;
  data.rindex = map.data + bom_size;
  data.lindex = data.rindex;
  data.needs_quote = 0;
  data.saw_non_ascii = 0;

  int exit_code = EXIT_SUCCESS;
  Bitfield parent_types;
  init_bits(&parent_types);
  int comma_ok;
  do_value(&data, ctx, ctx->newlines == 2 ? 2 : 0, &parent_types, &comma_ok); /* clean up leading newlines in -N mode */
  if (ctx->newlines) {
    while (data.rindex < data.data_end) {
      do_value(&data, ctx, ctx->newlines, &parent_types, &comma_ok);
    }
  }
  lighter_write_data(&data, 0);

  if (parent_types.current != (size_t)-1 || data.needs_quote) {
    /* Compute exactly what finalize_closures will append: 1 byte for an unterminated
     * string '"', 4 extra bytes if a dangling ':' must expand to ":null" (only when
     * inside a container with no value yet), and one '}'/']' per open container.
     * Stack depth follows directly from bit position; no bitfield walk needed. */
    int dangling_colon = !data.needs_quote && parent_types.current != (size_t)-1 && comma_ok == 0 && data.windex > data.data_start && data.windex[-1] == ':';
    size_t need_bytes =
        parent_types.bit_level + parent_types.byte_level * sizeof(uint64_t) * CHAR_BIT + (data.needs_quote ? 1u : 0u) + (dangling_colon ? 4u : 0u);
    if ((size_t)(data.buffer_end - data.windex) < need_bytes) {
      uint8_t* old_data_h = NULL;
      size_t old_size_h = 0;
      size_t new_size = (size_t)(data.windex - data.data_start) + need_bytes;
      if (lighter_map_expand(&map, new_size, &old_data_h, &old_size_h) == 0) {
        if (old_data_h) {
          lighter_map_unmap(old_data_h, old_size_h);
        }
        /* The mapping may have moved on Unix; rebase pointers by the relocation delta. */
        ptrdiff_t shift = map.data - data.data_start;
        data.data_start += shift;
        data.windex += shift;
        data.lindex += shift;
        data.rindex += shift;
        data.data_end = map.data + new_size;
        data.buffer_end = map.data + new_size;
        map_capacity = new_size;
      }
    }
    finalize_closures(&data, &parent_types, comma_ok);
  }
  if (parent_types.bits != &parent_types.initial_bits) {
    free(parent_types.bits);
  }
  if (ctx->newlines == 1 && data.windex > data.data_start && *(data.windex - 1) == '\n') {
    --(data.windex); /* clean up trailing newline in -n mode */
  }

  size_t written = (size_t)(data.windex - data.data_start);
  if (!ctx->force_utf8_output && orig_encoding != LIGHTER_ENC_UTF8 && written > 0) {
    /* Fast path: minified output is pure ASCII (very common — JSON minifier
     * strips whitespace and unicode-escapes anything non-ASCII unless the
     * input had raw multibyte UTF-8 characters in strings). For ASCII output,
     * each input byte expands to a fixed-width zero-padded value in the
     * destination encoding — vectorizable as a widening write.
     *
     * data.saw_non_ascii was OR'd by the string parser whenever any non-ASCII
     * byte was written, so checking it is free vs. rescanning the buffer. */
    if (!data.saw_non_ascii) {
      size_t back_size;
      int is_le;
      if (orig_encoding == LIGHTER_ENC_UTF32LE || orig_encoding == LIGHTER_ENC_UTF32BE) {
        back_size = written * 4;
        is_le = (orig_encoding == LIGHTER_ENC_UTF32LE);
        if (back_size > map_capacity) {
          fprintf(stderr, "Could not re-transcode %s in place\n", filename);
          lighter_map_close(&map);
          return EXIT_FAILURE;
        }
        lighter_transcode_utf8_ascii_to_utf32(data.data_start, written, map.data + back_size, is_le);
      } else {
        back_size = written * 2;
        is_le = (orig_encoding == LIGHTER_ENC_UTF16LE);
        if (back_size > map_capacity) {
          fprintf(stderr, "Could not re-transcode %s in place\n", filename);
          lighter_map_close(&map);
          return EXIT_FAILURE;
        }
        lighter_transcode_utf8_ascii_to_utf16(data.data_start, written, map.data + back_size, is_le);
      }
      written = back_size;
    } else {
      size_t back_size = lighter_transcode_from_utf8_size(data.data_start, written, orig_encoding);
      if ((written > back_size ? written : back_size) > map_capacity) {
        fprintf(stderr, "Could not re-transcode %s in place\n", filename);
        lighter_map_close(&map);
        return EXIT_FAILURE;
      }
      /* BOM is stripped; transcode preserves the original byte order. Byte order is
       * unambiguously inferable from null-byte patterns for JSON whose first char is ASCII. */
      lighter_transcode_from_utf8_backward(data.data_start, written, orig_encoding, map.data + back_size);
      written = back_size;
    }
  }

  if (written > 0 && lighter_map_sync(&map, written, ctx->async_io) != 0) {
    fprintf(stderr, "Could not sync file\n");
    exit_code = EXIT_FAILURE;
  }
  /* Always truncate (even to 0) so that any reserved closure headroom doesn't
   * leak into the output as null/garbage bytes. */
  if (exit_code == EXIT_SUCCESS && lighter_map_truncate(&map, written) != 0) {
    fprintf(stderr, "Could not truncate file. It may have garbage at the end\n");
  }

  lighter_map_close(&map);
  if (!ctx->quiet) {
    /* Single fwrite is the only output guaranteed-atomic across threads on
     * POSIX (writes <= PIPE_BUF / page boundary are atomic in practice). Build
     * the whole "filename: Saved N bytes\n" line first, then emit it once. */
    unsigned long saved = (orig_file_size > written) ? (unsigned long)(orig_file_size - written) : 0ul;
    char line[2048];
    int n = snprintf(line, sizeof(line), "%s: Saved %lu bytes\n", filename, saved);
    if (n > 0) {
      if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
      }
      fwrite(line, 1, (size_t)n, stdout);
    }
  }
  return exit_code;
}

/** Run do_file on a heap-allocated path; free the path. Used as the body of
 * each OpenMP task spawned by the directory walker, and called inline in the
 * sequential build. The OR-aggregation into *exit_code is done unconditionally:
 * on the parallel path each task writes only on failure (rare), and OpenMP's
 * task-completion serialization makes the unsynchronized OR safe in practice.
 * For strict correctness without OpenMP atomics, wrap the assignment in
 * `#pragma omp critical` — measured cost is below noise. */
static void process_one_file(Context* ctx, char* path, int* exit_code) {
  if (do_file(ctx, path) != EXIT_SUCCESS) {
#if defined(_OPENMP)
  #pragma omp atomic write
#endif
    *exit_code = EXIT_FAILURE;
  }
  free(path);
}

#if LIGHTER_PLATFORM_WIN
/** Recursively walk a directory on Windows, spawning an OpenMP task per file. */
static int walk_dir_win(Context* ctx, PathBuffer* pb, int* exit_code_out) {
  size_t plen = strlen(pb->buf);
  wchar_t* long_wpath = lighter_make_long_path_w(pb->buf);
  if (!long_wpath) {
    return EXIT_FAILURE;
  }

  size_t wplen = wcslen(long_wpath);
  wchar_t* search_path = (wchar_t*)malloc((wplen + 3) * sizeof(wchar_t));
  if (!search_path) {
    free(long_wpath);
    return EXIT_FAILURE;
  }
  memcpy(search_path, long_wpath, (wplen + 1) * sizeof(wchar_t));

  if (wplen > 0 && search_path[wplen - 1] != L'\\' && search_path[wplen - 1] != L'/') {
    search_path[wplen] = L'\\';
    search_path[wplen + 1] = L'*';
    search_path[wplen + 2] = L'\0';
  } else {
    search_path[wplen] = L'*';
    search_path[wplen + 1] = L'\0';
  }
  free(long_wpath);

  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(search_path, &fd);
  free(search_path);

  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Could not open %s\n", pb->buf);
    return EXIT_FAILURE;
  }
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
      continue;
    }

    int ulen = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, NULL, 0, NULL, NULL);
    if (ulen <= 0) {
      continue;
    }

    if (plen + ulen + 2 > pb->cap) {
      size_t new_cap = (plen + ulen + 2) * 2;
      char* new_buf = (char*)realloc(pb->buf, new_cap);
      if (!new_buf) {
        fprintf(stderr, "Out of memory\n");
        FindClose(h);
        return EXIT_FAILURE;
      }
      pb->buf = new_buf;
      pb->cap = new_cap;
    }

    if (plen > 0 && pb->buf[plen - 1] != '\\' && pb->buf[plen - 1] != '/') {
      pb->buf[plen] = '\\';
      WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, pb->buf + plen + 1, ulen, NULL, NULL);
    } else {
      WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, pb->buf + plen, ulen, NULL, NULL);
    }

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (walk_dir_win(ctx, pb, exit_code_out) != EXIT_SUCCESS) {
        *exit_code_out = EXIT_FAILURE;
      }
    } else if (dir_should_process(ctx, pb->buf)) {
      char* path = strdup(pb->buf);
      if (!path) {
        fprintf(stderr, "Out of memory\n");
        FindClose(h);
        return EXIT_FAILURE;
      }
  #if defined(_OPENMP)
    #pragma omp task firstprivate(path, ctx, exit_code_out)
      process_one_file(ctx, path, exit_code_out);
  #else
      process_one_file(ctx, path, exit_code_out);
  #endif
    }
    pb->buf[plen] = '\0';
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return *exit_code_out;
}
#else
/** Recursively walk a directory on POSIX, spawning an OpenMP task per file. */
static int walk_dir(Context* ctx, PathBuffer* pb, int* exit_code_out) {
  DIR* dir = opendir(pb->buf);
  if (!dir) {
    fprintf(stderr, "Could not open %s: %s\n", pb->buf, strerror(errno));
    return EXIT_FAILURE;
  }
  size_t plen = strlen(pb->buf);
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    size_t elen = strlen(entry->d_name);
    if (plen + elen + 2 > pb->cap) {
      size_t new_cap = (plen + elen + 2) * 2;
      char* new_buf = (char*)realloc(pb->buf, new_cap);
      if (!new_buf) {
        fprintf(stderr, "Out of memory\n");
        closedir(dir);
        return EXIT_FAILURE;
      }
      pb->buf = new_buf;
      pb->cap = new_cap;
    }

    if (plen > 0 && pb->buf[plen - 1] != '/') {
      pb->buf[plen] = '/';
      strcpy(pb->buf + plen + 1, entry->d_name);
    } else {
      strcpy(pb->buf + plen, entry->d_name);
    }

    if (entry->d_type == DT_DIR) {
      if (walk_dir(ctx, pb, exit_code_out) != EXIT_SUCCESS) {
        *exit_code_out = EXIT_FAILURE;
      }
    } else if (dir_should_process(ctx, pb->buf)) {
      char* path = strdup(pb->buf);
      if (!path) {
        fprintf(stderr, "Out of memory\n");
        closedir(dir);
        return EXIT_FAILURE;
      }
  #if defined(_OPENMP)
    #pragma omp task firstprivate(path, ctx, exit_code_out)
      process_one_file(ctx, path, exit_code_out);
  #else
      process_one_file(ctx, path, exit_code_out);
  #endif
    }
    pb->buf[plen] = '\0';
  }
  closedir(dir);
  return *exit_code_out;
}
#endif

/** Top-level directory dispatch: walk the tree in a single thread, spawning an
 * OpenMP task per matching file so workers can start processing as soon as the
 * first file is found (no upfront list, no batched walk latency). NFC tables
 * load lazily and thread-safely on the first non-ASCII string seen. */
static int do_dir(Context* ctx, PathBuffer* pb) {
  int exit_code = EXIT_SUCCESS;
#if defined(_OPENMP)
  #pragma omp parallel
  {
  #pragma omp single
    {
#endif
#if LIGHTER_PLATFORM_WIN
      walk_dir_win(ctx, pb, &exit_code);
#else
  walk_dir(ctx, pb, &exit_code);
#endif
#if defined(_OPENMP)
    }
  }
#endif
  return exit_code;
}

/** Print command-line usage and exit with status. */
void usage(char progname[], int status) {
  fprintf(status == EXIT_SUCCESS ? stdout : stderr,
          "Usage: %s [options] path\n"
          "JSON minifier\n"
          "Options:\n"
          "  -p N Numeric precision (number of decimal places; can be negative)\n"
          "  -0   Preserve negative zero (e.g. \"-0\" stays \"-0\")\n"
          "  -8   Force UTF-8 output\n"
          "  -n   Process NDJSON/JSON Lines\n"
          "  -N   Process NDJSON, preserving empty lines\n"
          "  -s   Block until writes are flushed to disk (default: async)\n"
          "  -U   Disable Unicode normalization\n"
          "  -q   Suppress output\n",
          progname);
  exit(status);
}

/** Parse command-line options and process the requested file or directory. */
#ifndef LIGHTER_NO_MAIN
int main(int argc, char* argv[]) {
  int negative = 0;
  Context ctx = {
      .precision = LIGHTER_PRECISION_UNLIMITED,
      .quiet = 0,
      .newlines = 0,
      .disable_nfc = 0,
      .force_utf8_output = 0,
      .async_io = 1,
      .preserve_neg_zero = 0,
      /* CPU feature probes run once at startup; cached for every file/number/string. */
      .has_avx2 = lighter_cpu_supports_avx2(),
      .has_neon = lighter_cpu_supports_neon(),
      .has_rvv = lighter_cpu_supports_rvv(),
  };
  char* i;
  int optind_val = 1;

  while (optind_val < argc && argv[optind_val][0] == '-' && argv[optind_val][1]) {
    char* o = argv[optind_val] + 1;
    int skip_rest = 0;
    if (*o == '-') {
      ++o;
      if (!*o) {
        break;
      }
    }
    for (; *o; ++o) {
      switch (*o) {
        case 'h':
        case '?':
          usage(argv[0], EXIT_SUCCESS);
          break;
        case 'q':
          ctx.quiet = 1;
          break;
        case '8':
          ctx.force_utf8_output = 1;
          break;
        case 'n':
          ctx.newlines = 1;
          break;
        case 'N':
          ctx.newlines = 2;
          break;
        case 'U':
          ctx.disable_nfc = 1;
          break;
        case 's':
          ctx.async_io = 0;
          break;
        case '0':
          ctx.preserve_neg_zero = 1;
          break;
        case 'p': {
          if (o[1]) {
            i = o + 1;
          } else if (optind_val + 1 < argc) {
            i = argv[optind_val + 1];
            ++optind_val; /* consume next arg */
          } else {
            usage(argv[0], EXIT_FAILURE);
          }
          negative = 0;
          if (*i == '-') {
            negative = 1;
            ++i;
          }
          ctx.precision = 0;
          for (; *i && ctx.precision < LIGHTER_PRECISION_UNLIMITED; ++i) {
            switch (*i) {
              case '0':
              case '1':
              case '2':
              case '3':
              case '4':
              case '5':
              case '6':
              case '7':
              case '8':
              case '9':
                if (ctx.precision > LIGHTER_PRECISION_UNLIMITED / 10 || (ctx.precision == LIGHTER_PRECISION_UNLIMITED / 10 && *i > '7')) {
                  fprintf(stderr, "Precision limited to %lld\n", (long long)LIGHTER_PRECISION_UNLIMITED);
                  ctx.precision = LIGHTER_PRECISION_UNLIMITED;
                }
                ctx.precision = ctx.precision * 10 + *i - '0';
                break;
              default:
                fprintf(stderr, "Precision must be an integer\n");
                exit(EXIT_FAILURE);
            }
          }
          if (negative) {
            ctx.precision = -ctx.precision;
          }
          skip_rest = 1;
          break;
        }
        default:
          usage(argv[0], EXIT_FAILURE);
      }
      if (skip_rest) {
        break;
      }
    }
    ++optind_val;
  }

  if (argc - optind_val != 1) {
    usage(argv[0], EXIT_FAILURE);
  }

  #if LIGHTER_PLATFORM_WIN
  DWORD att = INVALID_FILE_ATTRIBUTES;
  wchar_t* wpath = lighter_make_long_path_w(argv[optind_val]);
  if (wpath) {
    att = GetFileAttributesW(wpath);
    free(wpath);
  }
  if (att != INVALID_FILE_ATTRIBUTES && (att & FILE_ATTRIBUTE_DIRECTORY)) {
    PathBuffer pb;
    pb.cap = LIGHTER_PATH_BUFFER_INITIAL_CAPACITY;
    size_t arg_len = strlen(argv[optind_val]);
    if (arg_len + 1 > pb.cap) {
      pb.cap = arg_len + 1;
    }
    pb.buf = (char*)malloc(pb.cap);
    if (!pb.buf) {
      return EXIT_FAILURE;
    }
    memcpy(pb.buf, argv[optind_val], arg_len + 1);
    int ret = do_dir(&ctx, &pb);
    free(pb.buf);
    return ret;
  }
  #else
  struct stat sb;
  if (stat(argv[optind_val], &sb) == 0 && (sb.st_mode & S_IFDIR)) {
    PathBuffer pb;
    pb.cap = LIGHTER_PATH_BUFFER_INITIAL_CAPACITY;
    size_t arg_len = strlen(argv[optind_val]);
    if (arg_len + 1 > pb.cap) {
      pb.cap = arg_len + 1;
    }
    pb.buf = (char*)malloc(pb.cap);
    if (!pb.buf) {
      return EXIT_FAILURE;
    }
    memcpy(pb.buf, argv[optind_val], arg_len + 1);
    int ret = do_dir(&ctx, &pb);
    free(pb.buf);
    return ret;
  }
  #endif
  return do_file(&ctx, argv[optind_val]);
}
#endif /* LIGHTER_NO_MAIN */
