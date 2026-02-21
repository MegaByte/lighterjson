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
#include "unicode_nfc_shared.h"
#include "unicode_nfc_runtime.h"

/* \uXXXX = 4 hex digits; UTF-8 and surrogate boundaries (Unicode). */
#define UNICODE_ESCAPE_HEX_LEN  4
#define UTF8_ASCII_MAX           0x80u
#define UTF8_2BYTE_MAX           0x800u
#define UTF8_3BYTE_MAX           0x10000u
#define SURROGATE_HIGH_START     0xD800u
#define SURROGATE_LOW_START      0xDC00u
#define SURROGATE_MASK           0x3FFu
#define SURROGATE_OFFSET         0x10000u

static inline uint64_t lighter_string_hex_value(LighterData* data) {
  uint64_t value = 0;
  if (data->rindex + UNICODE_ESCAPE_HEX_LEN > data->data_end) {
    return (uint64_t)INT64_MAX;
  }
  for (size_t i = 0; i < UNICODE_ESCAPE_HEX_LEN; ++i) {
    const uint64_t x = data->rindex[i];
    const uint64_t shift = (UNICODE_ESCAPE_HEX_LEN - 1 - i) << 2;
    if (x >= '0' && x <= '9') {
      value += ((x - '0') << shift);
    } else if (x >= 'A' && x <= 'F') {
      value += ((x - 'A' + 10) << shift);
    } else if (x >= 'a' && x <= 'f') {
      value += ((x - 'a' + 10) << shift);
    } else {
      return (uint64_t)INT64_MAX;
    }
  }
  return value;
}

static inline void lighter_string_do_unicode(LighterData* data) {
  uint64_t value = lighter_string_hex_value(data);
  uint64_t value2 = 0;
  if (value == (uint64_t)INT64_MAX) {
    fprintf(stderr, "INVALID HEX\n");
    return;
  }
  if (value < 0x20) {  /* C0 controls */
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
        --(data->lindex);
        data->rindex += 4;
        return;
    }
    lighter_write_data(data, UNICODE_ESCAPE_HEX_LEN);
    return;
  }
  lighter_write_data(data, UNICODE_ESCAPE_HEX_LEN);
  if (value < UTF8_ASCII_MAX) {
    *data->windex++ = (uint8_t)value;
    return;
  }
  if (value < UTF8_2BYTE_MAX) {
    *data->windex++ = ((uint8_t)(value >> 6) & 0x1F) | 0xC0;
    *data->windex++ = ((uint8_t)(value & 0x3F)) | 0x80;
    return;
  }
  if (value >= SURROGATE_HIGH_START) {  /* possible high surrogate; check for pair */
    value2 = lighter_string_hex_value(data);
    if (value2 == (uint64_t)INT64_MAX) {
      fprintf(stderr, "INVALID HEX\n");
      return;
    }
    if (value2 >= SURROGATE_LOW_START && value2 <= SURROGATE_LOW_START + SURROGATE_MASK) {  /* low surrogate */
      value = (((value & SURROGATE_MASK) << 10) | (value2 & SURROGATE_MASK)) + SURROGATE_OFFSET;
    }
    lighter_write_data(data, UNICODE_ESCAPE_HEX_LEN);
  }
  if (value < UTF8_3BYTE_MAX) {
    *data->windex++ = ((uint8_t)(value >> 12) & 0xF) | 0xE0;
    *data->windex++ = (uint8_t)(value >> 6) & 0x3F;
    *data->windex++ = ((uint8_t)value & 0x3F) | 0x80;
  } else {
    *data->windex++ = ((uint8_t)(value >> 18) & 0x7) | 0xF0;
    *data->windex++ = ((uint8_t)(value >> 12) & 0x3F) | 0x80;
    *data->windex++ = ((uint8_t)(value >> 6) & 0x3F) | 0x80;
    *data->windex++ = ((uint8_t)value & 0x3F) | 0x80;
  }
}

static inline void lighter_string_do_escape(LighterData* data) {
  if (data->rindex + 1 < data->data_end) {
    switch (data->rindex[1]) {
      case 'u':
        lighter_write_data(data, 2);  /* \u */
        lighter_string_do_unicode(data);
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
}

/** Parse and optionally NFC-normalize a JSON string. NFC data is loaded on first use
 *  when a string ends. Uses lighter_write_data to flush segments. */
static inline void lighter_do_string(LighterData* data) {
  ++(data->rindex);
  while (data->rindex < data->data_end) {
    switch (*data->rindex) {
      case '\\':
        lighter_string_do_escape(data);
        break;
      case '"': {
        if (nfc_quick_check("lighter.nfc", data->lindex + 1, data->rindex) != NFC_QC_YES) {
          ptrdiff_t pending = data->rindex - data->lindex;
          lighter_write_data(data, 0);
          uint8_t* str_content_start = data->windex - pending + 1;
          uint8_t* str_content_end = data->windex - 1;
          uint8_t* new_end = nfc_normalize_utf8_incremental(
              nfc_get_or_load("lighter.nfc"), str_content_start, str_content_end);
          memmove(new_end, data->windex - 1, 1);
          data->windex = new_end + 1;
        } else {
          ++(data->rindex);
        }
        return;
      }
      default:
        ++(data->rindex);
    }
  }
}

#endif /* LIGHTER_STRING_H */
