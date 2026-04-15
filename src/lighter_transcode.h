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

/** Transcode UTF-16/32 to UTF-8. src and dst can overlap if dst < src. */
static inline void lighter_transcode_to_utf8(const uint8_t* src, size_t src_size, uint8_t* dst, LighterEncoding enc, size_t* out_size) {
  uint8_t* d = dst;
  const uint8_t* s = src;
  const uint8_t* end = src + src_size;

  if (enc == LIGHTER_ENC_UTF32LE || enc == LIGHTER_ENC_UTF32BE) {
    while (s + 4 <= end) {
      uint32_t cp;
      if (enc == LIGHTER_ENC_UTF32LE) {
        cp = s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
      } else {
        cp = (s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3];
      }
      s += 4;
      d = lighter_write_utf8_scalar(d, cp);
    }
  } else if (enc == LIGHTER_ENC_UTF16LE || enc == LIGHTER_ENC_UTF16BE) {
    while (s + 2 <= end) {
      uint32_t cp;
      if (enc == LIGHTER_ENC_UTF16LE) {
        cp = s[0] | (s[1] << 8);
      } else {
        cp = (s[0] << 8) | s[1];
      }
      s += 2;
      if (cp >= 0xD800 && cp <= 0xDBFF && s + 2 <= end) {
        uint32_t low;
        if (enc == LIGHTER_ENC_UTF16LE) {
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

/** In-place/backward UTF-8 transcode. dst_end points one byte past the output range. */
static inline void lighter_transcode_from_utf8_backward(const uint8_t* src, size_t src_size, LighterEncoding enc, uint8_t* dst_end) {
  const uint8_t* start = src;
  const uint8_t* s = src + src_size;
  uint8_t* d = dst_end;

  while (s > start) {
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
