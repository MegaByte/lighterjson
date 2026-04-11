/**
 * @file   lighter_string.h
 * @brief  JSON string parsing and NFC normalization (self-contained header).
 */

#ifndef LIGHTER_STRING_H
#define LIGHTER_STRING_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lighter_common.h"
#include "unicode_nfc_runtime.h"
#include "unicode_nfc_shared.h"

/* \uXXXX = 4 hex digits; UTF-8 and surrogate boundaries (Unicode). */
#define UNICODE_ESCAPE_HEX_LEN 4
#define UTF8_ASCII_MAX 0x80u
#define UTF8_2BYTE_MAX 0x800u
#define UTF8_3BYTE_MAX 0x10000u
#define SURROGATE_HIGH_START 0xD800u
#define SURROGATE_LOW_START 0xDC00u
#define SURROGATE_MASK 0x3FFu
#define SURROGATE_OFFSET 0x10000u

static inline uint64_t lighter_string_hex_value(LighterData* data) {
  uint64_t value = 0;
  if (data->rindex + UNICODE_ESCAPE_HEX_LEN > data->data_end) {
    return (uint64_t)INT64_MAX;
  }
  for (size_t i = 0; i < UNICODE_ESCAPE_HEX_LEN; ++i) {
    const uint64_t x = data->rindex[i];
    const uint64_t shift = (UNICODE_ESCAPE_HEX_LEN - 1 - i) << 2;
    if (x >= '0' && x <= '9') {
      value += ((x - '0') << shift);
    } else if (x >= 'A' && x <= 'F') {
      value += ((x - 'A' + 10) << shift);
    } else if (x >= 'a' && x <= 'f') {
      value += ((x - 'a' + 10) << shift);
    } else {
      return (uint64_t)INT64_MAX;
    }
  }
  return value;
}

static inline void lighter_string_do_unicode(LighterData* data) {
  uint64_t value = lighter_string_hex_value(data);
  uint64_t value2 = 0;
  if (value == (uint64_t)INT64_MAX) {
    fprintf(stderr, "INVALID HEX\n");
    return;
  }
  if (value < 0x20) { /* C0 controls */
    *data->windex++ = '\\';
    switch ((unsigned)value) {
      case '\b':
        *data->windex++ = 'b';
        break;
      case '\f':
        *data->windex++ = 'f';
        break;
      case '\n':
        *data->windex++ = 'n';
        break;
      case '\r':
        *data->windex++ = 'r';
        break;
      case '\t':
        *data->windex++ = 't';
        break;
      default:
        --(data->lindex);
        data->rindex += 4;
        return;
    }
    lighter_write_data(data, UNICODE_ESCAPE_HEX_LEN);
    return;
  }
  lighter_write_data(data, UNICODE_ESCAPE_HEX_LEN);
  if (value < UTF8_ASCII_MAX) {
    *data->windex++ = (uint8_t)value;
    return;
  }
  if (value < UTF8_2BYTE_MAX) {
    *data->windex++ = ((uint8_t)(value >> 6) & 0x1F) | 0xC0;
    *data->windex++ = ((uint8_t)(value & 0x3F)) | 0x80;
    return;
  }
  if (value >= SURROGATE_HIGH_START) { /* possible high surrogate; check for pair */
    value2 = lighter_string_hex_value(data);
    if (value2 == (uint64_t)INT64_MAX) {
      fprintf(stderr, "INVALID HEX\n");
      return;
    }
    if (value2 >= SURROGATE_LOW_START && value2 <= SURROGATE_LOW_START + SURROGATE_MASK) { /* low surrogate */
      value = (((value & SURROGATE_MASK) << 10) | (value2 & SURROGATE_MASK)) + SURROGATE_OFFSET;
    }
    lighter_write_data(data, UNICODE_ESCAPE_HEX_LEN);
  }
  if (value < UTF8_3BYTE_MAX) {
    *data->windex++ = ((uint8_t)(value >> 12) & 0xF) | 0xE0;
    *data->windex++ = (uint8_t)(value >> 6) & 0x3F;
    *data->windex++ = ((uint8_t)value & 0x3F) | 0x80;
  } else {
    *data->windex++ = ((uint8_t)(value >> 18) & 0x7) | 0xF0;
    *data->windex++ = ((uint8_t)(value >> 12) & 0x3F) | 0x80;
    *data->windex++ = ((uint8_t)(value >> 6) & 0x3F) | 0x80;
    *data->windex++ = ((uint8_t)value & 0x3F) | 0x80;
  }
}

static inline void lighter_string_do_escape(LighterData* data) {
  if (data->rindex + 1 < data->data_end) {
    switch (data->rindex[1]) {
      case 'u':
        lighter_write_data(data, 2); /* \u */
        lighter_string_do_unicode(data);
        break;
      case '"':
      case '\\':
      case '/':
      case 'b':
      case 'f':
      case 'n':
      case 'r':
      case 't':
        data->rindex += 2;
        break;
      default:
        lighter_write_data(data, 1);
        ++(data->rindex);
    }
  }
}

#include "lighter_cpu.h"

static inline void lighter_do_string_impl(LighterData* data, int disable_nfc, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  uint8_t* start = data->lindex;
  uint8_t* run = data->rindex;
  uint8_t* end = data->data_end;

  while (run < end) {
    uint8_t* p = run;
    #if LIGHTER_PLATFORM_X86
      if (has_avx512) {
        __m512i quotes = _mm512_set1_epi8('"');
        __m512i backslashes = _mm512_set1_epi8('\\');
        while (p + 64 <= end) {
          __m512i chunk = _mm512_loadu_si512((const void*)p);
          __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, quotes) | _mm512_cmpeq_epi8_mask(chunk, backslashes);
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
        __m256i quotes = _mm256_set1_epi8('"');
        __m256i backslashes = _mm256_set1_epi8('\\');
        while (p + 32 <= end) {
          __m256i chunk = _mm256_loadu_si256((const __m256i*)p);
          __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(chunk, quotes), _mm256_cmpeq_epi8(chunk, backslashes));
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
        uint8x16_t quotes = vdupq_n_u8('"');
        uint8x16_t backslashes = vdupq_n_u8('\\');
        while (p + 16 <= end) {
          uint8x16_t chunk = vld1q_u8(p);
          uint8x16_t m = vorrq_u8(vceqq_u8(chunk, quotes), vceqq_u8(chunk, backslashes));
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
        while (p < end) {
          size_t n = end - p;
          size_t vl = __riscv_vsetvli(n, __RISCV_E8, __RISCV_M1, __RISCV_TA, __RISCV_MA);
          vuint8m1_t chunk = __riscv_vle8_v_u8m1(p, vl);
          vbool8_t m = __riscv_vmseq_vx_u8m1_b8(chunk, '"', vl);
          m = __riscv_vmor_mm_b8(m, __riscv_vmseq_vx_u8m1_b8(chunk, '\\', vl), vl);
          intptr_t index = __riscv_vfirst_m_b8(m, vl);
          if (index >= 0) {
            p += index;
            break;
          }
          p += vl;
        }
      }
    #endif
    run = p;
    if (run >= end) break;

    switch (*run) {
      case '\\':
        data->rindex = run;
        lighter_string_do_escape(data);
        run = data->rindex;
        break;
      case '"': {
        if (!disable_nfc && nfc_quick_check("lighter.nfc", start + 1, run, has_avx512, has_avx2, has_neon, has_rvv) != NFC_QC_YES) {
          data->rindex = run;
          lighter_write_data(data, 0);
          uint8_t* content_start = data->windex - (run - start) + 1;
          uint8_t* content_end = data->windex - 1;
          uint8_t* new_end = nfc_normalize_utf8_incremental(nfc_get_or_load("lighter.nfc"), content_start, content_end);
          memmove(new_end, data->windex - 1, 1);
          data->windex = new_end + 1;
          run = data->rindex;
        } else {
          ++run;
        }
        data->rindex = run;
        return;
      }
      default:
        ++run;
    }
    data->rindex = run;
  }
}

static inline void lighter_do_string(LighterData* data, int disable_nfc, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  uint8_t* start = data->rindex;
  ++(data->rindex);
  if (has_avx512)
    lighter_do_string_impl(data, disable_nfc, 1, 0, 0, 0);
  else if (has_avx2)
    lighter_do_string_impl(data, disable_nfc, 0, 1, 0, 0);
  else if (has_neon)
    lighter_do_string_impl(data, disable_nfc, 0, 0, 1, 0);
  else if (has_rvv)
    lighter_do_string_impl(data, disable_nfc, 0, 0, 0, 1);
  else
    lighter_do_string_impl(data, disable_nfc, 0, 0, 0, 0);
}

#endif /* LIGHTER_STRING_H */
