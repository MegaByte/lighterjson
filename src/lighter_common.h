/**
 * @file   lighter_common.h
 * @brief  Shared data structures and helpers for parser modules.
 */

#ifndef LIGHTER_COMMON_H
#define LIGHTER_COMMON_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** Value for "preserve number form" (no rounding); use for precision. */
#define LIGHTER_PRECISION_UNLIMITED INT64_MAX

typedef struct LighterData {
  uint8_t* data_start;
  uint8_t* rindex;
  uint8_t* windex;
  uint8_t* lindex;
  uint8_t* data_end;
} LighterData;

/** Copy pending segment [lindex, rindex) and advance by index_offset. */
static inline void lighter_write_data(LighterData* data, ptrdiff_t index_offset) {
  ptrdiff_t pending = data->rindex - data->lindex;
  if (pending) {
    if (data->windex != data->lindex) {
      memmove(data->windex, data->lindex, (size_t)pending);
    }
    data->windex += pending;
  }
  data->rindex += index_offset;
  data->lindex = data->rindex;
}

/** Return non-zero when cp is a valid Unicode scalar value. */
static inline int lighter_is_unicode_scalar(uint32_t cp) {
  return cp <= 0x10FFFFu && (cp < 0xD800u || cp > 0xDFFFu);
}

/** Encode one Unicode scalar into UTF-8 and return the updated dst pointer. */
static inline uint8_t* lighter_write_utf8_scalar(uint8_t* dst, uint32_t cp) {
  if (!lighter_is_unicode_scalar(cp)) {
    return dst;
  }
  if (cp < 0x80u) {
    *dst++ = (uint8_t)cp;
    return dst;
  }
  if (cp < 0x800u) {
    *dst++ = (uint8_t)(0xC0u | (cp >> 6));
    *dst++ = (uint8_t)(0x80u | (cp & 0x3Fu));
    return dst;
  }
  if (cp < 0x10000u) {
    *dst++ = (uint8_t)(0xE0u | (cp >> 12));
    *dst++ = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    *dst++ = (uint8_t)(0x80u | (cp & 0x3Fu));
    return dst;
  }
  *dst++ = (uint8_t)(0xF0u | (cp >> 18));
  *dst++ = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
  *dst++ = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
  *dst++ = (uint8_t)(0x80u | (cp & 0x3Fu));
  return dst;
}

/** Portable signed 64-bit addition overflow check. */
#if defined(__GNUC__) || defined(__clang__)
  #define lighter_add_overflow(a, b, res) __builtin_add_overflow(a, b, res)
  #define LIGHTER_LIKELY(x) __builtin_expect(!!(x), 1)
  #define LIGHTER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define LIGHTER_LIKELY(x) (x)
  #define LIGHTER_UNLIKELY(x) (x)
static inline int lighter_add_overflow(int64_t a, int64_t b, int64_t* res) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
    return 1;
  }
  *res = a + b;
  return 0;
}
#endif

#endif /* LIGHTER_COMMON_H */
