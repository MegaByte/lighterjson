/**
 * @file      lighterjson.c
 * @brief     JSON minifier
 * @author    Aaron Kaluszka
 * @version   1.0.0
 * @date      21 Feb 2026
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
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>

  #include "lighter_transcode.h"
#endif

#include "lighter_bitfield.h"
#include "lighter_common.h"
#include "lighter_cpu.h"
#include "lighter_memmap.h"
#include "lighter_number.h"
#include "lighter_string.h"

typedef struct LighterContext {
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
} LighterContext;

typedef struct PathBuffer {
  char* buf;
  size_t cap;
} PathBuffer;

static int do_file(LighterContext* ctx, char filename[]);
static int do_dir(LighterContext* ctx, PathBuffer* pb);

static inline uint8_t* skip_whitespace_impl(uint8_t* run, uint8_t* end, int include_newline, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  #if LIGHTER_PLATFORM_X86
    if (has_avx512) {
      __m512i spaces = _mm512_set1_epi8(' ');
      __m512i tabs = _mm512_set1_epi8('\t');
      __m512i crs = _mm512_set1_epi8('\r');
      __m512i lfs = _mm512_set1_epi8('\n');
      while (run + 64 <= end) {
        __m512i chunk = _mm512_loadu_si512((const void*)run);
        __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, spaces) | _mm512_cmpeq_epi8_mask(chunk, tabs) | _mm512_cmpeq_epi8_mask(chunk, crs);
        if (include_newline)
          mask |= _mm512_cmpeq_epi8_mask(chunk, lfs);
        if (mask != 0xFFFFFFFFFFFFFFFFULL) {
          #if defined(_MSC_VER)
            unsigned long offset;
            _BitScanForward64(&offset, ~mask);
            return run + offset;
          #else
            return run + __builtin_ctzll(~mask);
          #endif
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
        if (include_newline)
          m = _mm256_or_si256(m, _mm256_cmpeq_epi8(chunk, lfs));
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
        if (include_newline)
          m = vorrq_u8(m, vceqq_u8(chunk, lfs));
        uint64x2_t u64 = vreinterpretq_u64_u8(m);
        uint64_t low = vgetq_lane_u64(u64, 0);
        uint64_t high = vgetq_lane_u64(u64, 1);
        if (low != 0xFFFFFFFFFFFFFFFFULL) {
          #if defined(_MSC_VER)
            unsigned long offset;
            _BitScanForward64(&offset, ~low);
            return run + (offset >> 3);
          #else
            return run + (__builtin_ctzll(~low) >> 3);
          #endif
        } else if (high != 0xFFFFFFFFFFFFFFFFULL) {
          #if defined(_MSC_VER)
            unsigned long offset;
            _BitScanForward64(&offset, ~high);
            return run + (offset >> 3) + 8;
          #else
            return run + (__builtin_ctzll(~high) >> 3) + 8;
          #endif
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
        if (include_newline)
          m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\n', vl), vl);
        intptr_t index = __riscv_vfirst_m_b8(__riscv_vmnot_m_b8(m, vl), vl);
        if (index >= 0)
          return run + index;
        run += vl;
      }
    }
  #endif

  if (include_newline) {
    while (run < end && (*run == ' ' || *run == '\t' || *run == '\n' || *run == '\r'))
      ++run;
  } else {
    while (run < end && (*run == ' ' || *run == '\t' || *run == '\r'))
      ++run;
  }
  return run;
}

void skip_whitespace_run(LighterData* data, int include_newline, LighterContext* ctx) {
  uint8_t* run = data->rindex;
  uint8_t* end = data->data_end;

#if LIGHTER_PLATFORM_X86
  if (ctx->has_avx512)
    run = skip_whitespace_impl(run, end, include_newline, 1, 0, 0, 0);
  else if (ctx->has_avx2)
    run = skip_whitespace_impl(run, end, include_newline, 0, 1, 0, 0);
#elif LIGHTER_PLATFORM_ARM64
  if (ctx->has_neon)
    run = skip_whitespace_impl(run, end, include_newline, 0, 0, 1, 0);
#elif LIGHTER_PLATFORM_RISCV
  if (ctx->has_rvv)
    run = skip_whitespace_impl(run, end, include_newline, 0, 0, 0, 1);
#endif
  else
    run = skip_whitespace_impl(run, end, include_newline, 0, 0, 0, 0);

  if (include_newline) {
    while (run < end && (*run == ' ' || *run == '\t' || *run == '\n' || *run == '\r'))
      ++run;
  } else {
    while (run < end && (*run == ' ' || *run == '\t' || *run == '\r'))
      ++run;
  }
  lighter_write_data(data, run - data->rindex);
}

void do_literal(LighterData* data, const char* literal, size_t length) {
  if (strncmp((char*)data->rindex, literal, length)) {
    lighter_write_data(data, length);
  } else {
    data->rindex += length;
  }
}

static void do_string(LighterData* data, LighterContext* ctx) {
  lighter_do_string(data, ctx->disable_nfc, ctx->has_avx512, ctx->has_avx2, ctx->has_neon, ctx->has_rvv);
}

static int do_object_label(LighterData* data, LighterContext* ctx, int line_start) {
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case '"':
        do_string(data, ctx);
        return 0;
      case '}':
        return 1;
      case '\n':
        if (line_start) {
          /* NDJSON: newline inside object = end of line; stop without consuming */
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

static void do_object(LighterData* data, LighterContext* ctx, int line_start) {
  if (do_object_label(data, ctx, line_start)) {
    return;
  }
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case ':':
        lighter_write_data(data, 0); /* copy any pending (key or whitespace) */
        ++(data->rindex);
        lighter_write_data(data, 0); /* copy colon so we never advance past what we copy */
        return;
      case '\n':
        if (line_start) {
          /* NDJSON: newline before colon = end of line; stop without consuming */
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

static void do_number(LighterData* data, LighterContext* ctx) {
  lighter_do_number(data, ctx->precision);
}

static inline void do_value_blind_impl(LighterData* data, LighterContext* ctx, int line_start, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  while (data->rindex < data->data_end) {
    uint8_t* p = data->rindex;
    #if LIGHTER_PLATFORM_X86
      if (has_avx512) {
        __m512i spaces = _mm512_set1_epi8(' ');
        __m512i tabs = _mm512_set1_epi8('\t');
        __m512i crs = _mm512_set1_epi8('\r');
        __m512i lfs = _mm512_set1_epi8('\n');
        __m512i quotes = _mm512_set1_epi8('"');
        __m512i minus = _mm512_set1_epi8('-');
        while (p + 64 <= data->data_end) {
          __m512i chunk = _mm512_loadu_si512((const void*)p);
          __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, spaces) | _mm512_cmpeq_epi8_mask(chunk, tabs) | _mm512_cmpeq_epi8_mask(chunk, crs) |
                           _mm512_cmpeq_epi8_mask(chunk, lfs) | _mm512_cmpeq_epi8_mask(chunk, quotes) | _mm512_cmpeq_epi8_mask(chunk, minus) |
                           (_mm512_cmpge_epi8_mask(chunk, _mm512_set1_epi8('0')) & _mm512_cmple_epi8_mask(chunk, _mm512_set1_epi8('9')));
          if (mask != 0) {
            #if defined(_MSC_VER)
              unsigned long offset;
              _BitScanForward64(&offset, mask);
              p += offset;
            #else
              p += __builtin_ctzll(mask);
            #endif
            break;
          }
          p += 64;
        }
      } else if (has_avx2) {
        __m256i spaces = _mm256_set1_epi8(' ');
        __m256i tabs = _mm256_set1_epi8('\t');
        __m256i crs = _mm256_set1_epi8('\r');
        __m256i lfs = _mm256_set1_epi8('\n');
        __m256i quotes = _mm256_set1_epi8('"');
        __m256i minus = _mm256_set1_epi8('-');
        while (p + 32 <= data->data_end) {
          __m256i chunk = _mm256_loadu_si256((const __m256i*)p);
          __m256i m = _mm256_or_si256(
              _mm256_cmpeq_epi8(chunk, spaces),
              _mm256_or_si256(_mm256_cmpeq_epi8(chunk, tabs),
                              _mm256_or_si256(_mm256_cmpeq_epi8(chunk, crs),
                                              _mm256_or_si256(_mm256_cmpeq_epi8(chunk, lfs),
                                                              _mm256_or_si256(_mm256_cmpeq_epi8(chunk, quotes), _mm256_cmpeq_epi8(chunk, minus))))));
          __m256i digits = _mm256_and_si256(_mm256_cmpgt_epi8(chunk, _mm256_set1_epi8('0' - 1)), _mm256_cmpgt_epi8(_mm256_set1_epi8('9' + 1), chunk));
          m = _mm256_or_si256(m, digits);
          uint32_t mask = (uint32_t)_mm256_movemask_epi8(m);
          if (mask != 0) {
            #if defined(_MSC_VER)
              unsigned long offset;
              _BitScanForward(&offset, mask);
              p += offset;
            #else
              p += __builtin_ctz(mask);
            #endif
            break;
          }
          p += 32;
        }
      }
    #elif LIGHTER_PLATFORM_ARM64
      if (has_neon) {
        uint8x16_t spaces = vdupq_n_u8(' ');
        uint8x16_t tabs = vdupq_n_u8('\t');
        uint8x16_t crs = vdupq_n_u8('\r');
        uint8x16_t lfs = vdupq_n_u8('\n');
        uint8x16_t quotes = vdupq_n_u8('"');
        uint8x16_t minus = vdupq_n_u8('-');
        while (p + 16 <= data->data_end) {
          uint8x16_t chunk = vld1q_u8(p);
          uint8x16_t m =
              vorrq_u8(vceqq_u8(chunk, spaces),
                       vorrq_u8(vceqq_u8(chunk, tabs),
                                vorrq_u8(vceqq_u8(chunk, crs), vorrq_u8(vceqq_u8(chunk, lfs), vorrq_u8(vceqq_u8(chunk, quotes), vceqq_u8(chunk, minus))))));
          uint8x16_t digits = vandq_u8(vcgeq_u8(chunk, vdupq_n_u8('0')), vcleq_u8(chunk, vdupq_n_u8('9')));
          m = vorrq_u8(m, digits);
          uint64x2_t u64 = vreinterpretq_u64_u8(m);
          uint64_t low = vgetq_lane_u64(u64, 0);
          uint64_t high = vgetq_lane_u64(u64, 1);
          if (low != 0) {
            #if defined(_MSC_VER)
              unsigned long offset;
              _BitScanForward64(&offset, low);
              p += (offset >> 3);
            #else
              p += (__builtin_ctzll(low) >> 3);
            #endif
            break;
          } else if (high != 0) {
            #if defined(_MSC_VER)
              unsigned long offset;
              _BitScanForward64(&offset, high);
              p += (offset >> 3) + 8;
            #else
              p += (__builtin_ctzll(high) >> 3) + 8;
            #endif
            break;
          }
          p += 16;
        }
      }
    #elif LIGHTER_PLATFORM_RISCV
      if (has_rvv) {
        while (p < data->data_end) {
          size_t n = data->data_end - p;
          size_t vl = __riscv_vsetvli(n, __RISCV_E8, __RISCV_M1, __RISCV_TA, __RISCV_MA);
          vuint8m1_t chunk = __riscv_vle8_v_u8m1(p, vl);
          vbool8_t m = __riscv_vmseq_vx_u8m1_b8(chunk, ' ', vl);
          m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\t', vl), vl);
          m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\r', vl), vl);
          m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\n', vl), vl);
          m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '"', vl), vl);
          m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '-', vl), vl);
          vbool8_t digits = __riscv_vmand_mm_b8(__riscv_vmsgeu_vx_u8m1_b8(chunk, '0', vl), __riscv_vmsleu_vx_u8m1_b8(chunk, '9', vl), vl);
          m = __riscv_vmor_mm_b8(m, digits, vl);
          intptr_t index = __riscv_vfirst_m_b8(m, vl);
          if (index >= 0) {
            p += index;
            break;
          }
          p += vl;
        }
      }
    #endif
    data->rindex = p;
    if (data->rindex >= data->data_end)
      break;

    uint8_t c = *data->rindex;
    if (c == '"') {
      do_string(data, ctx);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      do_number(data, ctx);
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (c == '\n') {
        if (line_start == 1)
          --line_start;
        else if (line_start == 2)
          ++(data->rindex);
        else
          skip_whitespace_run(data, 1, ctx);
      } else {
        skip_whitespace_run(data, line_start ? 0 : 1, ctx);
      }
    } else {
      lighter_write_data(data, 1);
    }
  }
}

static int do_value(LighterData* data, LighterContext* ctx, int line_start) {
  if (!ctx->safe_mode) {
    if (ctx->has_avx512)
      do_value_blind_impl(data, ctx, line_start, 1, 0, 0, 0);
    else if (ctx->has_avx2)
      do_value_blind_impl(data, ctx, line_start, 0, 1, 0, 0);
    else if (ctx->has_neon)
      do_value_blind_impl(data, ctx, line_start, 0, 0, 1, 0);
    else if (ctx->has_rvv)
      do_value_blind_impl(data, ctx, line_start, 0, 0, 0, 1);
    else
      do_value_blind_impl(data, ctx, line_start, 0, 0, 0, 0);
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
            // fallthrough
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
      default:  // invalid
        lighter_write_data(data, 1);
    }
  }
  if (parent_types.bits != &parent_types.initial_bits) {
    free(parent_types.bits);
  }
  return 0;
}

/* Return 1 if filename should be processed: .json always; .jsonl/.ndjson when in NDJSON mode. */
static int dir_should_process(LighterContext* ctx, const char* name) {
  size_t len = strlen(name);
  return (len >= 5 && strcmp(name + len - 5, ".json") == 0) ||
         (ctx->newlines && ((len >= 6 && strcmp(name + len - 6, ".jsonl") == 0) || (len >= 7 && strcmp(name + len - 7, ".ndjson") == 0)));
}

#if LIGHTER_PLATFORM_WIN
static int do_dir_win(LighterContext* ctx, PathBuffer* pb) {
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
      pb->cap = (plen + ulen + 2) * 2;
      pb->buf = (char*)realloc(pb->buf, pb->cap);
    }

    if (plen > 0 && pb->buf[plen - 1] != '\\' && pb->buf[plen - 1] != '/') {
      pb->buf[plen] = '\\';
      WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, pb->buf + plen + 1, ulen, NULL, NULL);
    } else {
      WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, pb->buf + plen, ulen, NULL, NULL);
    }

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      do_dir_win(ctx, pb);
    } else if (dir_should_process(ctx, pb->buf)) {
      do_file(ctx, pb->buf);
    }
    pb->buf[plen] = '\0';
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return EXIT_SUCCESS;
}
static int do_dir(LighterContext* ctx, PathBuffer* pb) {
  return do_dir_win(ctx, pb);
}
#else
static int do_dir(LighterContext* ctx, PathBuffer* pb) {
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
      pb->cap = (plen + elen + 2) * 2;
      pb->buf = (char*)realloc(pb->buf, pb->cap);
    }

    if (plen > 0 && pb->buf[plen - 1] != '/') {
      pb->buf[plen] = '/';
      strcpy(pb->buf + plen + 1, entry->d_name);
    } else {
      strcpy(pb->buf + plen, entry->d_name);
    }

    if (entry->d_type == DT_DIR) {
      do_dir(ctx, pb);
    } else if (dir_should_process(ctx, pb->buf)) {
      do_file(ctx, pb->buf);
    }
    pb->buf[plen] = '\0';
  }
  closedir(dir);
  return EXIT_SUCCESS;
}
#endif

static int do_file(LighterContext* ctx, char filename[]) {
  LighterMap map = {0};
  if (lighter_map_open(&map, filename, 0) != 0) {
    return EXIT_FAILURE;
  }
  int exit_code = EXIT_SUCCESS;
  if (!ctx->quiet) {
    printf("%s: ", filename);
  }

  LighterEncoding orig_enc = lighter_detect_encoding(map.data, map.size);
  size_t orig_size = map.size;
  if (orig_enc != LIGHTER_ENC_UTF8) {
    size_t max_utf8 = (orig_enc == LIGHTER_ENC_UTF16LE || orig_enc == LIGHTER_ENC_UTF16BE) ? (orig_size * 3 / 2) + 4 : orig_size + 4;
    if (max_utf8 > orig_size) {
      lighter_map_truncate(&map, max_utf8);
      lighter_map_close(&map);
      if (lighter_map_open(&map, filename, 0) != 0)
        return EXIT_FAILURE;
      memmove(map.data + max_utf8 - orig_size, map.data, orig_size);
      size_t utf8_size;
      lighter_transcode_to_utf8(map.data + max_utf8 - orig_size, orig_size, map.data, orig_enc, &utf8_size);
      map.size = utf8_size;
    } else {
      size_t utf8_size;
      lighter_transcode_to_utf8(map.data, map.size, map.data, orig_enc, &utf8_size);
      map.size = utf8_size;
    }
  }

  LighterData data;
  data.data_start = data.rindex = data.windex = data.lindex = map.data;
  data.data_end = map.data + map.size;

  ctx->has_avx512 = lighter_cpu_supports_avx512bw();
  ctx->has_avx2 = lighter_cpu_supports_avx2();
  ctx->has_neon = lighter_cpu_supports_neon();
  ctx->has_rvv = lighter_cpu_supports_rvv();
  do_value(&data, ctx, ctx->newlines == 2 ? 2 : 0);
  if (ctx->newlines) {
    while (data.rindex < data.data_end) {
      do_value(&data, ctx, ctx->newlines);
    }
  }
  lighter_write_data(&data, 0);
  if (ctx->newlines == 1 && data.windex > data.data_start && *(data.windex - 1) == '\n') {
    --(data.windex);
  }

  size_t written = (size_t)(data.windex - data.data_start);
  if (orig_enc != LIGHTER_ENC_UTF8 && written > orig_size && (orig_enc == LIGHTER_ENC_UTF16LE || orig_enc == LIGHTER_ENC_UTF16BE)) {
    uint8_t* temp_utf8 = (uint8_t*)malloc(written);
    if (temp_utf8) {
      memcpy(temp_utf8, data.data_start, written);
      size_t back_size;
      lighter_transcode_from_utf8(temp_utf8, written, orig_enc, map.data, &back_size);
      free(temp_utf8);
      written = back_size;
    }
  }

  if (written > 0 && lighter_map_sync(&map, written, ctx->async_io) != 0) {
    fprintf(stderr, "Could not sync file\n");
    exit_code = EXIT_FAILURE;
  }
  if (exit_code == EXIT_SUCCESS && written > 0 && lighter_map_truncate(&map, written) != 0) {
    fprintf(stderr, "Could not truncate file. It may have garbage at the end\n");
  }
  lighter_map_close(&map);
  if (!ctx->quiet && data.data_end > data.data_start) {
    printf("Saved %lu bytes\n", (unsigned long)(data.data_end - data.windex));
  }
  return exit_code;
}

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

int main(int argc, char* argv[]) {
  int negative = 0;
  LighterContext ctx = {
      .precision = LIGHTER_PRECISION_UNLIMITED,
      .quiet = 0,
      .newlines = 0,
      .disable_nfc = 0,
      .async_io = 0,
      .safe_mode = 0,
      .has_avx512 = 0,
      .has_avx2 = 0,
      .has_neon = 0,
      .has_rvv = 0,
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
    pb.cap = 4096;
    size_t arg_len = strlen(argv[optind_val]);
    if (arg_len + 1 > pb.cap)
      pb.cap = arg_len + 1;
    pb.buf = (char*)malloc(pb.cap);
    if (!pb.buf)
      return EXIT_FAILURE;
    strcpy(pb.buf, argv[optind_val]);
    int ret = do_dir(&ctx, &pb);
    free(pb.buf);
    return ret;
  }
#else
  struct stat sb;
  if (stat(argv[optind_val], &sb) == 0 && (sb.st_mode & S_IFDIR)) {
    PathBuffer pb;
    pb.cap = 4096;
    size_t arg_len = strlen(argv[optind_val]);
    if (arg_len + 1 > pb.cap)
      pb.cap = arg_len + 1;
    pb.buf = (char*)malloc(pb.cap);
    if (!pb.buf)
      return EXIT_FAILURE;
    strcpy(pb.buf, argv[optind_val]);
    int ret = do_dir(&ctx, &pb);
    free(pb.buf);
    return ret;
  }
#endif
  return do_file(&ctx, argv[optind_val]);
}
