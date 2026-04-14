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

/* bit_level/byte_level point at the NEXT free slot. After push, the most recently
 * written bit is at (prev position). `current` caches that bit. Empty stack has current=-1. */
static inline void increment_bit(Bitfield* bitfield) {
  if (bitfield->bit_level == sizeof(uint64_t) * CHAR_BIT - 1) {
    bitfield->bit_level = 0;
    ++(bitfield->byte_level);
    if (bitfield->byte_level >= bitfield->size) {
      /* Grow by doubling; use realloc for heap buffer (but not for the embedded initial slot). */
      size_t new_size = bitfield->size * 2;
      uint64_t* new_bits;
      if (bitfield->bits == &bitfield->initial_bits) {
        new_bits = (uint64_t*)malloc(new_size * sizeof(uint64_t));
        if (!new_bits) {
          return; /* out of memory: silently cap the stack */
        }
        new_bits[0] = bitfield->initial_bits;
      } else {
        new_bits = (uint64_t*)realloc(bitfield->bits, new_size * sizeof(uint64_t));
        if (!new_bits) {
          return;
        }
      }
      /* Zero new slots. */
      for (size_t i = bitfield->size; i < new_size; ++i) {
        new_bits[i] = 0;
      }
      bitfield->bits = new_bits;
      bitfield->size = new_size;
    }
  } else {
    ++(bitfield->bit_level);
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
  /* Move back one position. */
  if (bitfield->bit_level == 0) {
    if (bitfield->byte_level == 0) {
      /* Already empty. */
      bitfield->current = (size_t)-1;
      return;
    }
    --(bitfield->byte_level);
    bitfield->bit_level = sizeof(uint64_t) * CHAR_BIT - 1;
  } else {
    --(bitfield->bit_level);
  }
  /* Now bit_level/byte_level point to the just-popped position. The new top is
   * the position before that. */
  if (bitfield->bit_level == 0 && bitfield->byte_level == 0) {
    bitfield->current = (size_t)-1;
  } else if (bitfield->bit_level == 0) {
    /* Previous position is in prior word at the MSB. */
    bitfield->current = (bitfield->bits[bitfield->byte_level - 1] >> (sizeof(uint64_t) * CHAR_BIT - 1)) & 1;
  } else {
    bitfield->current = (bitfield->bits[bitfield->byte_level] >> (bitfield->bit_level - 1)) & 1;
  }
}

#endif /* LIGHTER_BITFIELD_H */
