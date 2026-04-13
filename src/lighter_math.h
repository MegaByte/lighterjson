#ifndef LIGHTER_MATH_H
#define LIGHTER_MATH_H

#include <stdint.h>

/** Binary search magnitude detection for uint64_t digits. */
static inline uint32_t lighter_digits_u64(uint64_t n) {
  if (n < 10000000000ULL) {
    if (n < 100000) {
      if (n < 100) {
        if (n < 10) {
          return 1;
        }
        return 2;
      }
      if (n < 1000) {
        return 3;
      }
      if (n < 10000) {
        return 4;
      }
      return 5;
    }
    if (n < 10000000) {
      if (n < 1000000) {
        return 6;
      }
      return 7;
    }
    if (n < 100000000) {
      return 8;
    }
    if (n < 1000000000) {
      return 9;
    }
    return 10;
  }
  if (n < 1000000000000000ULL) {
    if (n < 1000000000000ULL) {
      if (n < 100000000000ULL) {
        return 11;
      }
      return 12;
    }
    if (n < 10000000000000ULL) {
      return 13;
    }
    if (n < 100000000000000ULL) {
      return 14;
    }
    return 15;
  }
  if (n < 100000000000000000ULL) {
    if (n < 10000000000000000ULL) {
      return 16;
    }
    return 17;
  }
  if (n < 1000000000000000000ULL) {
    return 18;
  }
  if (n < 10000000000000000000ULL) {
    return 19;
  }
  return 20;
}

#endif /* LIGHTER_MATH_H */
