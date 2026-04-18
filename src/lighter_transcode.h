/**
 * @file      lighter_transcode.h
 * @brief     Encoding detection and transcoding for LighterJSON
 */
#ifndef LIGHTER_TRANSCODE_H
#define LIGHTER_TRANSCODE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lighter_common.h"
#include "lighter_cpu.h"

typedef enum LighterEncoding {
  LIGHTER_ENC_UTF8,
  LIGHTER_ENC_UTF16LE,
  LIGHTER_ENC_UTF16BE,
  LIGHTER_ENC_UTF32LE,
  LIGHTER_ENC_UTF32BE
} LighterEncoding;

#define LIGHTER_UTF8_MAX_BYTES 4

/** Detect encoding based on BOM or null byte patterns. */
static inline LighterEncoding lighter_detect_encoding(const uint8_t* buf, size_t size) {
  if (size >= 4) {
    if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0xFE && buf[3] == 0xFF) {
      return LIGHTER_ENC_UTF32BE;
    }
    if (buf[0] == 0xFF && buf[1] == 0xFE && buf[2] == 0x00 && buf[3] == 0x00) {
      return LIGHTER_ENC_UTF32LE;
    }
    if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00) {
      return LIGHTER_ENC_UTF32BE;
    }
    if (buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x00) {
      return LIGHTER_ENC_UTF32LE;
    }
  }
  if (size >= 3) {
    if (buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
      return LIGHTER_ENC_UTF8;
    }
  }
  if (size >= 2) {
    if (buf[0] == 0xFE && buf[1] == 0xFF) {
      return LIGHTER_ENC_UTF16BE;
    }
    if (buf[0] == 0xFF && buf[1] == 0xFE) {
      return LIGHTER_ENC_UTF16LE;
    }
    if (buf[0] == 0x00) {
      return LIGHTER_ENC_UTF16BE;
    }
    if (buf[1] == 0x00) {
      return LIGHTER_ENC_UTF16LE;
    }
  }
  return LIGHTER_ENC_UTF8;
}

/** Return the size of the BOM for the given encoding and buffer. */
static inline size_t lighter_encoding_bom_size(LighterEncoding enc, const uint8_t* buf, size_t size) {
  if (enc == LIGHTER_ENC_UTF32LE || enc == LIGHTER_ENC_UTF32BE) {
    if (size >= 4 &&
        ((buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0xFE && buf[3] == 0xFF) || (buf[0] == 0xFF && buf[1] == 0xFE && buf[2] == 0x00 && buf[3] == 0x00))) {
      return 4;
    }
  } else if (enc == LIGHTER_ENC_UTF16LE || enc == LIGHTER_ENC_UTF16BE) {
    if (size >= 2 && ((buf[0] == 0xFE && buf[1] == 0xFF) || (buf[0] == 0xFF && buf[1] == 0xFE))) {
      return 2;
    }
  } else if (enc == LIGHTER_ENC_UTF8) {
    if (size >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
      return 3;
    }
  }
  return 0;
}

/* ─────────── UTF-16 ASCII-only fast path ─────────── */

#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
/** AVX2: scan UTF-16 for any non-ASCII codepoint. Returns 0 on first non-ASCII. */
LIGHTER_TARGET_AVX2
static int lighter_utf16_is_ascii_avx2(const uint8_t* src, size_t size, int is_le) {
  __m256i mask = is_le ? _mm256_set1_epi16((short)0xFF80) : _mm256_set1_epi16((short)0x80FFu);
  while (size >= 32) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)src);
    if (_mm256_testz_si256(chunk, mask) == 0) {
      return 0;
    }
    src += 32;
    size -= 32;
  }
  return 1;
}

/** AVX2: pack low (or high) byte of each UTF-16 codepoint into a UTF-8 byte. */
LIGHTER_TARGET_AVX2
static size_t lighter_transcode_utf16_ascii_avx2(const uint8_t* src, size_t src_size, uint8_t* dst, int is_le) {
  uint8_t* d = dst;
  size_t i = 0;
  while (i + 64 <= src_size) {
    __m256i a = _mm256_loadu_si256((const __m256i*)(src + i));
    __m256i b = _mm256_loadu_si256((const __m256i*)(src + i + 32));
    if (!is_le) {
      a = _mm256_srli_epi16(a, 8);
      b = _mm256_srli_epi16(b, 8);
    } else {
      a = _mm256_and_si256(a, _mm256_set1_epi16(0x00FF));
      b = _mm256_and_si256(b, _mm256_set1_epi16(0x00FF));
    }
    __m256i packed = _mm256_packus_epi16(a, b);
    packed = _mm256_permute4x64_epi64(packed, 0xD8);
    _mm256_storeu_si256((__m256i*)d, packed);
    d += 32;
    i += 64;
  }
  return (size_t)(d - dst);
}
#endif

#if LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
/** RVV: scan UTF-16 buffer for any non-ASCII codepoint. Returns 0 if non-ASCII found. */
LIGHTER_TARGET_RVV
static int lighter_utf16_is_ascii_rvv(const uint8_t* src, size_t size, int is_le) {
  while (size >= 2) {
    size_t vl = __riscv_vsetvl_e16m1(size / 2);
    vuint16m1_t chunk = __riscv_vle16_v_u16m1((const uint16_t*)src, vl);
    /* For LE: low byte is in bits 0..7, high byte in 8..15. ASCII means value < 0x80.
     * For BE: byte order is reversed in memory but vle16 loaded little-endian, so the
     *         "high byte" of the codepoint is actually in bits 0..7. ASCII means
     *         (chunk & 0x00FF) == 0 AND (chunk >> 8) < 0x80, i.e. the byte-swapped
     *         interpretation is < 0x80, which is the same test as (chunk & 0x80FF) == 0. */
    uint16_t mask = is_le ? 0xFF80u : 0x80FFu;
    vbool16_t bad = __riscv_vmsne_vx_u16m1_b16(__riscv_vand_vx_u16m1(chunk, mask, vl), 0, vl);
    if (__riscv_vfirst_m_b16(bad, vl) >= 0) {
      return 0;
    }
    src += vl * 2;
    size -= vl * 2;
  }
  return 1;
}

/** RVV: pack low (or high) byte of each UTF-16 codepoint into a UTF-8 byte. */
LIGHTER_TARGET_RVV
static size_t lighter_transcode_utf16_ascii_rvv(const uint8_t* src, size_t src_size, uint8_t* dst, int is_le) {
  uint8_t* d = dst;
  while (src_size >= 2) {
    size_t vl = __riscv_vsetvl_e16m1(src_size / 2);
    vuint16m1_t chunk = __riscv_vle16_v_u16m1((const uint16_t*)src, vl);
    if (!is_le) {
      chunk = __riscv_vsrl_vx_u16m1(chunk, 8, vl);
    } else {
      chunk = __riscv_vand_vx_u16m1(chunk, 0x00FF, vl);
    }
    vuint8mf2_t narrowed = __riscv_vncvt_x_x_w_u8mf2(chunk, vl);
    __riscv_vse8_v_u8mf2(d, narrowed, vl);
    d += vl;
    src += vl * 2;
    src_size -= vl * 2;
  }
  return (size_t)(d - dst);
}
#endif

/** Return non-zero when every UTF-16 codepoint in [src, src+size) is ASCII (<0x80).
 * `is_le` selects which byte of each pair carries the high bits (low-byte for LE).
 * Caller must ensure size is a multiple of 2. */
static inline int lighter_utf16_is_ascii_only(const uint8_t* src, size_t size, int is_le) {
  if (size & 1u) {
    return 0; /* malformed: odd byte count cannot be UTF-16 */
  }
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (size >= 32 && lighter_cpu_supports_avx2()) {
    if (!lighter_utf16_is_ascii_avx2(src, size, is_le)) {
      return 0;
    }
    size_t consumed = (size / 32) * 32;
    src += consumed;
    size -= consumed;
  }
#elif LIGHTER_PLATFORM_ARM64
  {
    uint8x16_t mask = is_le ? vreinterpretq_u8_u16(vdupq_n_u16(0xFF00)) : vreinterpretq_u8_u16(vdupq_n_u16(0x00FF));
    while (size >= 16) {
      uint8x16_t chunk = vld1q_u8(src);
      if (vmaxvq_u8(chunk) >= 0x80) {
        return 0;
      }
      if (vmaxvq_u8(vandq_u8(chunk, mask)) != 0) {
        return 0;
      }
      src += 16;
      size -= 16;
    }
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (lighter_cpu_supports_rvv() && size >= 16) {
    if (!lighter_utf16_is_ascii_rvv(src, size, is_le)) {
      return 0;
    }
    src += size;
    size = 0;
  }
#endif
  /* Scalar tail */
  for (size_t i = 0; i < size; i += 2) {
    uint8_t hi = is_le ? src[i + 1] : src[i];
    uint8_t lo = is_le ? src[i] : src[i + 1];
    if (hi != 0 || (lo & 0x80) != 0) {
      return 0;
    }
  }
  return 1;
}

/** Fast in-place transcode of UTF-16 ASCII-only content to UTF-8. Picks every
 * other byte from src into dst. src and dst may overlap if dst <= src. */
static inline void lighter_transcode_utf16_ascii_to_utf8(const uint8_t* src, size_t src_size, uint8_t* dst, int is_le, size_t* out_size) {
  uint8_t* d = dst;
  size_t i = 0;
  size_t offset = is_le ? 0 : 1;
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (src_size >= 64 && lighter_cpu_supports_avx2()) {
    size_t produced = lighter_transcode_utf16_ascii_avx2(src, src_size, d, is_le);
    d += produced;
    i += produced * 2;
  }
#elif LIGHTER_PLATFORM_ARM64
  while (i + 32 <= src_size) {
    uint8x16x2_t pair = vld2q_u8(src + i);
    uint8x16_t lows = is_le ? pair.val[0] : pair.val[1];
    vst1q_u8(d, lows);
    d += 16;
    i += 32;
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (lighter_cpu_supports_rvv() && src_size - i >= 16) {
    size_t produced = lighter_transcode_utf16_ascii_rvv(src + i, src_size - i, d, is_le);
    /* RVV helper consumed all complete codepoints in its window; the scalar tail
     * below handles whatever remains (the helper stops when fewer than 2 bytes left). */
    d += produced;
    i += produced * 2;
  }
#endif
  /* Scalar tail */
  while (i + 2 <= src_size) {
    *d++ = src[i + offset];
    i += 2;
  }
  *out_size = (size_t)(d - dst);
}

/* ─────────── UTF-32 ASCII-only fast path ─────────── */

#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
LIGHTER_TARGET_AVX2
static int lighter_utf32_is_ascii_avx2(const uint8_t* src, size_t size, int is_le) {
  __m256i mask = is_le ? _mm256_set1_epi32((int)0xFFFFFF80) : _mm256_set1_epi32((int)0x80FFFFFFu);
  while (size >= 32) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)src);
    if (_mm256_testz_si256(chunk, mask) == 0) {
      return 0;
    }
    src += 32;
    size -= 32;
  }
  return 1;
}

LIGHTER_TARGET_AVX2
static size_t lighter_transcode_utf32_ascii_avx2(const uint8_t* src, size_t src_size, uint8_t* dst, int is_le) {
  uint8_t* d = dst;
  size_t i = 0;
  while (i + 128 <= src_size) {
    __m256i a = _mm256_loadu_si256((const __m256i*)(src + i));
    __m256i b = _mm256_loadu_si256((const __m256i*)(src + i + 32));
    __m256i c = _mm256_loadu_si256((const __m256i*)(src + i + 64));
    __m256i e = _mm256_loadu_si256((const __m256i*)(src + i + 96));
    if (is_le) {
      __m256i mask32 = _mm256_set1_epi32(0xFF);
      a = _mm256_and_si256(a, mask32);
      b = _mm256_and_si256(b, mask32);
      c = _mm256_and_si256(c, mask32);
      e = _mm256_and_si256(e, mask32);
    } else {
      a = _mm256_srli_epi32(a, 24);
      b = _mm256_srli_epi32(b, 24);
      c = _mm256_srli_epi32(c, 24);
      e = _mm256_srli_epi32(e, 24);
    }
    __m256i ab = _mm256_packus_epi32(a, b);
    __m256i ce = _mm256_packus_epi32(c, e);
    __m256i packed = _mm256_packus_epi16(ab, ce);
    packed = _mm256_permutevar8x32_epi32(packed, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
    _mm256_storeu_si256((__m256i*)d, packed);
    d += 32;
    i += 128;
  }
  return (size_t)(d - dst);
}
#endif

#if LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
LIGHTER_TARGET_RVV
static int lighter_utf32_is_ascii_rvv(const uint8_t* src, size_t size, int is_le) {
  while (size >= 4) {
    size_t vl = __riscv_vsetvl_e32m1(size / 4);
    vuint32m1_t chunk = __riscv_vle32_v_u32m1((const uint32_t*)src, vl);
    if (!is_le) {
      /* For BE the low byte (significant) is at offset 3 in memory; vle32 read
       * little-endian so the codepoint's low byte sits in bits 24..31. We test
       * "all bytes except low-of-codepoint == 0 AND low-of-codepoint < 0x80". */
      vbool32_t bad = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(chunk, 0x80FFFFFFu, vl), 0, vl);
      if (__riscv_vfirst_m_b32(bad, vl) >= 0) {
        return 0;
      }
    } else {
      vbool32_t bad = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(chunk, 0xFFFFFF80u, vl), 0, vl);
      if (__riscv_vfirst_m_b32(bad, vl) >= 0) {
        return 0;
      }
    }
    src += vl * 4;
    size -= vl * 4;
  }
  return 1;
}

LIGHTER_TARGET_RVV
static size_t lighter_transcode_utf32_ascii_rvv(const uint8_t* src, size_t src_size, uint8_t* dst, int is_le) {
  uint8_t* d = dst;
  while (src_size >= 4) {
    size_t vl = __riscv_vsetvl_e32m1(src_size / 4);
    vuint32m1_t chunk = __riscv_vle32_v_u32m1((const uint32_t*)src, vl);
    if (!is_le) {
      chunk = __riscv_vsrl_vx_u32m1(chunk, 24, vl);
    } else {
      chunk = __riscv_vand_vx_u32m1(chunk, 0xFF, vl);
    }
    vuint16mf2_t step1 = __riscv_vncvt_x_x_w_u16mf2(chunk, vl);
    vuint8mf4_t narrowed = __riscv_vncvt_x_x_w_u8mf4(step1, vl);
    __riscv_vse8_v_u8mf4(d, narrowed, vl);
    d += vl;
    src += vl * 4;
    src_size -= vl * 4;
  }
  return (size_t)(d - dst);
}
#endif

/** Return non-zero when every UTF-32 codepoint in [src, src+size) is ASCII (<0x80).
 * Caller must ensure size is a multiple of 4. */
static inline int lighter_utf32_is_ascii_only(const uint8_t* src, size_t size, int is_le) {
  if (size & 3u) {
    return 0;
  }
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (size >= 32 && lighter_cpu_supports_avx2()) {
    if (!lighter_utf32_is_ascii_avx2(src, size, is_le)) {
      return 0;
    }
    size_t consumed = (size / 32) * 32;
    src += consumed;
    size -= consumed;
  }
#elif LIGHTER_PLATFORM_ARM64
  {
    uint32x4_t mask = is_le ? vdupq_n_u32(0xFFFFFF80u) : vdupq_n_u32(0x80FFFFFFu);
    while (size >= 16) {
      uint32x4_t chunk = vld1q_u32((const uint32_t*)src);
      if (vmaxvq_u32(vandq_u32(chunk, mask)) != 0) {
        return 0;
      }
      src += 16;
      size -= 16;
    }
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (lighter_cpu_supports_rvv() && size >= 16) {
    if (!lighter_utf32_is_ascii_rvv(src, size, is_le)) {
      return 0;
    }
    src += size;
    size = 0;
  }
#endif
  /* Scalar tail */
  for (size_t i = 0; i < size; i += 4) {
    if (is_le) {
      if (src[i + 3] || src[i + 2] || src[i + 1] || (src[i] & 0x80)) {
        return 0;
      }
    } else {
      if (src[i] || src[i + 1] || src[i + 2] || (src[i + 3] & 0x80)) {
        return 0;
      }
    }
  }
  return 1;
}

/** Fast in-place transcode of UTF-32 ASCII-only content to UTF-8. */
static inline void lighter_transcode_utf32_ascii_to_utf8(const uint8_t* src, size_t src_size, uint8_t* dst, int is_le, size_t* out_size) {
  uint8_t* d = dst;
  size_t i = 0;
  size_t offset = is_le ? 0 : 3;
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (src_size >= 128 && lighter_cpu_supports_avx2()) {
    size_t produced = lighter_transcode_utf32_ascii_avx2(src, src_size, d, is_le);
    d += produced;
    i += produced * 4;
  }
#elif LIGHTER_PLATFORM_ARM64
  while (i + 64 <= src_size) {
    uint32x4_t a = vld1q_u32((const uint32_t*)(src + i));
    uint32x4_t b = vld1q_u32((const uint32_t*)(src + i + 16));
    uint32x4_t c = vld1q_u32((const uint32_t*)(src + i + 32));
    uint32x4_t e = vld1q_u32((const uint32_t*)(src + i + 48));
    if (!is_le) {
      a = vshrq_n_u32(a, 24);
      b = vshrq_n_u32(b, 24);
      c = vshrq_n_u32(c, 24);
      e = vshrq_n_u32(e, 24);
    }
    /* Two-step narrow: 32→16 (taking low half of each lane), then 16→8. */
    uint16x8_t ab = vcombine_u16(vmovn_u32(a), vmovn_u32(b));
    uint16x8_t ce = vcombine_u16(vmovn_u32(c), vmovn_u32(e));
    uint8x16_t packed = vcombine_u8(vmovn_u16(ab), vmovn_u16(ce));
    vst1q_u8(d, packed);
    d += 16;
    i += 64;
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (lighter_cpu_supports_rvv() && src_size - i >= 16) {
    size_t produced = lighter_transcode_utf32_ascii_rvv(src + i, src_size - i, d, is_le);
    d += produced;
    i += produced * 4;
  }
#endif
  /* Scalar tail */
  while (i + 4 <= src_size) {
    *d++ = src[i + offset];
    i += 4;
  }
  *out_size = (size_t)(d - dst);
}

#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
/** AVX2: count leading ASCII codepoints of a UTF-32 buffer; returns advanced pointer. */
LIGHTER_TARGET_AVX2
static const uint8_t* lighter_utf32_ascii_run_avx2(const uint8_t* p, size_t avail, int is_le) {
  __m256i mask = is_le ? _mm256_set1_epi32((int)0xFFFFFF80) : _mm256_set1_epi32((int)0x80FFFFFFu);
  while (avail >= 32) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)p);
    if (_mm256_testz_si256(chunk, mask) == 0) {
      break;
    }
    p += 32;
    avail -= 32;
  }
  return p;
}
#endif

#if LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
/** RVV: count leading ASCII codepoints of a UTF-32 buffer; returns advanced pointer. */
LIGHTER_TARGET_RVV
static const uint8_t* lighter_utf32_ascii_run_rvv(const uint8_t* p, const uint8_t* end, int is_le) {
  size_t avail = (size_t)(end - p);
  while (avail >= 16) {
    size_t vl = __riscv_vsetvl_e32m1(avail / 4);
    vuint32m1_t chunk = __riscv_vle32_v_u32m1((const uint32_t*)p, vl);
    uint32_t m = is_le ? 0xFFFFFF80u : 0x80FFFFFFu;
    vbool32_t bad = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(chunk, m, vl), 0, vl);
    intptr_t first = __riscv_vfirst_m_b32(bad, vl);
    if (first >= 0) {
      return p + first * 4;
    }
    p += vl * 4;
    avail = (size_t)(end - p);
  }
  return p;
}
#endif

/** Greedy ASCII probe: count how many leading bytes of [src, end) form a contiguous
 * run of ASCII UTF-32 codepoints. Returns byte count (multiple of 4). */
static inline size_t lighter_utf32_ascii_run(const uint8_t* src, const uint8_t* end, int is_le) {
  const uint8_t* p = src;
  size_t avail = (size_t)(end - p);
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (avail >= 32 && lighter_cpu_supports_avx2()) {
    p = lighter_utf32_ascii_run_avx2(p, avail, is_le);
    avail = (size_t)(end - p);
  }
#elif LIGHTER_PLATFORM_ARM64
  uint32x4_t mask = is_le ? vdupq_n_u32(0xFFFFFF80u) : vdupq_n_u32(0x80FFFFFFu);
  while (avail >= 16) {
    uint32x4_t chunk = vld1q_u32((const uint32_t*)p);
    if (vmaxvq_u32(vandq_u32(chunk, mask)) != 0) {
      break;
    }
    p += 16;
    avail -= 16;
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (avail >= 16 && lighter_cpu_supports_rvv()) {
    p = lighter_utf32_ascii_run_rvv(p, end, is_le);
    avail = (size_t)(end - p);
  }
#endif
  /* Scalar tail: one codepoint at a time */
  while (avail >= 4) {
    if (is_le) {
      if (p[3] || p[2] || p[1] || (p[0] & 0x80)) {
        break;
      }
    } else {
      if (p[0] || p[1] || p[2] || (p[3] & 0x80)) {
        break;
      }
    }
    p += 4;
    avail -= 4;
  }
  return (size_t)(p - src);
}

#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
/** AVX2: count leading ASCII codepoints of a UTF-16 buffer; returns advanced pointer. */
LIGHTER_TARGET_AVX2
static const uint8_t* lighter_utf16_ascii_run_avx2(const uint8_t* p, size_t avail, int is_le) {
  __m256i mask = is_le ? _mm256_set1_epi16((short)0xFF80) : _mm256_set1_epi16((short)0x80FFu);
  while (avail >= 32) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)p);
    if (_mm256_testz_si256(chunk, mask) == 0) {
      break;
    }
    p += 32;
    avail -= 32;
  }
  return p;
}
#endif

#if LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
/** RVV: count leading ASCII codepoints of a UTF-16 buffer; returns advanced pointer. */
LIGHTER_TARGET_RVV
static const uint8_t* lighter_utf16_ascii_run_rvv(const uint8_t* p, const uint8_t* end, int is_le) {
  size_t avail = (size_t)(end - p);
  while (avail >= 16) {
    size_t vl = __riscv_vsetvl_e16m1(avail / 2);
    vuint16m1_t chunk = __riscv_vle16_v_u16m1((const uint16_t*)p, vl);
    uint16_t m = is_le ? 0xFF80u : 0x80FFu;
    vbool16_t bad = __riscv_vmsne_vx_u16m1_b16(__riscv_vand_vx_u16m1(chunk, m, vl), 0, vl);
    intptr_t first = __riscv_vfirst_m_b16(bad, vl);
    if (first >= 0) {
      return p + first * 2;
    }
    p += vl * 2;
    avail = (size_t)(end - p);
  }
  return p;
}
#endif

/** Greedy ASCII probe for UTF-16: count leading bytes forming ASCII codepoints. */
static inline size_t lighter_utf16_ascii_run(const uint8_t* src, const uint8_t* end, int is_le) {
  const uint8_t* p = src;
  size_t avail = (size_t)(end - p);
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (avail >= 32 && lighter_cpu_supports_avx2()) {
    p = lighter_utf16_ascii_run_avx2(p, avail, is_le);
    avail = (size_t)(end - p);
  }
#elif LIGHTER_PLATFORM_ARM64
  uint16x8_t mask = is_le ? vdupq_n_u16(0xFF80u) : vdupq_n_u16(0x80FFu);
  while (avail >= 16) {
    uint16x8_t chunk = vld1q_u16((const uint16_t*)p);
    if (vmaxvq_u16(vandq_u16(chunk, mask)) != 0) {
      break;
    }
    p += 16;
    avail -= 16;
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (avail >= 16 && lighter_cpu_supports_rvv()) {
    p = lighter_utf16_ascii_run_rvv(p, end, is_le);
    avail = (size_t)(end - p);
  }
#endif
  /* Scalar tail */
  while (avail >= 2) {
    uint8_t hi = is_le ? p[1] : p[0];
    uint8_t lo = is_le ? p[0] : p[1];
    if (hi != 0 || (lo & 0x80) != 0) {
      break;
    }
    p += 2;
    avail -= 2;
  }
  return (size_t)(p - src);
}

/** Transcode UTF-16/32 to UTF-8. src and dst can overlap if dst < src.
 * Hybrid strategy: at each iteration, probe how many leading codepoints are ASCII;
 * if a substantial run, vectorize the bulk pack; otherwise advance one codepoint
 * at a time through the scalar path. This captures the SIMD win for ASCII-heavy
 * content (most real text outside dense CJK) without surrogate-pair complexity. */
static inline void lighter_transcode_to_utf8(const uint8_t* src, size_t src_size, uint8_t* dst, LighterEncoding enc, size_t* out_size) {
  uint8_t* d = dst;
  const uint8_t* s = src;
  const uint8_t* end = src + src_size;

  if (enc == LIGHTER_ENC_UTF32LE || enc == LIGHTER_ENC_UTF32BE) {
    int is_le = (enc == LIGHTER_ENC_UTF32LE);
    while (s + 4 <= end) {
      /* Greedy ASCII bulk path: detect a run of ASCII codepoints and pack them
       * with the existing vectorized ASCII transcoder. The threshold (32 bytes
       * = 8 codepoints) is the smallest that engages the ASCII transcoder's
       * SIMD path while avoiding overhead on short runs. */
      size_t ascii_bytes = lighter_utf32_ascii_run(s, end, is_le);
      if (ascii_bytes >= 32) {
        size_t out;
        lighter_transcode_utf32_ascii_to_utf8(s, ascii_bytes, d, is_le, &out);
        d += out;
        s += ascii_bytes;
        continue;
      }
      /* Non-ASCII or short run: scalar one-at-a-time. */
      uint32_t cp;
      if (is_le) {
        cp = s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
      } else {
        cp = (s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3];
      }
      s += 4;
      d = lighter_write_utf8_scalar(d, cp);
    }
  } else if (enc == LIGHTER_ENC_UTF16LE || enc == LIGHTER_ENC_UTF16BE) {
    int is_le = (enc == LIGHTER_ENC_UTF16LE);
    while (s + 2 <= end) {
      size_t ascii_bytes = lighter_utf16_ascii_run(s, end, is_le);
      if (ascii_bytes >= 32) {
        size_t out;
        lighter_transcode_utf16_ascii_to_utf8(s, ascii_bytes, d, is_le, &out);
        d += out;
        s += ascii_bytes;
        continue;
      }
      uint32_t cp;
      if (is_le) {
        cp = s[0] | (s[1] << 8);
      } else {
        cp = (s[0] << 8) | s[1];
      }
      s += 2;
      if (cp >= 0xD800 && cp <= 0xDBFF && s + 2 <= end) {
        uint32_t low;
        if (is_le) {
          low = s[0] | (s[1] << 8);
        } else {
          low = (s[0] << 8) | s[1];
        }
        if (low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          s += 2;
        } else {
          continue;
        }
      } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        continue;
      }
      d = lighter_write_utf8_scalar(d, cp);
    }
  }
  *out_size = (size_t)(d - dst);
}

/** Decode one UTF-8 code point from s and report its byte length. */
static inline int lighter_utf8_decode_forward(const uint8_t* s, const uint8_t* end, uint32_t* cp, size_t* len) {
  if (s >= end) {
    return 0;
  }
  if (s[0] <= 0x7F) {
    *cp = s[0];
    *len = 1;
    return 1;
  }
  if ((s[0] & 0xE0) == 0xC0 && s + 1 < end) {
    *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    *len = 2;
    return 1;
  }
  if ((s[0] & 0xF0) == 0xE0 && s + 2 < end) {
    *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    *len = 3;
    return 1;
  }
  if ((s[0] & 0xF8) == 0xF0 && s + 3 < end) {
    *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    *len = 4;
    return 1;
  }
  *cp = 0;
  *len = 1;
  return 0;
}

/** Decode the last UTF-8 code point ending at end and report its byte length. */
static inline int lighter_utf8_decode_backward(const uint8_t* start, const uint8_t* end, uint32_t* cp, size_t* len) {
  const uint8_t* p = end;
  size_t back = 0;

  if (p <= start) {
    return 0;
  }
  do {
    --p;
    ++back;
  } while (p > start && (*p & 0xC0) == 0x80 && back < LIGHTER_UTF8_MAX_BYTES);
  if ((*p & 0xC0) == 0x80) {
    *cp = 0;
    *len = 1;
    return 0;
  }
  return lighter_utf8_decode_forward(p, end, cp, len) && *len == back;
}

/* ─────────── Reverse: UTF-8 ASCII-only fast paths ─────────── */

#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
/** AVX2: widen ASCII UTF-8 bytes to UTF-16 backward; returns updated d, updated i. */
LIGHTER_TARGET_AVX2
static void lighter_widen_ascii_to_utf16_avx2(const uint8_t* src, uint8_t** d_io, size_t* i_io, int is_le) {
  uint8_t* d = *d_io;
  size_t i = *i_io;
  while (i >= 32) {
    __m256i lo = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i*)(src + i - 16)));
    __m256i hi = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i*)(src + i - 32)));
    if (!is_le) {
      lo = _mm256_slli_epi16(lo, 8);
      hi = _mm256_slli_epi16(hi, 8);
    }
    _mm256_storeu_si256((__m256i*)(d - 32), lo);
    _mm256_storeu_si256((__m256i*)(d - 64), hi);
    d -= 64;
    i -= 32;
  }
  *d_io = d;
  *i_io = i;
}

/** AVX2: widen ASCII UTF-8 bytes to UTF-32 backward. */
LIGHTER_TARGET_AVX2
static void lighter_widen_ascii_to_utf32_avx2(const uint8_t* src, uint8_t** d_io, size_t* i_io, int is_le) {
  uint8_t* d = *d_io;
  size_t i = *i_io;
  while (i >= 16) {
    __m128i bytes = _mm_loadu_si128((const __m128i*)(src + i - 16));
    __m256i a = _mm256_cvtepu8_epi32(bytes);
    __m256i b = _mm256_cvtepu8_epi32(_mm_srli_si128(bytes, 8));
    if (!is_le) {
      a = _mm256_slli_epi32(a, 24);
      b = _mm256_slli_epi32(b, 24);
    }
    _mm256_storeu_si256((__m256i*)(d - 32), b);
    _mm256_storeu_si256((__m256i*)(d - 64), a);
    d -= 64;
    i -= 16;
  }
  *d_io = d;
  *i_io = i;
}
#endif

#if LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
/** RVV: widen ASCII UTF-8 bytes to UTF-16. Walks forward in chunks computing the
 * appropriate destination offset (RVV doesn't have a natural backward strided
 * store). Returns nothing; the caller must position d and handle any tail bytes. */
LIGHTER_TARGET_RVV
static size_t lighter_widen_ascii_to_utf16_rvv(const uint8_t* src, size_t i, uint8_t* base_dst, int is_le) {
  size_t pos = 0;
  while (i - pos >= 8) {
    size_t vl = __riscv_vsetvl_e8mf2(i - pos);
    vuint8mf2_t chunk = __riscv_vle8_v_u8mf2(src + pos, vl);
    vuint16m1_t widened = __riscv_vzext_vf2_u16m1(chunk, vl);
    if (!is_le) {
      widened = __riscv_vsll_vx_u16m1(widened, 8, vl);
    }
    __riscv_vse16_v_u16m1((uint16_t*)(base_dst + pos * 2), widened, vl);
    pos += vl;
  }
  return pos;
}

/** RVV: widen ASCII UTF-8 bytes to UTF-32. */
LIGHTER_TARGET_RVV
static size_t lighter_widen_ascii_to_utf32_rvv(const uint8_t* src, size_t i, uint8_t* base_dst, int is_le) {
  size_t pos = 0;
  while (i - pos >= 4) {
    size_t vl = __riscv_vsetvl_e8mf4(i - pos);
    vuint8mf4_t chunk = __riscv_vle8_v_u8mf4(src + pos, vl);
    vuint16mf2_t step1 = __riscv_vzext_vf2_u16mf2(chunk, vl);
    vuint32m1_t widened = __riscv_vzext_vf2_u32m1(step1, vl);
    if (!is_le) {
      widened = __riscv_vsll_vx_u32m1(widened, 24, vl);
    }
    __riscv_vse32_v_u32m1((uint32_t*)(base_dst + pos * 4), widened, vl);
    pos += vl;
  }
  return pos;
}
#endif

/** Reverse-pack ASCII UTF-8 bytes into UTF-16 (each byte → 2 bytes, low byte = the
 * char, high byte = 0). Walks both pointers backward; dst_end points one past the
 * output range. Output is exactly src_size * 2 bytes. */
static inline void lighter_transcode_utf8_ascii_to_utf16(const uint8_t* src, size_t src_size, uint8_t* dst_end, int is_le) {
  uint8_t* d = dst_end;
  size_t i = src_size;
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (i >= 32 && lighter_cpu_supports_avx2()) {
    lighter_widen_ascii_to_utf16_avx2(src, &d, &i, is_le);
  }
#elif LIGHTER_PLATFORM_ARM64
  while (i >= 16) {
    uint8x16_t chunk = vld1q_u8(src + i - 16);
    /* Widen via interleave with zeros (LE) or swap order (BE). */
    uint8x16_t zero = vdupq_n_u8(0);
    uint8x16x2_t pair;
    if (is_le) {
      pair.val[0] = chunk; /* low bytes */
      pair.val[1] = zero;  /* high bytes */
    } else {
      pair.val[0] = zero;
      pair.val[1] = chunk;
    }
    vst2q_u8(d - 32, pair);
    d -= 32;
    i -= 16;
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (i >= 16 && lighter_cpu_supports_rvv()) {
    /* Walk forward in chunks; output is the same size regardless of direction,
     * and forward strided writes avoid awkward backward indexing in RVV. */
    uint8_t* base_dst = d - i * 2;
    size_t pos = lighter_widen_ascii_to_utf16_rvv(src, i, base_dst, is_le);
    d = base_dst;
    i -= pos;
    while (i > 0) {
      --i;
      uint8_t b = src[i];
      if (is_le) {
        d[i * 2] = b;
        d[i * 2 + 1] = 0;
      } else {
        d[i * 2] = 0;
        d[i * 2 + 1] = b;
      }
    }
    return;
  }
#endif
  while (i > 0) {
    --i;
    uint8_t b = src[i];
    if (is_le) {
      *--d = 0;
      *--d = b;
    } else {
      *--d = b;
      *--d = 0;
    }
  }
}

/** Reverse-pack ASCII UTF-8 bytes into UTF-32 (each byte → 4 bytes). */
static inline void lighter_transcode_utf8_ascii_to_utf32(const uint8_t* src, size_t src_size, uint8_t* dst_end, int is_le) {
  uint8_t* d = dst_end;
  size_t i = src_size;
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (i >= 16 && lighter_cpu_supports_avx2()) {
    lighter_widen_ascii_to_utf32_avx2(src, &d, &i, is_le);
  }
#elif LIGHTER_PLATFORM_ARM64
  while (i >= 8) {
    uint8x8_t chunk = vld1_u8(src + i - 8);
    uint16x8_t w = vmovl_u8(chunk);
    uint32x4_t lo32 = vmovl_u16(vget_low_u16(w));
    uint32x4_t hi32 = vmovl_u16(vget_high_u16(w));
    if (!is_le) {
      lo32 = vshlq_n_u32(lo32, 24);
      hi32 = vshlq_n_u32(hi32, 24);
    }
    vst1q_u32((uint32_t*)(d - 32), lo32);
    vst1q_u32((uint32_t*)(d - 16), hi32);
    d -= 32;
    i -= 8;
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
  if (i >= 16 && lighter_cpu_supports_rvv()) {
    uint8_t* base_dst = d - i * 4;
    size_t pos = lighter_widen_ascii_to_utf32_rvv(src, i, base_dst, is_le);
    d = base_dst;
    i -= pos;
    while (i > 0) {
      --i;
      uint8_t b = src[i];
      if (is_le) {
        d[i * 4] = b;
        d[i * 4 + 1] = 0;
        d[i * 4 + 2] = 0;
        d[i * 4 + 3] = 0;
      } else {
        d[i * 4] = 0;
        d[i * 4 + 1] = 0;
        d[i * 4 + 2] = 0;
        d[i * 4 + 3] = b;
      }
    }
    return;
  }
#endif
  while (i > 0) {
    --i;
    uint8_t b = src[i];
    if (is_le) {
      *--d = 0;
      *--d = 0;
      *--d = 0;
      *--d = b;
    } else {
      *--d = b;
      *--d = 0;
      *--d = 0;
      *--d = 0;
    }
  }
}

/** Return the output size for transcoding UTF-8 to enc, skipping invalid scalars. */
static inline size_t lighter_transcode_from_utf8_size(const uint8_t* src, size_t src_size, LighterEncoding enc) {
  const uint8_t* s = src;
  const uint8_t* end = src + src_size;
  size_t out_size = 0;

  if (enc == LIGHTER_ENC_UTF8) {
    return src_size;
  }
  while (s < end) {
    uint32_t cp;
    size_t len;
    if (!lighter_utf8_decode_forward(s, end, &cp, &len) || !lighter_is_unicode_scalar(cp)) {
      ++s;
      continue;
    }
    s += len;
    if (enc == LIGHTER_ENC_UTF32LE || enc == LIGHTER_ENC_UTF32BE) {
      out_size += 4;
    } else if (cp <= 0xFFFFu) {
      out_size += 2;
    } else {
      out_size += 4;
    }
  }
  return out_size;
}

#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
/** AVX2: scan backward for the latest non-ASCII byte; returns updated p. */
LIGHTER_TARGET_AVX2
static const uint8_t* lighter_utf8_trailing_ascii_avx2(const uint8_t* start, const uint8_t* p) {
  while (p - start >= 32) {
    __m256i chunk = _mm256_loadu_si256((const __m256i*)(p - 32));
    int mask = _mm256_movemask_epi8(chunk);
    if (mask != 0) {
      int highest_set = 31 - __builtin_clz((unsigned)mask);
      return p - (31 - highest_set);
    }
    p -= 32;
  }
  return p;
}
#endif

/** Greedy backward ASCII probe: find longest tail of [start, end) that is all ASCII.
 * Returns the number of trailing ASCII bytes. */
static inline size_t lighter_utf8_trailing_ascii(const uint8_t* start, const uint8_t* end) {
  const uint8_t* p = end;
#if LIGHTER_PLATFORM_X86 && (defined(__GNUC__) || defined(__clang__))
  if (p - start >= 32 && lighter_cpu_supports_avx2()) {
    p = lighter_utf8_trailing_ascii_avx2(start, p);
  }
#elif LIGHTER_PLATFORM_ARM64
  while (p - start >= 16) {
    uint8x16_t chunk = vld1q_u8(p - 16);
    if (vmaxvq_u8(chunk) >= 0x80) {
      /* Find the latest non-ASCII byte in the chunk. Walk from the end backward. */
      for (int i = 15; i >= 0; --i) {
        if (chunk[i] >= 0x80) {
          p -= (15 - i);
          return (size_t)(end - p);
        }
      }
    }
    p -= 16;
  }
#elif LIGHTER_PLATFORM_RISCV && !defined(LIGHTER_NO_RVV_INTRINSICS)
    /* RVV doesn't have a native "find last set" so we fall back to a forward
     * scan within each tail chunk. Cost is comparable; chunks are small. */
#endif
  /* Scalar tail: scan backward */
  while (p > start && p[-1] < 0x80) {
    --p;
  }
  return (size_t)(end - p);
}

/** Bulk-write `n` ASCII UTF-8 bytes to `dst_end` as wide codepoints; returns the
 * new (lower) write pointer. Reuses the existing ASCII widening helpers. */
static inline uint8_t* lighter_widen_ascii_backward(const uint8_t* src_start, size_t n, uint8_t* dst_end, LighterEncoding enc) {
  if (enc == LIGHTER_ENC_UTF32LE || enc == LIGHTER_ENC_UTF32BE) {
    lighter_transcode_utf8_ascii_to_utf32(src_start, n, dst_end, enc == LIGHTER_ENC_UTF32LE);
    return dst_end - n * 4;
  } else {
    lighter_transcode_utf8_ascii_to_utf16(src_start, n, dst_end, enc == LIGHTER_ENC_UTF16LE);
    return dst_end - n * 2;
  }
}

/** In-place/backward UTF-8 transcode. dst_end points one byte past the output range.
 * Hybrid strategy: at each iteration, find the longest trailing ASCII run and
 * widen-write it via the existing vectorized ASCII helper, then handle one
 * non-ASCII codepoint scalar-style. */
static inline void lighter_transcode_from_utf8_backward(const uint8_t* src, size_t src_size, LighterEncoding enc, uint8_t* dst_end) {
  const uint8_t* start = src;
  const uint8_t* s = src + src_size;
  uint8_t* d = dst_end;

  while (s > start) {
    /* Greedy ASCII bulk path */
    size_t trail = lighter_utf8_trailing_ascii(start, s);
    if (trail >= 32) {
      d = lighter_widen_ascii_backward(s - trail, trail, d, enc);
      s -= trail;
      continue;
    }
    /* Scalar single-codepoint path */
    uint32_t cp;
    size_t len;
    if (!lighter_utf8_decode_backward(start, s, &cp, &len) || !lighter_is_unicode_scalar(cp)) {
      --s;
      continue;
    }
    s -= len;

    if (enc == LIGHTER_ENC_UTF32LE) {
      *--d = (uint8_t)((cp >> 24) & 0xFF);
      *--d = (uint8_t)((cp >> 16) & 0xFF);
      *--d = (uint8_t)((cp >> 8) & 0xFF);
      *--d = (uint8_t)(cp & 0xFF);
    } else if (enc == LIGHTER_ENC_UTF32BE) {
      *--d = (uint8_t)(cp & 0xFF);
      *--d = (uint8_t)((cp >> 8) & 0xFF);
      *--d = (uint8_t)((cp >> 16) & 0xFF);
      *--d = (uint8_t)((cp >> 24) & 0xFF);
    } else if (cp <= 0xFFFFu) {
      if (enc == LIGHTER_ENC_UTF16LE) {
        *--d = (uint8_t)((cp >> 8) & 0xFF);
        *--d = (uint8_t)(cp & 0xFF);
      } else {
        *--d = (uint8_t)(cp & 0xFF);
        *--d = (uint8_t)((cp >> 8) & 0xFF);
      }
    } else {
      uint32_t high = 0xD800u + ((cp - 0x10000u) >> 10);
      uint32_t low = 0xDC00u + ((cp - 0x10000u) & 0x3FFu);
      if (enc == LIGHTER_ENC_UTF16LE) {
        *--d = (uint8_t)((low >> 8) & 0xFF);
        *--d = (uint8_t)(low & 0xFF);
        *--d = (uint8_t)((high >> 8) & 0xFF);
        *--d = (uint8_t)(high & 0xFF);
      } else {
        *--d = (uint8_t)(low & 0xFF);
        *--d = (uint8_t)((low >> 8) & 0xFF);
        *--d = (uint8_t)(high & 0xFF);
        *--d = (uint8_t)((high >> 8) & 0xFF);
      }
    }
  }
}

#endif
