/**
 * @file   lighter_number.h
 * @brief  JSON number parsing and reformatting (self-contained header).
 */

#ifndef LIGHTER_NUMBER_H
#define LIGHTER_NUMBER_H

#include <stdint.h>

#include "lighter_common.h"
#include "lighter_math.h"
#include "lighter_simd.h"

/** Write an exponent string adjusted by a small delta. Performs string-based addition/subtraction. */
static inline void lighter_write_adjusted_exponent(LighterData* data, uint8_t* start, uint64_t len, int negative, int64_t delta) {
  /* Compute output = (negative ? -|digits| : +|digits|) + delta.
   * For huge |digits|, |delta| << |digits|, so the output sign matches `negative`.
   * Whether we add or subtract on the absolute value is determined by:
   *   add iff sign(delta) matches the original sign. */
  int add_op = (delta >= 0) ? !negative : negative;
  int64_t d = delta < 0 ? -delta : delta;

  if (len == 0) {
    /* No digits: output is just delta. Sign = sign(delta). */
    if (delta == 0) {
      *data->windex++ = '0';
      return;
    }
    if (delta < 0) {
      *data->windex++ = '-';
    }
    uint32_t digits = lighter_digits_u64((uint64_t)d);
    data->windex += digits;
    uint8_t* p = data->windex;
    while (d) {
      *(--p) = (d % 10) + '0';
      d /= 10;
    }
    return;
  }

  if (len < 18) {
    /* Fits in uint64_t. Compute in signed arithmetic. */
    int64_t val = 0;
    for (uint64_t i = 0; i < len; ++i) {
      val = val * 10 + (start[i] - '0');
    }
    if (negative) {
      val = -val;
    }
    val += delta;
    if (val == 0) {
      *data->windex++ = '0';
      return;
    }
    if (val < 0) {
      *data->windex++ = '-';
      val = -val;
    }
    uint32_t digits = lighter_digits_u64((uint64_t)val);
    data->windex += digits;
    uint8_t* p = data->windex;
    while (val) {
      *(--p) = (val % 10) + '0';
      val /= 10;
    }
    return;
  }

  /* Huge string math. |digits| dominates |delta|; sign stays `negative`.
   * The source [start, start+len) may overlap the write destination, so use
   * lighter_write_data to copy the digits into place first (overlap-safe), then
   * do carry/borrow propagation in-place. Carry/borrow usually terminates within
   * a few digits. */
  if (d == 0) {
    if (negative) {
      *data->windex++ = '-';
    }
    data->lindex = start;
    data->rindex = start + len;
    lighter_write_data(data, 0);
    return;
  }

  if (negative) {
    *data->windex++ = '-';
  }
  /* Flush the exponent digits from [start, start+len) into the output area. After
   * this call, the digits live at [windex-len, windex) (contiguous, no overlap). */
  uint8_t* prev_lindex = data->lindex;
  uint8_t* prev_rindex = data->rindex;
  data->lindex = start;
  data->rindex = start + len;
  lighter_write_data(data, 0);
  data->lindex = prev_lindex;
  data->rindex = prev_rindex;
  uint8_t* dst = data->windex - len;

  if (add_op) {
    int carry = (int)d;
    for (int64_t i = (int64_t)len - 1; i >= 0; --i) {
      int v = (dst[i] - '0') + carry;
      if (v < 10) {
        dst[i] = (uint8_t)(v + '0');
        carry = 0;
        break;
      }
      dst[i] = (uint8_t)((v - 10) + '0');
      carry = 1;
    }
    if (carry) {
      /* All digits were 9; expand by one leading digit. Use write_data to shift
       * [dst, dst+len) right by 1, then write '1' at dst[0]. */
      data->windex = dst + 1;
      data->lindex = dst;
      data->rindex = dst + len;
      lighter_write_data(data, 0);
      /* After write_data: windex = dst+1+len (correct final position). Restore book-keeping. */
      data->lindex = prev_lindex;
      data->rindex = prev_rindex;
      dst[0] = '1';
    }
  } else {
    int borrow = (int)d;
    for (int64_t i = (int64_t)len - 1; i >= 0; --i) {
      int v = (dst[i] - '0') - borrow;
      if (v >= 0) {
        dst[i] = (uint8_t)(v + '0');
        borrow = 0;
        break;
      }
      dst[i] = (uint8_t)((v + 10) + '0');
      borrow = 1;
    }
    /* Leading digit may have dropped to 0; collapse leading zeros via write_data. */
    uint8_t* end = data->windex;
    uint8_t* lead = dst;
    while (lead < end - 1 && *lead == '0') {
      ++lead;
    }
    if (lead != dst) {
      data->windex = dst;
      data->lindex = lead;
      data->rindex = end;
      lighter_write_data(data, 0);
      /* windex now at dst + (end-lead), correct. */
      data->lindex = prev_lindex;
      data->rindex = prev_rindex;
    }
  }
}

static inline void lighter_do_number_impl(LighterData* data, int64_t precision, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  (void)has_avx512;
  (void)has_avx2;
  (void)has_neon;
  (void)has_rvv;
  uint8_t* decimal = 0;
  uint8_t* exponent = 0;
  uint8_t* non_zero_start = 0;
  uint8_t* non_zero_finish = 0;
  uint8_t* exponent_start = 0;
  uint8_t* number_end = 0;
  int64_t exponent_value = 0;
  int64_t min_exponent = 0;
  int64_t max_exponent = 0;
  int64_t new_decimal = 0;
  int64_t new_exponent = 0;
  uint64_t digit_width = 0;
  uint64_t new_exponent_width = 0;
  int negative = 0;
  int negative_exponent = 0;
  uint64_t zeroes = 0;
  int exponent_saturated = 0;
  uint8_t* i;
  uint8_t* p_scan = data->rindex;
#if LIGHTER_PLATFORM_X86
  const __m256i avx2_zero = _mm256_set1_epi8('0');
  const __m256i avx2_dot = _mm256_set1_epi8('.');
  const __m256i avx2_e = _mm256_set1_epi8('e');
  const __m256i avx2_E = _mm256_set1_epi8('E');
#elif LIGHTER_PLATFORM_ARM64
  const uint8x16_t neon_zero = vdupq_n_u8('0');
  const uint8x16_t neon_dot = vdupq_n_u8('.');
  const uint8x16_t neon_e = vdupq_n_u8('e');
  const uint8x16_t neon_E = vdupq_n_u8('E');
#endif

  if (*p_scan == '-') {
    negative = 1;
    ++p_scan;
  }
  data->rindex = p_scan; /* update for later use in loops if needed */

  /* Fast path for common short integers: already canonical form, no reformatting.
   * Requires: first digit is '1'..'9', subsequent bytes up to a non-digit are all
   * digits (no '.', no 'e'/'E'), and precision is UNLIMITED so no rounding occurs.
   * Big win on integer-heavy workloads; small overhead on float-heavy ones. */
  if (LIGHTER_LIKELY(precision == LIGHTER_PRECISION_UNLIMITED && p_scan < data->data_end && (unsigned)(*p_scan - '1') < 9u)) {
    uint8_t* q = p_scan + 1;
    while (q < data->data_end && (unsigned)(*q - '0') <= 9u) {
      ++q;
    }
    if (LIGHTER_LIKELY(q >= data->data_end || (*q != '.' && *q != 'e' && *q != 'E'))) {
      data->rindex = q;
      return;
    }
  }

  /* Loop 1: Find decimal, exponent marker, and significant digit bounds */
  for (i = p_scan; i < data->data_end && !exponent && !number_end;) {
    /* SIMD acceleration for long significands */
#if LIGHTER_PLATFORM_X86
    if (has_avx2 && i + 32 <= data->data_end) {
      __m256i chunk = _mm256_loadu_si256((const __m256i*)i);
      __m256i m_digit = lighter_simd_is_digit_avx2(chunk);
      __m256i m_dot = _mm256_cmpeq_epi8(chunk, avx2_dot);
      __m256i m_exp = _mm256_or_si256(_mm256_cmpeq_epi8(chunk, avx2_e), _mm256_cmpeq_epi8(chunk, avx2_E));
      uint32_t mask_delimit = lighter_simd_mask_avx2(_mm256_or_si256(m_dot, m_exp));
      uint32_t mask_invalid = ~lighter_simd_mask_avx2(m_digit) & 0xFFFFFFFF;

      if (LIGHTER_UNLIKELY(mask_delimit || mask_invalid)) {
        /* Finding first action point */
        uint32_t first_action;
        if (mask_delimit && mask_invalid) {
          uint32_t d = lighter_simd_first_set_avx2(mask_delimit);
          uint32_t v = lighter_simd_first_set_avx2(mask_invalid);
          first_action = (d < v) ? d : v;
        } else {
          first_action = lighter_simd_first_set_avx2(mask_delimit ? mask_delimit : mask_invalid);
        }

        /* Process up to first_action for non-zero bounds */
        {
          __m256i m_nonzero = _mm256_andnot_si256(_mm256_cmpeq_epi8(chunk, avx2_zero), m_digit);
          uint32_t mask_nonzero = lighter_simd_mask_avx2(m_nonzero);
          if (mask_nonzero) {
            uint32_t bits = mask_nonzero & (uint32_t)((1ULL << first_action) - 1);
            if (bits) {
              if (LIGHTER_LIKELY(!non_zero_start)) {
                non_zero_start = i + lighter_simd_first_set_avx2(bits);
              }
              non_zero_finish = i + 31 - (uint32_t)__builtin_clz(bits);
            }
          }
        }
        i += first_action;
        /* Exit SIMD and let scalar handle the special character */
      } else {
        /* Fast skip: All are digits, update bounds */
        {
          __m256i m_nonzero = _mm256_andnot_si256(_mm256_cmpeq_epi8(chunk, avx2_zero), m_digit);
          uint32_t mask_nonzero = lighter_simd_mask_avx2(m_nonzero);
          if (mask_nonzero) {
            if (LIGHTER_LIKELY(!non_zero_start)) {
              non_zero_start = i + lighter_simd_first_set_avx2(mask_nonzero);
            }
            non_zero_finish = i + 31 - (uint32_t)__builtin_clz(mask_nonzero);
          }
        }
        i += 32;
        continue;
      }
    }
#elif LIGHTER_PLATFORM_ARM64
    if (has_neon && i + 16 <= data->data_end) {
      uint8x16_t chunk = vld1q_u8(i);
      uint8x16_t m_digit = lighter_simd_is_digit_neon(chunk);
      uint8x16_t m_dot = vceqq_u8(chunk, neon_dot);
      uint8x16_t m_exp = vorrq_u8(vceqq_u8(chunk, neon_e), vceqq_u8(chunk, neon_E));
      uint8x16_t m_delimit = vorrq_u8(m_dot, m_exp);
      uint8x16_t m_invalid = vmvnq_u8(m_digit);

      uint64_t mask_delimit = vgetq_lane_u64(vreinterpretq_u64_u8(m_delimit), 0) | vgetq_lane_u64(vreinterpretq_u64_u8(m_delimit), 1);
      uint64_t mask_invalid = vgetq_lane_u64(vreinterpretq_u64_u8(m_invalid), 0) | vgetq_lane_u64(vreinterpretq_u64_u8(m_invalid), 1);

      if (LIGHTER_UNLIKELY(mask_delimit || mask_invalid)) {
        /* Exit SIMD for simplicity on action point */
      } else {
        /* Fast skip: update bounds */
        {
          uint8x16_t m_nonzero = vbicq_u8(m_digit, vceqq_u8(chunk, neon_zero));
          uint64_t low = vgetq_lane_u64(vreinterpretq_u64_u8(m_nonzero), 0);
          uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(m_nonzero), 1);
          if (LIGHTER_LIKELY(!non_zero_start)) {
            if (low) {
              non_zero_start = i + lighter_simd_first_set_neon(low);
            } else if (high) {
              non_zero_start = i + lighter_simd_first_set_neon(high) + 8;
            }
          }
          if (high) {
            non_zero_finish = i + 15 - (__builtin_clzll(high) >> 3);
          } else if (low) {
            non_zero_finish = i + 7 - (__builtin_clzll(low) >> 3);
          }
        }
        i += 16;
        continue;
      }
    }
#elif LIGHTER_PLATFORM_RISCV
    if (has_rvv && i < data->data_end) {
      size_t n = (size_t)(data->data_end - i);
      size_t vl = __riscv_vsetvli(n, __RISCV_E8, __RISCV_M1, __RISCV_TA, __RISCV_MA);
      vuint8m1_t chunk = __riscv_vle8_v_u8m1(i, vl);
      vbool8_t m_digit = lighter_simd_is_digit_rvv(chunk, vl);
      vbool8_t m_dot = __riscv_vmseq_vx_u8m1_b8(chunk, '.', vl);
      vbool8_t m_exp = __riscv_vmor_mm_b8(__riscv_vmseq_vx_u8m1_b8(chunk, 'e', vl), __riscv_vmseq_vx_u8m1_b8(chunk, 'E', vl), vl);
      vbool8_t m_delimit = __riscv_vmor_mm_b8(m_dot, m_exp, vl);
      vbool8_t m_invalid = __riscv_vmnot_m_b8(m_digit, vl);
      intptr_t action = __riscv_vfirst_m_b8(__riscv_vmor_mm_b8(m_delimit, m_invalid, vl), vl);

      if (LIGHTER_UNLIKELY(action >= 0)) {
        {
          vbool8_t m_nonzero = __riscv_vmand_mm_b8(m_digit, __riscv_vmsne_vx_u8m1_b8(chunk, '0', vl), vl);
          intptr_t fnz = __riscv_vfirst_m_b8(m_nonzero, vl);
          if (fnz >= 0 && fnz < action) {
            if (LIGHTER_LIKELY(!non_zero_start)) {
              non_zero_start = i + fnz;
            }
            for (intptr_t j = action - 1; j >= fnz; --j) {
              if (i[j] >= '1' && i[j] <= '9') {
                non_zero_finish = i + j;
                break;
              }
            }
          }
        }
        i += action;
      } else {
        {
          vbool8_t m_nonzero = __riscv_vmand_mm_b8(m_digit, __riscv_vmsne_vx_u8m1_b8(chunk, '0', vl), vl);
          intptr_t fnz = __riscv_vfirst_m_b8(m_nonzero, vl);
          if (fnz >= 0) {
            if (LIGHTER_LIKELY(!non_zero_start)) {
              non_zero_start = i + fnz;
            }
            for (intptr_t j = (intptr_t)vl - 1; j >= fnz; --j) {
              if (i[j] >= '1' && i[j] <= '9') {
                non_zero_finish = i + j;
                break;
              }
            }
          }
        }
        i += vl;
        continue;
      }
    }
#endif

    switch (*i) {
      case '.':
        decimal = i;
        break;
      case 'e':
      case 'E':
        exponent = i;
        if (i + 1 < data->data_end) {
          switch (*(i + 1)) {
            case '-':
              negative_exponent = 1;
              /* fallthrough */
            case '+':
              ++i;
          }
        }
        break;
      case '0':
        break;
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        if (LIGHTER_LIKELY(!non_zero_start)) {
          non_zero_start = i;
        }
        non_zero_finish = i;
        break;
      default:
        number_end = i - 1;
    }
    if (!number_end) {
      i++;
    }
  }

  if (LIGHTER_UNLIKELY(!number_end && i < data->data_end)) {
    for (; i < data->data_end && !number_end;) {
#if LIGHTER_PLATFORM_X86
      if (has_avx2 && i + 32 <= data->data_end) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)i);
        uint32_t mask_invalid = ~lighter_simd_mask_avx2(lighter_simd_is_digit_avx2(chunk)) & 0xFFFFFFFF;
        if (LIGHTER_UNLIKELY(mask_invalid)) {
          uint32_t first_action = lighter_simd_first_set_avx2(mask_invalid);
          if (!exponent_start && first_action > 0) {
            exponent_start = i;
          }
          i += first_action;
          number_end = i - 1;
          break;
        } else {
          if (!exponent_start) {
            exponent_start = i;
          }
          i += 32;
          continue;
        }
      }
#elif LIGHTER_PLATFORM_ARM64
      if (has_neon && i + 16 <= data->data_end) {
        uint8x16_t chunk = vld1q_u8(i);
        uint8x16_t m_digit = lighter_simd_is_digit_neon(chunk);
        uint8x16_t m_invalid = vmvnq_u8(m_digit);
        uint64_t low = vgetq_lane_u64(vreinterpretq_u64_u8(m_invalid), 0);
        uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(m_invalid), 1);
        if (LIGHTER_UNLIKELY(low || high)) {
          uint64_t first_action = low ? lighter_simd_first_set_neon(low) : lighter_simd_first_set_neon(high) + 8;
          if (!exponent_start && first_action > 0) {
            exponent_start = i;
          }
          i += first_action;
          number_end = i - 1;
          break;
        } else {
          if (!exponent_start) {
            exponent_start = i;
          }
          i += 16;
          continue;
        }
      }
#elif LIGHTER_PLATFORM_RISCV
      if (has_rvv && i < data->data_end) {
        size_t n = (size_t)(data->data_end - i);
        size_t vl = __riscv_vsetvli(n, __RISCV_E8, __RISCV_M1, __RISCV_TA, __RISCV_MA);
        vuint8m1_t chunk = __riscv_vle8_v_u8m1(i, vl);
        vbool8_t m_digit = lighter_simd_is_digit_rvv(chunk, vl);
        intptr_t invalid = __riscv_vfirst_m_b8(__riscv_vmnot_m_b8(m_digit, vl), vl);
        if (LIGHTER_UNLIKELY(invalid >= 0)) {
          if (!exponent_start && invalid > 0) {
            exponent_start = i;
          }
          i += invalid;
          number_end = i - 1;
          break;
        } else {
          if (!exponent_start) {
            exponent_start = i;
          }
          i += vl;
          continue;
        }
      }
#endif
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
          if (!exponent_start) {
            exponent_start = i;
          }
          break;
        default:
          number_end = i - 1;
      }
      if (!number_end) {
        i++;
      }
    }
  }
  if (!number_end) {
    number_end = data->data_end - 1;
  }
  if (LIGHTER_UNLIKELY(!non_zero_start)) {
    /* Value is zero. Emit just '0' (sign dropped for -0) and skip the whole source number. */
    if (negative) {
      --(data->rindex);
    }
    lighter_write_data(data, number_end + 1 - data->rindex);
    *data->windex++ = '0';
    return;
  }
  if (!exponent_start) {
    exponent_start = number_end;
  }
  uint8_t* p;
  if (exponent) {
    p = exponent + 1;
    if (p < data->data_end && (*p == '+' || *p == '-')) {
      ++p;
    }
    while (p < data->data_end && *p == '0') {
      ++p;
    }
    exponent_start = p;
    for (i = (uint8_t*)exponent_start; i <= number_end; ++i) {
      if (LIGHTER_UNLIKELY(exponent_value > 922337203685477580LL || (exponent_value == 922337203685477580LL && (*i - '0') > 7))) {
        exponent_value = 9223372036854775807LL;
        exponent_saturated = 1;
        break; /* further digits don't change the saturated value */
      }
      exponent_value = exponent_value * 10 + (*i - '0');
    }
  }
  if (negative_exponent) {
    exponent_value *= -1;
  }
  int is_huge = exponent_saturated;
  if (LIGHTER_UNLIKELY(is_huge)) {
    /* Already saturated; delta arithmetic is irrelevant. */
    max_exponent = exponent_value;
    min_exponent = exponent_value;
  } else {
    int64_t delta_max = (int64_t)(decimal ? decimal > non_zero_start ? decimal - 1 : decimal : exponent ? exponent - 1 : number_end) - (int64_t)non_zero_start;
    int64_t delta_min = (int64_t)(decimal    ? decimal > non_zero_finish ? decimal - 1 : decimal
                                  : exponent ? exponent - 1
                                             : number_end) -
                        (int64_t)non_zero_finish;
    if (LIGHTER_UNLIKELY(LIGHTER_ADD_OVERFLOW(exponent_value, delta_max, &max_exponent))) {
      is_huge = 1;
    }
    if (LIGHTER_UNLIKELY(LIGHTER_ADD_OVERFLOW(exponent_value, delta_min, &min_exponent))) {
      is_huge = 1;
    }
  }

  if (LIGHTER_LIKELY(precision == LIGHTER_PRECISION_UNLIMITED)) {
    /* Skip rounding */
  } else if (is_huge) {
    /* Huge exponent handling for rounding */
    if (exponent_value < 0) {
      /* Effectively rounds to zero if precision is within int64_t limits */
      if (negative) {
        --(data->rindex);
      }
      lighter_write_data(data, number_end + 1 - data->rindex);
      *data->windex++ = '0';
      return;
    }
    /* Else huge positive: rounding point is infinitely far to the right, so it's a no-op on digits */
  } else {
    if (-precision > max_exponent) {
      if (negative) {
        --(data->rindex);
      }
      lighter_write_data(data, number_end + 1 - data->rindex);
      *data->windex++ = '0';
      return;
    }
    if (-precision > min_exponent) {
      min_exponent = -precision;
      /* Compute the rounding-cut byte position from a known-safe anchor.
       * Clamp i into [non_zero_start - 1, non_zero_finish] to protect the *(i+1) read below. */
      uint8_t* anchor = decimal ? (decimal > non_zero_start ? decimal - 1 : decimal) : exponent ? exponent - 1 : number_end;
      int64_t shift = precision + exponent_value;
      ptrdiff_t max_shift = (ptrdiff_t)(non_zero_finish - anchor);
      ptrdiff_t min_shift = (ptrdiff_t)(non_zero_start - 1 - anchor);
      if (shift > max_shift) {
        shift = max_shift;
      } else if (shift < min_shift) {
        shift = min_shift;
      }
      i = anchor + shift;
      if (i < non_zero_finish) {
        if (*(i + 1) >= '5') {
          for (; i >= non_zero_start; --i) {
            if (*i == '9') {
              ++min_exponent;
            } else if (*i != '.') {
              ++*i;
              break;
            }
          }
          if (i < non_zero_start) {
            *(++i) = '1';
            ++max_exponent;
          }
        }
        while (i >= non_zero_start && *i == '0') {
          --i;
          ++min_exponent;
        }
      }
      non_zero_finish = i;
    }
  }

  digit_width = max_exponent - min_exponent + 1;
  if (min_exponent > 0) {
    zeroes = min_exponent;
  } else if (max_exponent < 0) {
    zeroes = -max_exponent;
  }
  if (zeroes < 3) {
    if (min_exponent < 0) {
      new_decimal = max_exponent >= 0 ? max_exponent + 1 : 1;
    }
  } else {
    new_exponent = min_exponent;
    zeroes = 0;
  }
  if (is_huge) {
    new_exponent = 1; /* Trigger scientific notation output path */
    zeroes = 0;
  }
  if (LIGHTER_UNLIKELY(non_zero_start > data->rindex)) {
    lighter_write_data(data, non_zero_start - data->rindex);
  }
  if (LIGHTER_LIKELY(!is_huge && decimal == data->rindex + (ptrdiff_t)new_decimal && exponent_value == new_exponent)) {
    /* Source layout already matches target: just advance rindex past the digits. */
    data->rindex += zeroes + digit_width + (decimal ? 1 : 0);
  } else if (zeroes && max_exponent < 0) {
    /* Original algorithm from commit 6812f60, with a leading flush added so any
     * pending '-' isn't overwritten by the '0' or '.' back-fill below. */
    if (data->lindex < data->rindex) {
      lighter_write_data(data, 0);
    }
    i = data->windex;
    data->windex += zeroes + 1;
    if (non_zero_start < decimal && non_zero_finish > decimal) {
      lighter_write_data(data, decimal - non_zero_start + 1);
      data->rindex = non_zero_finish + 1;
      data->windex += decimal - non_zero_start;
      lighter_write_data(data, -(ptrdiff_t)digit_width - 1);
      data->rindex = decimal;
      data->windex = i + zeroes + 1;
      lighter_write_data(data, non_zero_finish - decimal);
      data->windex += non_zero_finish - decimal;
    } else {
      data->rindex = non_zero_finish + 1;
    }
    lighter_write_data(data, 0);
    *i++ = '0';
    *i++ = '.';
    if (zeroes > 1) {
      *i = '0';
    }
  } else {
    /* Original algorithm from commit 6812f60. */
    if (decimal) {
      if ((!new_decimal && non_zero_start < decimal && non_zero_finish > decimal) || (new_decimal && non_zero_start + new_decimal > decimal)) {
        data->rindex = decimal;
        lighter_write_data(data, 1);
      } else if (new_decimal && decimal && non_zero_start + new_decimal < decimal) {
        data->rindex = non_zero_start + new_decimal;
        lighter_write_data(data, 0);
        i = data->windex++;
        data->rindex = decimal;
        lighter_write_data(data, 1);
        *i = '.';
      }
    }
    if (new_decimal && (!decimal || non_zero_start + new_decimal > decimal)) {
      data->rindex = non_zero_start + new_decimal + (decimal ? 1 : 0);
      lighter_write_data(data, 0);
      i = data->windex++;
      data->rindex = non_zero_finish + 1;
      lighter_write_data(data, 0);
      *i = '.';
    } else {
      data->rindex = non_zero_finish + 1;
    }
    if (zeroes) {
      if (non_zero_finish + 1 + zeroes == (exponent ? exponent : number_end)) {
        data->rindex += zeroes;
      } else {
        lighter_write_data(data, 0);
        *data->windex++ = '0';
        if (zeroes > 1) {
          *data->windex++ = '0';
        }
      }
    }
  }
  if (LIGHTER_UNLIKELY(exponent > data->rindex)) {
    lighter_write_data(data, exponent - data->rindex);
  }
  if (LIGHTER_UNLIKELY(new_exponent || is_huge)) {
    if (is_huge) {
      /* Positional minimal length reformatting for huge exponents */
      lighter_write_data(data, non_zero_finish + 1 - data->rindex);
      *data->windex++ = 'E';
      int64_t shift =
          (int64_t)(decimal ? (decimal > non_zero_finish ? decimal - 1 : decimal) : (exponent ? exponent - 1 : number_end)) - (int64_t)non_zero_finish;
      lighter_write_adjusted_exponent(data, exponent_start, number_end - exponent_start + 1, negative_exponent, shift);
      data->rindex = number_end + 1;
      data->lindex = data->rindex;
    } else {
      int64_t temp = new_exponent < 0 ? -new_exponent : new_exponent;
      while (temp) {
        temp /= 10;
        ++new_exponent_width;
      }
      if (new_exponent == 0) {
        new_exponent_width = 1;
      }

      lighter_write_data(data, (exponent_start && exponent_start > data->rindex) ? exponent_start - data->rindex : 0);
      *data->windex++ = 'E';
      if (new_exponent < 0) {
        *data->windex++ = '-';
      }
      if (new_exponent == exponent_value) {
        data->lindex = data->rindex;
        data->rindex += new_exponent_width;
        lighter_write_data(data, 0);
      } else {
        data->windex += new_exponent_width - 1;
        if (new_exponent < 0) {
          new_exponent = -new_exponent;
        }
        while (new_exponent) {
          *data->windex-- = new_exponent % 10 + '0';
          new_exponent /= 10;
        }
        data->windex += new_exponent_width + 1;
        data->rindex = number_end + 1;
        data->lindex = data->rindex;
      }
    }
  }
  /* Advance past the last byte of the number so the caller's outer loop doesn't
   * re-enter do_number on a leftover exponent digit. */
  if (number_end + 1 > data->rindex) {
    lighter_write_data(data, (number_end + 1) - data->rindex);
  }
}

/** Parse and optionally reformat a JSON number. precision: LIGHTER_PRECISION_UNLIMITED = preserve form. */
static inline void lighter_do_number(LighterData* data, int64_t precision) {
  lighter_do_number_impl(data, precision, lighter_cpu_supports_avx512bw(), lighter_cpu_supports_avx2(), lighter_cpu_supports_neon(),
                         lighter_cpu_supports_rvv());
}

#endif /* LIGHTER_NUMBER_H */
