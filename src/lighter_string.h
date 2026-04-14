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

static const uint8_t lighter_hex_table[64] = {
    0,    1,    2,    3,    4,    5,    6,    7,    8,    9,    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, /* 0-9, :;<=>? */
    0x80, 10,   11,   12,   13,   14,   15,   0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, /* @, A-F, G-O */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, /* P-Z, [\]^_ */
    0x80, 10,   11,   12,   13,   14,   15,   0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80  /* `, a-f, g-o */
};

static inline uint64_t lighter_string_hex_value(LighterData* data) {
  if (data->rindex + UNICODE_ESCAPE_HEX_LEN > data->data_end) {
    return (uint64_t)INT64_MAX;
  }
  const uint8_t x0 = data->rindex[0] - '0';
  const uint8_t x1 = data->rindex[1] - '0';
  const uint8_t x2 = data->rindex[2] - '0';
  const uint8_t x3 = data->rindex[3] - '0';

  if ((x0 | x1 | x2 | x3) & 0xC0) {
    return (uint64_t)INT64_MAX;
  }

  const uint8_t v0 = lighter_hex_table[x0];
  const uint8_t v1 = lighter_hex_table[x1];
  const uint8_t v2 = lighter_hex_table[x2];
  const uint8_t v3 = lighter_hex_table[x3];

  if ((v0 | v1 | v2 | v3) & 0x80) {
    return (uint64_t)INT64_MAX;
  }

  return (uint64_t)((v0 << 12) | (v1 << 8) | (v2 << 4) | v3);
}

static inline void lighter_string_do_unicode(LighterData* data) {
  uint64_t value = lighter_string_hex_value(data);
  if (value == (uint64_t)INT64_MAX) {
    fprintf(stderr, "INVALID HEX\n");
    return;
  }
  data->rindex += UNICODE_ESCAPE_HEX_LEN;
  data->lindex = data->rindex;

  if (value >= SURROGATE_HIGH_START && value <= SURROGATE_HIGH_START + SURROGATE_MASK) {
    /* check for low surrogate \uXXXX */
    if (data->rindex + 2 + UNICODE_ESCAPE_HEX_LEN <= data->data_end && data->rindex[0] == '\\' && data->rindex[1] == 'u') {
      uint8_t* saved_rindex = data->rindex;
      data->rindex += 2;
      uint64_t value2 = lighter_string_hex_value(data);
      if (value2 >= SURROGATE_LOW_START && value2 <= SURROGATE_LOW_START + SURROGATE_MASK) {
        value = (((value & SURROGATE_MASK) << 10) | (value2 & SURROGATE_MASK)) + SURROGATE_OFFSET;
        data->rindex += UNICODE_ESCAPE_HEX_LEN;
        data->lindex = data->rindex;
      } else {
        data->rindex = saved_rindex;
      }
    }
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
        /* Unhandled control character: re-escape as \uXXXX */
        data->windex--; /* remove \ */
        *data->windex++ = '\\';
        *data->windex++ = 'u';
        /* Back-fill hex */
        for (int i = 0; i < 4; ++i) {
          uint8_t h = (uint8_t)((value >> ((3 - i) << 2)) & 0xF);
          *data->windex++ = (h < 10) ? ('0' + h) : ('a' + h - 10);
        }
        break;
    }
  } else if (value < UTF8_ASCII_MAX) {
    if (value == '"' || value == '\\') {
      *data->windex++ = '\\';
    }
    *data->windex++ = (uint8_t)value;
  } else if (value < UTF8_2BYTE_MAX) {
    *data->windex++ = ((uint8_t)(value >> 6) & 0x1F) | 0xC0;
    *data->windex++ = ((uint8_t)(value & 0x3F)) | 0x80;
  } else if (value < UTF8_3BYTE_MAX) {
    *data->windex++ = ((uint8_t)(value >> 12) & 0xF) | 0xE0;
    *data->windex++ = ((uint8_t)(value >> 6) & 0x3F) | 0x80;
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
        lighter_write_data(data, 0); /* flush before \u */
        data->rindex += 2;           /* skip \u */
        data->lindex = data->rindex;
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

#if LIGHTER_PLATFORM_X86
LIGHTER_TARGET_AVX512
static inline void lighter_simd_avx512_string_skip(LighterData* data) {
  while (data->rindex + 64 <= data->data_end) {
    __m512i chunk = _mm512_loadu_si512((const void*)data->rindex);
    __m512i quote = _mm512_set1_epi8('"');
    __m512i escape = _mm512_set1_epi8('\\');

    __mmask64 test_quote = _mm512_cmpeq_epi8_mask(chunk, quote);
    __mmask64 test_escape = _mm512_cmpeq_epi8_mask(chunk, escape);
    __mmask64 test_either = test_quote | test_escape;

    if (test_either == 0) {
      data->rindex += 64;
    } else {
  #if defined(_MSC_VER)
      unsigned long offset;
    #if defined(_M_X64)
      _BitScanForward64(&offset, test_either);
      data->rindex += offset;
    #else
      if ((uint32_t)test_either != 0) {
        _BitScanForward(&offset, (uint32_t)test_either);
        data->rindex += offset;
      } else {
        _BitScanForward(&offset, (uint32_t)(test_either >> 32));
        data->rindex += offset + 32;
      }
    #endif
  #else
      data->rindex += __builtin_ctzll(test_either);
  #endif
      return;
    }
  }
}

LIGHTER_TARGET_AVX2
static inline void lighter_simd_avx2_string_skip(LighterData* data) {
  while (data->rindex + 32 <= data->data_end) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)data->rindex);
    __m256i quote = _mm256_set1_epi8('"');
    __m256i escape = _mm256_set1_epi8('\\');

    __m256i test_quote = _mm256_cmpeq_epi8(chunk, quote);
    __m256i test_escape = _mm256_cmpeq_epi8(chunk, escape);
    __m256i test_either = _mm256_or_si256(test_quote, test_escape);

    uint32_t mask = (uint32_t)_mm256_movemask_epi8(test_either);

    if (mask == 0) {
      data->rindex += 32;
    } else {
  #if defined(_MSC_VER)
      unsigned long offset;
      _BitScanForward(&offset, mask);
      data->rindex += offset;
  #else
      data->rindex += __builtin_ctz(mask);
  #endif
      return;
    }
  }
}
#endif /* LIGHTER_PLATFORM_X86 */
#if LIGHTER_PLATFORM_ARM64
static inline void lighter_simd_neon_string_skip(LighterData* data) {
  while (data->rindex + 16 <= data->data_end) {
    uint8x16_t chunk = vld1q_u8((const uint8_t*)data->rindex);
    uint8x16_t quote = vdupq_n_u8('"');
    uint8x16_t escape = vdupq_n_u8('\\');

    uint8x16_t test_quote = vceqq_u8(chunk, quote);
    uint8x16_t test_escape = vceqq_u8(chunk, escape);
    uint8x16_t test_either = vorrq_u8(test_quote, test_escape);

    uint64x2_t u64 = vreinterpretq_u64_u8(test_either);
    uint64_t low = vgetq_lane_u64(u64, 0);
    uint64_t high = vgetq_lane_u64(u64, 1);

    if (low != 0) {
  #if defined(_MSC_VER)
      unsigned long offset;
      _BitScanForward64(&offset, low);
      data->rindex += (offset >> 3);
  #else
      data->rindex += __builtin_ctzll(low) >> 3;
  #endif
      return;
    } else if (high != 0) {
  #if defined(_MSC_VER)
      unsigned long offset;
      _BitScanForward64(&offset, high);
      data->rindex += (offset >> 3) + 8;
  #else
      data->rindex += (__builtin_ctzll(high) >> 3) + 8;
  #endif
      return;
    }
    data->rindex += 16;
  }
}
#endif

#if LIGHTER_PLATFORM_RISCV
static inline void lighter_simd_rvv_string_skip(LighterData* data) {
  while (data->rindex < data->data_end) {
    size_t n = data->data_end - data->rindex;
    size_t vl = __riscv_vsetvli(n, __RISCV_E8, __RISCV_M1, __RISCV_TA, __RISCV_MA);
    vuint8m1_t chunk = __riscv_vle8_v_u8m1(data->rindex, vl);
    vbool8_t m_quote = __riscv_vmseq_vx_u8m1_b8(chunk, '"', vl);
    vbool8_t m_escape = __riscv_vmseq_vx_u8m1_b8(chunk, '\\', vl);
    vbool8_t mask = __riscv_vmor_mm_b8(m_quote, m_escape, vl);
    intptr_t index = __riscv_vfirst_m_b8(mask, vl);
    if (index >= 0) {
      data->rindex += index;
      return;
    }
    data->rindex += vl;
  }
}
#endif

static inline int lighter_string_tail_at_end(LighterData* data, int disable_nfc, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  if (data->rindex >= data->data_end) {
    return 1;
  }
  switch (*data->rindex) {
    case '\\':
      lighter_string_do_escape(data);
      break;
    case '"': {
      if (!disable_nfc && nfc_quick_check("lighter.nfc", data->lindex + 1, data->rindex, has_avx512, has_avx2, has_neon, has_rvv) != NFC_QC_YES) {
        ptrdiff_t pending = data->rindex - data->lindex;
        lighter_write_data(data, 0);
        uint8_t* str_content_start = data->windex - pending + 1;
        uint8_t* str_content_end = data->windex - 1;
        uint8_t* new_end = nfc_normalize_utf8_incremental(nfc_get_or_load("lighter.nfc"), str_content_start, str_content_end);
        memmove(new_end, data->windex - 1, 1);
        data->windex = new_end + 1;
      } else {
        ++(data->rindex);
      }
      return 1;
    }
    default:
      ++(data->rindex);
  }
  return 0;
}

/** Parse and optionally NFC-normalize a JSON string. NFC data is loaded on first use
 *  when a string ends. Uses lighter_write_data to flush segments. */
static inline void lighter_do_string(LighterData* data, int disable_nfc, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  ++(data->rindex);

#if LIGHTER_PLATFORM_X86
  if (has_avx512) {
    while (data->rindex < data->data_end) {
      lighter_simd_avx512_string_skip(data);
      if (lighter_string_tail_at_end(data, disable_nfc, has_avx512, has_avx2, has_neon, has_rvv)) {
        return;
      }
    }
    return;
  }
  if (has_avx2) {
    while (data->rindex < data->data_end) {
      lighter_simd_avx2_string_skip(data);
      if (lighter_string_tail_at_end(data, disable_nfc, has_avx512, has_avx2, has_neon, has_rvv)) {
        return;
      }
    }
    return;
  }
#endif

#if LIGHTER_PLATFORM_ARM64
  if (has_neon) {
    while (data->rindex < data->data_end) {
      lighter_simd_neon_string_skip(data);
      if (lighter_string_tail_at_end(data, disable_nfc, has_avx512, has_avx2, has_neon, has_rvv)) {
        return;
      }
    }
    return;
  }
#endif

#if LIGHTER_PLATFORM_RISCV
  if (has_rvv) {
    while (data->rindex < data->data_end) {
      lighter_simd_rvv_string_skip(data);
      if (lighter_string_tail_at_end(data, disable_nfc, has_avx512, has_avx2, has_neon, has_rvv)) {
        return;
      }
    }
    return;
  }
#endif

  while (data->rindex < data->data_end) {
    while (data->rindex + 8 <= data->data_end) {
      uint64_t v;
      memcpy(&v, data->rindex, 8);
      if (lighter_has_byte(v, '"') || lighter_has_byte(v, '\\')) {
        break;
      }
      data->rindex += 8;
    }
    if (lighter_string_tail_at_end(data, disable_nfc, has_avx512, has_avx2, has_neon, has_rvv)) {
      return;
    }
  }
}

#endif /* LIGHTER_STRING_H */
