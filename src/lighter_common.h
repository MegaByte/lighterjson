/**
 * @file   lighter_common.h
 * @brief  Shared types for lighter library modules (string, number).
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

static inline int lighter_has_zero_byte(uint64_t v) {
  return ((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL) != 0;
}

static inline int lighter_has_byte(uint64_t v, uint8_t c) {
  uint64_t mask = 0x0101010101010101ULL * c;
  return lighter_has_zero_byte(v ^ mask);
}

/** Copy pending segment [lindex, rindex) and advance by index_offset. */
static inline void lighter_write_data(LighterData* data, ptrdiff_t index_offset) {
  memmove(data->windex, data->lindex, (size_t)(data->rindex - data->lindex));
  data->windex += data->rindex - data->lindex;
  data->rindex += index_offset;
  data->lindex = data->rindex;
}

/** Portable signed 64-bit addition overflow check. */
#if defined(__GNUC__) || defined(__clang__)
  #define LIGHTER_ADD_OVERFLOW(a, b, res) __builtin_add_overflow(a, b, res)
#else
static inline int LIGHTER_ADD_OVERFLOW(int64_t a, int64_t b, int64_t* res) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
    return 1;
  }
  *res = a + b;
  return 0;
}
#endif

#endif /* LIGHTER_COMMON_H */
