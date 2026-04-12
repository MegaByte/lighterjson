/**
 * @file   lighter_simd.h
 * @brief  Shared SIMD utilities for character range checks and masks.
 */

#ifndef LIGHTER_SIMD_H
#define LIGHTER_SIMD_H

#include "lighter_cpu.h"

#if LIGHTER_PLATFORM_X86
  #include <immintrin.h>

/** Returns a mask of bytes that are digits '0'-'9'. */
static inline __m256i lighter_simd_is_digit_avx2(__m256i chunk) {
  return _mm256_and_si256(_mm256_cmpgt_epi8(chunk, _mm256_set1_epi8('0' - 1)), _mm256_cmpgt_epi8(_mm256_set1_epi8('9' + 1), chunk));
}

  #if defined(LIGHTER_TARGET_AVX512)
static inline __mmask64 lighter_simd_is_digit_avx512(__m512i chunk) {
  return _mm512_cmpgt_epi8_mask(chunk, _mm512_set1_epi8('0' - 1)) & _mm512_cmpgt_epi8_mask(_mm512_set1_epi8('9' + 1), chunk);
}
  #endif

static inline uint32_t lighter_simd_mask_avx2(__m256i m) {
  return (uint32_t)_mm256_movemask_epi8(m);
}

static inline uint32_t lighter_simd_first_set_avx2(uint32_t mask) {
  #if defined(_MSC_VER)
  unsigned long offset;
  _BitScanForward(&offset, mask);
  return (uint32_t)offset;
  #else
  return (uint32_t)__builtin_ctz(mask);
  #endif
}

#endif /* LIGHTER_PLATFORM_X86 */

#if LIGHTER_PLATFORM_ARM64
  #include <arm_neon.h>

static inline uint8x16_t lighter_simd_is_digit_neon(uint8x16_t chunk) {
  return vandq_u8(vcgeq_u8(chunk, vdupq_n_u8('0')), vcleq_u8(chunk, vdupq_n_u8('9')));
}

static inline uint64_t lighter_simd_first_set_neon(uint64_t mask) {
  #if defined(_MSC_VER)
  unsigned long offset;
  _BitScanForward64(&offset, mask);
  return (uint64_t)(offset >> 3);
  #else
  return (uint64_t)(__builtin_ctzll(mask) >> 3);
  #endif
}
#endif /* LIGHTER_PLATFORM_ARM64 */

#if LIGHTER_PLATFORM_RISCV
static inline vbool8_t lighter_simd_is_digit_rvv(vuint8m1_t chunk, size_t vl) {
  return __riscv_vmand_mm_b8(__riscv_vmsgeu_vx_u8m1_b8(chunk, '0', vl), __riscv_vmsleu_vx_u8m1_b8(chunk, '9', vl), vl);
}
#endif /* LIGHTER_PLATFORM_RISCV */

#endif /* LIGHTER_SIMD_H */
