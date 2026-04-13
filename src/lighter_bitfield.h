/**
 * @file   lighter_bitfield.h
 * @brief  Bit stack for tracking array/object nesting (used by JSON parser).
 */

#ifndef LIGHTER_BITFIELD_H
#define LIGHTER_BITFIELD_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct Bitfield {
  size_t size;
  uint64_t initial_bits;
  uint64_t* bits;
  size_t bit_level;
  size_t byte_level;
  size_t current;
} Bitfield;

typedef enum Container {
  None = -1,
  Array,
  Object
} Container;

static inline void init_bits(Bitfield* bitfield) {
  bitfield->size = 1; /* number of uint64_t elements */
  bitfield->initial_bits = 0;
  bitfield->bits = &bitfield->initial_bits;
  bitfield->bit_level = 0;
  bitfield->byte_level = 0;
  bitfield->current = -1;
}

static inline void increment_bit(Bitfield* bitfield) {
  if (bitfield->bit_level == sizeof(uint64_t) * CHAR_BIT - 1) {
    bitfield->bit_level = 0;
    ++(bitfield->byte_level);
  } else {
    ++(bitfield->bit_level);
  }
  if (bitfield->byte_level >= bitfield->size) {
    size_t new_size = bitfield->size + 1;
    uint64_t* new_bits = (uint64_t*)malloc(new_size * sizeof(uint64_t));
    if (bitfield->bits == &bitfield->initial_bits) {
      new_bits[0] = bitfield->initial_bits;
    } else {
      for (size_t i = 0; i < bitfield->size; ++i) {
        new_bits[i] = bitfield->bits[i];
      }
      free(bitfield->bits);
    }
    new_bits[new_size - 1] = 0;
    bitfield->bits = new_bits;
    bitfield->size = new_size;
  }
}

static inline void push_set_bit(Bitfield* bitfield) {
  bitfield->current = 1;
  bitfield->bits[bitfield->byte_level] |= (1ULL << bitfield->bit_level);
  increment_bit(bitfield);
}

static inline void push_clear_bit(Bitfield* bitfield) {
  bitfield->current = 0;
  bitfield->bits[bitfield->byte_level] &= ~(1ULL << bitfield->bit_level);
  increment_bit(bitfield);
}

static inline void pop_bit(Bitfield* bitfield) {
  if (bitfield->bit_level == 0) {
    if (bitfield->byte_level > 0) {
      --(bitfield->byte_level);
      bitfield->bit_level = sizeof(uint64_t) * CHAR_BIT - 1;
    }
  } else {
    --(bitfield->bit_level);
  }
  bitfield->current = bitfield->bit_level > 0 || bitfield->byte_level > 0 ? (bitfield->bits[bitfield->byte_level] >> (bitfield->bit_level - 1)) & 1 : -1;
}

#endif /* LIGHTER_BITFIELD_H */
