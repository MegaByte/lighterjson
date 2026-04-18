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

/** Parse four hex digits at rindex as a \uXXXX value. */
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

/** Decode one JSON \u escape and append its UTF-8 form to the output. */
static inline void lighter_string_do_unicode(LighterData* data, int* saw_non_ascii) {
  uint64_t value = lighter_string_hex_value(data);
  if (value == (uint64_t)INT64_MAX) {
    /* Malformed \uXXXX: pass through the remaining bytes as-is so we don't
     * corrupt surrounding data or loop forever. */
    if (data->rindex < data->data_end) {
      ++(data->rindex);
    }
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
        return;
      }
    } else {
      return;
    }
  } else if (value >= SURROGATE_LOW_START && value <= SURROGATE_LOW_START + SURROGATE_MASK) {
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
  } else {
    /* Non-ASCII codepoint: NFC normalization needs to consider this string. */
    *saw_non_ascii = 1;
    data->windex = lighter_write_utf8_scalar(data->windex, (uint32_t)value);
  }
}

/** Handle a JSON string escape sequence at rindex. */
static inline void lighter_string_do_escape(LighterData* data, int* saw_non_ascii) {
  if (data->rindex + 1 >= data->data_end) {
    /* Trailing '\\' at end of input — advance past it to avoid infinite loop. */
    ++(data->rindex);
    return;
  }
  switch (data->rindex[1]) {
    case 'u':
      lighter_write_data(data, 0); /* flush before \u */
      data->rindex += 2;           /* skip \u */
      data->lindex = data->rindex;
      lighter_string_do_unicode(data, saw_non_ascii);
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

#include "lighter_cpu.h"

#if LIGHTER_PLATFORM_X86
LIGHTER_TARGET_AVX2
/** Advance to the next quote or backslash with an AVX2 scan. */
static inline void lighter_simd_avx2_string_skip(LighterData* data, int* saw_non_ascii) {
  const __m256i quote = _mm256_set1_epi8('"');
  const __m256i escape = _mm256_set1_epi8('\\');
  while (data->rindex + 32 <= data->data_end) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)data->rindex);
    if (!*saw_non_ascii && _mm256_movemask_epi8(chunk) != 0) {
      *saw_non_ascii = 1;
    }
    __m256i test_either = _mm256_or_si256(_mm256_cmpeq_epi8(chunk, quote), _mm256_cmpeq_epi8(chunk, escape));
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(test_either);
    if (mask == 0) {
      data->rindex += 32;
      continue;
    }
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
#endif /* LIGHTER_PLATFORM_X86 */
#if LIGHTER_PLATFORM_ARM64
/** Advance to the next quote or backslash with a NEON scan. */
static inline void lighter_simd_neon_string_skip(LighterData* data, int* saw_non_ascii) {
  const uint8x16_t quote = vdupq_n_u8('"');
  const uint8x16_t escape = vdupq_n_u8('\\');
  while (data->rindex + 16 <= data->data_end) {
    uint8x16_t chunk = vld1q_u8((const uint8_t*)data->rindex);
    if (!*saw_non_ascii && vmaxvq_u8(chunk) >= 0x80) {
      *saw_non_ascii = 1;
    }
    uint8x16_t test_either = vorrq_u8(vceqq_u8(chunk, quote), vceqq_u8(chunk, escape));
    /* Fast any-set check via horizontal max, then locate via two-u64 extract. */
    if (vmaxvq_u8(test_either) == 0) {
      data->rindex += 16;
      continue;
    }
    uint64x2_t u64 = vreinterpretq_u64_u8(test_either);
    uint64_t low = vgetq_lane_u64(u64, 0);
    if (low) {
  #if defined(_MSC_VER)
      unsigned long offset;
      _BitScanForward64(&offset, low);
      data->rindex += offset >> 3;
  #else
      data->rindex += __builtin_ctzll(low) >> 3;
  #endif
    } else {
      uint64_t high = vgetq_lane_u64(u64, 1);
  #if defined(_MSC_VER)
      unsigned long offset;
      _BitScanForward64(&offset, high);
      data->rindex += (offset >> 3) + 8;
  #else
      data->rindex += (__builtin_ctzll(high) >> 3) + 8;
  #endif
    }
    return;
  }
}
#endif

#if LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
/** Advance to the next quote or backslash with an RVV scan. */
LIGHTER_TARGET_RVV
static void lighter_simd_rvv_string_skip(LighterData* data, int* saw_non_ascii) {
  while (data->rindex < data->data_end) {
    size_t n = data->data_end - data->rindex;
    size_t vl = __riscv_vsetvl_e8m1(n);
    vuint8m1_t chunk = __riscv_vle8_v_u8m1(data->rindex, vl);
    if (!*saw_non_ascii) {
      vbool8_t hi = __riscv_vmsgtu_vx_u8m1_b8(chunk, 127, vl);
      if (__riscv_vfirst_m_b8(hi, vl) >= 0) {
        *saw_non_ascii = 1;
      }
    }
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

/** Finalize parsing when rindex is positioned at the string tail or closing quote.
 * out_quote_start is the windex position of the opening '"' in the output buffer
 * (saved by lighter_do_string before the first byte of this string was processed);
 * the NFC normalizer reads the full output content range from there. */
static inline int lighter_string_tail_at_end(LighterData* data, int disable_nfc, int has_avx2, int has_neon, int has_rvv, int* saw_non_ascii,
                                             uint8_t* out_quote_start) {
  if (data->rindex >= data->data_end) {
    return 1;
  }
  switch (*data->rindex) {
    case '\\':
      lighter_string_do_escape(data, saw_non_ascii);
      break;
    case '"': {
      /* lindex points at the opening quote (or last escape boundary); rindex points
       * at the closing quote. Content is [lindex+1, rindex) in the source plus any
       * decoded escape bytes already at [out_quote_start+1, windex). Flush the tail,
       * then normalize the full output content range if needed. */
      if (*saw_non_ascii) {
        data->saw_non_ascii = 1; /* propagate up so do_file can skip the ASCII rescan */
      }
      if (!disable_nfc && *saw_non_ascii) {
        lighter_write_data(data, 1); /* flush [lindex .. rindex), consume closing quote */
        uint8_t* str_content_start = out_quote_start + 1;
        uint8_t* str_content_end = data->windex;
        /* LIGHTERJSON_NFC_NO_QUICKCHECK=1 skips the dedicated prescan and goes straight
         * to incremental normalization (which has its own internal byte-scan early-exit).
         * Useful for benchmarking the quick-check's contribution. */
        static int qc_disabled = -1;
        if (qc_disabled < 0) {
          const char* e = getenv("LIGHTERJSON_NFC_NO_QUICKCHECK");
          qc_disabled = (e && *e && *e != '0');
        }
        if (qc_disabled || nfc_quick_check("lighter.nfc", str_content_start, str_content_end, has_avx2, has_neon, has_rvv) != NFC_QC_YES) {
          uint8_t* new_end = nfc_normalize_utf8_incremental(nfc_get_or_load("lighter.nfc"), str_content_start, str_content_end);
          data->windex = new_end;
        }
        *data->windex++ = '"';
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

/** Close a truncated string at EOF: flush the partial content and append '"'.
 * Called only when the input was truncated mid-string; not on the hot path. */
static inline void lighter_string_close_at_eof(LighterData* data) {
  /* lindex points at the opening '"' (or escape boundary); rindex == data_end.
   * Flush the partial bytes, then write a synthetic closing quote so output is
   * still valid JSON. If the buffer is full, signal upward so the caller can
   * expand the mapping and retry. */
  lighter_write_data(data, 0);
  if (data->windex < data->buffer_end) {
    *data->windex++ = '"';
  } else {
    data->needs_quote = 1;
  }
}

/** Append a synthetic closing quote when the string runs to EOF. */
static inline void lighter_string_finish(LighterData* data) {
  if (data->rindex >= data->data_end) {
    lighter_string_close_at_eof(data);
  }
}

/** Parse the JSON string at rindex and optionally NFC-normalize its content. */
static inline void lighter_do_string(LighterData* data, int disable_nfc, int has_avx2, int has_neon, int has_rvv) {
  /* Save windex BEFORE consuming the opening quote: with windex==lindex at entry,
   * after the first flush the opening '"' lands at this exact byte in the output.
   * We need this to bound the post-escape NFC content range, since escape decoding
   * writes non-ASCII bytes directly to windex outside [lindex, rindex). */
  uint8_t* out_quote_start = data->windex;
  ++(data->rindex);
  int saw_non_ascii = 0;

#if LIGHTER_PLATFORM_X86
  if (has_avx2) {
    while (data->rindex < data->data_end) {
      lighter_simd_avx2_string_skip(data, &saw_non_ascii);
      if (lighter_string_tail_at_end(data, disable_nfc, has_avx2, has_neon, has_rvv, &saw_non_ascii, out_quote_start)) {
        lighter_string_finish(data);
        return;
      }
    }
    lighter_string_close_at_eof(data);
    return;
  }
#endif

  /* ARM64 uses the 8-byte SWAR path here. Keep a reference to the NEON helper
   * so the target-specific implementation remains compiled. */
#if LIGHTER_PLATFORM_ARM64
  (void)lighter_simd_neon_string_skip;
#endif

#if LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (has_rvv && LIGHTER_RVV_SITE_ENABLED("string")) {
    while (data->rindex < data->data_end) {
      lighter_simd_rvv_string_skip(data, &saw_non_ascii);
      if (lighter_string_tail_at_end(data, disable_nfc, has_avx2, has_neon, has_rvv, &saw_non_ascii, out_quote_start)) {
        lighter_string_finish(data);
        return;
      }
    }
    lighter_string_close_at_eof(data);
    return;
  }
#endif

  while (data->rindex < data->data_end) {
    /* 8-byte SWAR scan: build a combined mask where each matching byte has bit 0x80 set,
     * then CTZ to jump directly to the first '"' or '\\'. Detects non-ASCII bytes
     * in-flight via the high-bit mask so we don't need a second scan. */
    while (data->rindex + 8 <= data->data_end) {
      uint64_t v;
      memcpy(&v, data->rindex, 8);
      if (!saw_non_ascii && (v & 0x8080808080808080ULL)) {
        saw_non_ascii = 1;
      }
      uint64_t q = v ^ 0x2222222222222222ULL;
      uint64_t e = v ^ 0x5C5C5C5C5C5C5C5CULL;
      q = (q - 0x0101010101010101ULL) & ~q & 0x8080808080808080ULL;
      e = (e - 0x0101010101010101ULL) & ~e & 0x8080808080808080ULL;
      uint64_t m = q | e;
      if (m) {
#if defined(_MSC_VER)
        unsigned long offset;
        _BitScanForward64(&offset, m);
        data->rindex += (size_t)(offset >> 3);
#else
        data->rindex += (size_t)(__builtin_ctzll(m) >> 3);
#endif
        break;
      }
      data->rindex += 8;
    }
    /* Byte-by-byte for the final <8 tail. */
    while (data->rindex < data->data_end) {
      uint8_t c = *data->rindex;
      if (c == '"' || c == '\\') {
        break;
      }
      if (c >= 0x80) {
        saw_non_ascii = 1;
      }
      ++data->rindex;
    }
    if (lighter_string_tail_at_end(data, disable_nfc, has_avx2, has_neon, has_rvv, &saw_non_ascii, out_quote_start)) {
      lighter_string_finish(data);
      return;
    }
  }
  lighter_string_close_at_eof(data);
}

#endif /* LIGHTER_STRING_H */
