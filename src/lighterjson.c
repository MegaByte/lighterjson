/**
 * @file      lighterjson.c
 * @brief     JSON minifier
 * @author    Aaron Kaluszka
 * @version   2.0.0
 * @date      11 Apr 2026
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

typedef struct Context {
  int64_t precision;
  int quiet;
  int newlines;
  int disable_nfc;
  int async_io;
  int safe_mode;
  int has_avx512;
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
#define LIGHTER_BOUNDARY_BASE 0x09u
#define LIGHTER_BOUNDARY_MASK_BITS 64u

/** Advance run past a contiguous whitespace span, using SIMD when worthwhile. */
static inline uint8_t* skip_whitespace_impl(uint8_t* run, uint8_t* end, int include_newline, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  (void)has_avx512;
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
  if (has_avx512) {
    __m512i spaces = _mm512_set1_epi8(' ');
    __m512i tabs = _mm512_set1_epi8('\t');
    __m512i crs = _mm512_set1_epi8('\r');
    __m512i lfs = _mm512_set1_epi8('\n');
    while (run + 64 <= end) {
      __m512i chunk = _mm512_loadu_si512((const void*)run);
      __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, spaces) | _mm512_cmpeq_epi8_mask(chunk, tabs) | _mm512_cmpeq_epi8_mask(chunk, crs);
      if (include_newline) {
        mask |= _mm512_cmpeq_epi8_mask(chunk, lfs);
      }
      if (mask != 0xFFFFFFFFFFFFFFFFULL) {
        return run + __builtin_ctzll(~mask);
      }
      run += 64;
    }
  } else if (has_avx2) {
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
        return run + __builtin_ctz(~mask);
      }
      run += 32;
    }
  }
#elif LIGHTER_PLATFORM_ARM64
  if (has_neon) {
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
#elif LIGHTER_PLATFORM_RISCV
  if (has_rvv) {
    while (run < end) {
      size_t n = end - run;
      size_t vl = __riscv_vsetvli(n, __RISCV_E8, __RISCV_M1, __RISCV_TA, __RISCV_MA);
      vuint8m1_t chunk = __riscv_vle8_v_u8m1(run, vl);
      vbool8_t m = __riscv_vmseq_vx_u8m1_b8(chunk, ' ', vl);
      m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\t', vl), vl);
      m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\r', vl), vl);
      if (include_newline) {
        m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\n', vl), vl);
      }
      intptr_t index = __riscv_vfirst_m_b8(__riscv_vmnot_m_b8(m, vl), vl);
      if (index >= 0) {
        return run + index;
      }
      run += vl;
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
  uint8_t* run = skip_whitespace_impl(data->rindex, data->data_end, include_newline, ctx->has_avx512, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
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
  lighter_do_string(data, ctx->disable_nfc, ctx->has_avx512, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
}

/** Dispatch JSON number parsing with the current precision and CPU flags. */
static void do_number(LighterData* data, Context* ctx) {
  lighter_do_number_impl(data, ctx->precision, ctx->has_avx512, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
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
        lighter_write_data(data, 1);
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
        lighter_write_data(data, 1);
    }
  }
}

/** Handle one structural byte reached by the blind value scanner. */
static inline void do_value_handle_byte(LighterData* data, Context* ctx, int* line_start) {
  uint8_t c = *data->rindex;
  if (c == '"') {
    do_string(data, ctx);
  } else if (c == '-' || ((unsigned)(c - '0') <= 9)) {
    do_number(data, ctx);
  } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
    if (c == '\n') {
      if (*line_start == 1) {
        --(*line_start);
      } else if (*line_start == 2) {
        ++(data->rindex);
      } else {
        skip_whitespace_run(data, 1, ctx);
      }
    } else {
      skip_whitespace_run(data, *line_start ? 0 : 1, ctx);
    }
  } else {
    ++(data->rindex);
  }
}

/** Fast-path value scan for non-safe mode. */
static inline void do_value_blind_impl(LighterData* data, Context* ctx, int line_start, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  /* Scan for the next "interesting" byte: whitespace, '"', '-', or a digit.
   * Everything else (structural {}[]:, and literals tfn) is a no-op in the scalar
   * dispatcher, so this loop advances until one of those bytes is found.
   *
   * Membership uses a rebased 64-bit bitmask over the byte range [0x09, 0x39].
   * Whitespace runs are handled separately by skip_whitespace_run. */
  (void)has_avx512;
  (void)has_avx2;
  (void)has_neon;
  (void)has_rvv;
  /* Mask bits for rebased positions (c - 0x09) of each target byte. */
  const uint64_t boundary_mask = (1ULL << ('\t' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('\n' - LIGHTER_BOUNDARY_BASE)) |
                                 (1ULL << ('\r' - LIGHTER_BOUNDARY_BASE)) | (1ULL << (' ' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('"' - LIGHTER_BOUNDARY_BASE)) |
                                 (1ULL << ('-' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('0' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('1' - LIGHTER_BOUNDARY_BASE)) |
                                 (1ULL << ('2' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('3' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('4' - LIGHTER_BOUNDARY_BASE)) |
                                 (1ULL << ('5' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('6' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('7' - LIGHTER_BOUNDARY_BASE)) |
                                 (1ULL << ('8' - LIGHTER_BOUNDARY_BASE)) | (1ULL << ('9' - LIGHTER_BOUNDARY_BASE));
  const uint8_t* end = data->data_end;
  while (data->rindex < end) {
    uint8_t* p = data->rindex;
    while (p < end) {
      uint8_t d = (uint8_t)(*p - LIGHTER_BOUNDARY_BASE);
      /* Rebased to the boundary-mask range; wider values fail the bit test
       * naturally because boundary_mask has no bits set above it. Guard against
       * shift-by-large-value (UB for shift >= LIGHTER_BOUNDARY_MASK_BITS). */
      if (d < LIGHTER_BOUNDARY_MASK_BITS && ((boundary_mask >> d) & 1ULL)) {
        break;
      }
      ++p;
    }
    data->rindex = p;
    if (data->rindex >= end) {
      return;
    }
    do_value_handle_byte(data, ctx, &line_start);
  }
}

/** Parse values from the current position, optionally with structural recovery. */
static int do_value(LighterData* data, Context* ctx, int line_start) {
  if (!ctx->safe_mode) {
    do_value_blind_impl(data, ctx, line_start, ctx->has_avx512, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
    return 0;
  }
  Bitfield parent_types;
  init_bits(&parent_types);
  int comma_ok = 0;
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case '"':
        do_string(data, ctx);
        comma_ok = 1;
        break;
      case '{':
        ++(data->rindex);
        push_set_bit(&parent_types);
        do_object(data, ctx, line_start);
        comma_ok = 0;
        break;
      case '}':
        if (parent_types.current == Object) {
          ++(data->rindex);
          pop_bit(&parent_types);
          comma_ok = 1;
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case '[':
        ++(data->rindex);
        push_clear_bit(&parent_types);
        comma_ok = 0;
        break;
      case ']':
        if (parent_types.current == Array) {
          ++(data->rindex);
          pop_bit(&parent_types);
          comma_ok = 1;
        } else {
          lighter_write_data(data, 1);
        }
        break;
      case ',':
        if (comma_ok && parent_types.current != None) {
          ++(data->rindex);
          if (parent_types.current == Object) {
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
        switch (line_start) {
          case 1:
            --line_start;
            /* fall through */
          case 2:
            ++(data->rindex);
            break;
          default:
            skip_whitespace_run(data, 1, ctx);
        }
        break;
      case ' ':
      case '\t':
      case '\r':
        skip_whitespace_run(data, line_start ? 0 : 1, ctx);
        break;
      default: /* invalid */
        lighter_write_data(data, 1);
    }
  }
  if (parent_types.bits != &parent_types.initial_bits) {
    free(parent_types.bits);
  }
  return 0;
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
  if (!ctx->quiet) {
    printf("%s: ", filename);
  }
  LighterEncoding orig_encoding = lighter_detect_encoding(map.data, map.size);
  size_t bom_size = lighter_encoding_bom_size(orig_encoding, map.data, map.size);
  size_t actual_size = map.size - bom_size;
  size_t orig_file_size = map.size; /* preserve for final savings report */

  if (orig_encoding != LIGHTER_ENC_UTF8) {
    size_t max_utf8 = (orig_encoding == LIGHTER_ENC_UTF16LE || orig_encoding == LIGHTER_ENC_UTF16BE)
                          ? (actual_size * LIGHTER_UTF16_TO_UTF8_GROW_NUMERATOR / LIGHTER_UTF16_TO_UTF8_GROW_DENOMINATOR) + LIGHTER_TRANSCODE_SLACK_BYTES
                          : actual_size + LIGHTER_TRANSCODE_SLACK_BYTES;
    uint8_t* old_data = NULL;
    size_t old_size_full = 0;

    if (lighter_map_expand(&map, max_utf8, &old_data, &old_size_full) != 0) {
      fprintf(stderr, "Could not expand file for transcoding\n");
      lighter_map_close(&map);
      return EXIT_FAILURE;
    }

    uint8_t* transcode_src;
    if (old_data) {
      transcode_src = old_data + bom_size;
    } else {
      /* Windows fallback: move to end of new mapping to avoid overlap issues */
      memmove(map.data + max_utf8 - actual_size, map.data + bom_size, actual_size);
      transcode_src = map.data + max_utf8 - actual_size;
    }

    size_t utf8_size;
    lighter_transcode_to_utf8(transcode_src, actual_size, map.data, orig_encoding, &utf8_size);
    map.size = utf8_size;
    map_capacity = max_utf8;

    if (old_data) {
      lighter_map_unmap(old_data, old_size_full);
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
  data.rindex = map.data + bom_size;
  data.lindex = data.rindex;

  int exit_code = EXIT_SUCCESS;
  do_value(&data, ctx, ctx->newlines == 2 ? 2 : 0); /* clean up leading newlines in -N mode */
  if (ctx->newlines) {
    while (data.rindex < data.data_end) {
      do_value(&data, ctx, ctx->newlines);
    }
  }
  lighter_write_data(&data, 0);
  if (ctx->newlines == 1 && data.windex > data.data_start && *(data.windex - 1) == '\n') {
    --(data.windex); /* clean up trailing newline in -n mode */
  }

  size_t written = (size_t)(data.windex - data.data_start);
  if (orig_encoding != LIGHTER_ENC_UTF8 && written > 0) {
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

  if (written > 0 && lighter_map_sync(&map, written, ctx->async_io) != 0) {
    fprintf(stderr, "Could not sync file\n");
    exit_code = EXIT_FAILURE;
  }
  if (exit_code == EXIT_SUCCESS && written > 0 && lighter_map_truncate(&map, written) != 0) {
    fprintf(stderr, "Could not truncate file. It may have garbage at the end\n");
  }

  lighter_map_close(&map);
  if (!ctx->quiet && orig_file_size > written) {
    printf("Saved %lu bytes\n", (unsigned long)(orig_file_size - written));
  } else if (!ctx->quiet) {
    printf("Saved 0 bytes\n");
  }
  return exit_code;
}

#if LIGHTER_PLATFORM_WIN
/** Recursively process one directory on Windows. */
static int do_dir_win(Context* ctx, PathBuffer* pb) {
  int exit_code = EXIT_SUCCESS;
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
  wcscpy(search_path, long_wpath);

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
      if (do_dir_win(ctx, pb) != EXIT_SUCCESS) {
        exit_code = EXIT_FAILURE;
      }
    } else if (dir_should_process(ctx, pb->buf)) {
      if (do_file(ctx, pb->buf) != EXIT_SUCCESS) {
        exit_code = EXIT_FAILURE;
      }
    }
    pb->buf[plen] = '\0';
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return exit_code;
}
/** Dispatch directory processing through the Windows walker. */
static int do_dir(Context* ctx, PathBuffer* pb) {
  return do_dir_win(ctx, pb);
}
#else
/** Recursively process one directory on POSIX platforms. */
static int do_dir(Context* ctx, PathBuffer* pb) {
  int exit_code = EXIT_SUCCESS;
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
      if (do_dir(ctx, pb) != EXIT_SUCCESS) {
        exit_code = EXIT_FAILURE;
      }
    } else if (dir_should_process(ctx, pb->buf)) {
      if (do_file(ctx, pb->buf) != EXIT_SUCCESS) {
        exit_code = EXIT_FAILURE;
      }
    }
    pb->buf[plen] = '\0';
  }
  closedir(dir);
  return exit_code;
}
#endif

/** Print command-line usage and exit with status. */
void usage(char progname[], int status) {
  fprintf(status == EXIT_SUCCESS ? stdout : stderr,
          "Usage: %s [options] path\n"
          "JSON minifier\n"
          "Options:\n"
          "  -p N Numeric precision (number of decimal places; can be negative)\n"
          "  -n   Process NDJSON/JSON Lines\n"
          "  -N   Process NDJSON, preserving empty lines\n"
          "  -a   Use asynchronous memory mapped I/O\n"
          "  -U   Disable Unicode normalization\n"
          "  -s   Safe mode. Enable strict structural tracking to gracefully parse broken streams\n"
          "  -q   Suppress output\n",
          progname);
  exit(status);
}

/** Parse command-line options and process the requested file or directory. */
int main(int argc, char* argv[]) {
  int negative = 0;
  Context ctx = {
      .precision = LIGHTER_PRECISION_UNLIMITED,
      .quiet = 0,
      .newlines = 0,
      .disable_nfc = 0,
      .async_io = 0,
      .safe_mode = 0,
      /* CPU feature probes run once at startup; cached for every file/number/string. */
      .has_avx512 = lighter_cpu_supports_avx512bw(),
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
          ctx.safe_mode = 1;
          break;
        case 'a':
          ctx.async_io = 1;
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
    strcpy(pb.buf, argv[optind_val]);
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
    strcpy(pb.buf, argv[optind_val]);
    int ret = do_dir(&ctx, &pb);
    free(pb.buf);
    return ret;
  }
#endif
  return do_file(&ctx, argv[optind_val]);
}
