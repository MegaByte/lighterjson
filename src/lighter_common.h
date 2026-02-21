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

/** Copy pending segment [lindex, rindex) and advance by index_offset. */
static inline void lighter_write_data(LighterData* data, ptrdiff_t index_offset) {
  memmove(data->windex, data->lindex, (size_t)(data->rindex - data->lindex));
  data->windex += data->rindex - data->lindex;
  data->rindex += index_offset;
  data->lindex = data->rindex;
}

#endif /* LIGHTER_COMMON_H */
