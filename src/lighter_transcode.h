/**
 * @file      lighter_transcode.h
 * @brief     Encoding detection and transcoding for LighterJSON
 */
#ifndef LIGHTER_TRANSCODE_H
#define LIGHTER_TRANSCODE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum LighterEncoding {
  LIGHTER_ENC_UTF8,
  LIGHTER_ENC_UTF16LE,
  LIGHTER_ENC_UTF16BE,
  LIGHTER_ENC_UTF32LE,
  LIGHTER_ENC_UTF32BE
} LighterEncoding;

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
      if (cp <= 0x7F) {
        *d++ = (uint8_t)cp;
      } else if (cp <= 0x7FF) {
        *d++ = 0xC0 | (cp >> 6);
        *d++ = 0x80 | (cp & 0x3F);
      } else if (cp <= 0xFFFF) {
        *d++ = 0xE0 | (cp >> 12);
        *d++ = 0x80 | ((cp >> 6) & 0x3F);
        *d++ = 0x80 | (cp & 0x3F);
      } else if (cp <= 0x10FFFF) {
        *d++ = 0xF0 | (cp >> 18);
        *d++ = 0x80 | ((cp >> 12) & 0x3F);
        *d++ = 0x80 | ((cp >> 6) & 0x3F);
        *d++ = 0x80 | (cp & 0x3F);
      }
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
        }
      }
      if (cp <= 0x7F) {
        *d++ = (uint8_t)cp;
      } else if (cp <= 0x7FF) {
        *d++ = 0xC0 | (cp >> 6);
        *d++ = 0x80 | (cp & 0x3F);
      } else if (cp <= 0xFFFF) {
        *d++ = 0xE0 | (cp >> 12);
        *d++ = 0x80 | ((cp >> 6) & 0x3F);
        *d++ = 0x80 | (cp & 0x3F);
      } else if (cp <= 0x10FFFF) {
        *d++ = 0xF0 | (cp >> 18);
        *d++ = 0x80 | ((cp >> 12) & 0x3F);
        *d++ = 0x80 | ((cp >> 6) & 0x3F);
        *d++ = 0x80 | (cp & 0x3F);
      }
    }
  }
  *out_size = (size_t)(d - dst);
}

/** Transcode UTF-8 BACK to original encoding if possible. */
static inline void lighter_transcode_from_utf8(const uint8_t* src, size_t src_size, LighterEncoding enc, uint8_t* dst, size_t* out_size) {
  if (enc == LIGHTER_ENC_UTF8) {
    memcpy(dst, src, src_size);
    *out_size = src_size;
    return;
  }
  uint8_t* d = dst;
  const uint8_t* s = src;
  const uint8_t* end = src + src_size;
  while (s < end) {
    uint32_t cp;
    if (s[0] <= 0x7F) {
      cp = *s++;
    } else if ((s[0] & 0xE0) == 0xC0 && s + 1 < end) {
      cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
      s += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && s + 2 < end) {
      cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
      s += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && s + 3 < end) {
      cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
      s += 4;
    } else {
      s++;
      continue;
    }

    if (enc == LIGHTER_ENC_UTF32LE) {
      *d++ = cp & 0xFF;
      *d++ = (cp >> 8) & 0xFF;
      *d++ = (cp >> 16) & 0xFF;
      *d++ = (cp >> 24) & 0xFF;
    } else if (enc == LIGHTER_ENC_UTF32BE) {
      *d++ = (cp >> 24) & 0xFF;
      *d++ = (cp >> 16) & 0xFF;
      *d++ = (cp >> 8) & 0xFF;
      *d++ = cp & 0xFF;
    } else if (enc == LIGHTER_ENC_UTF16LE || enc == LIGHTER_ENC_UTF16BE) {
      if (cp <= 0xFFFF) {
        if (enc == LIGHTER_ENC_UTF16LE) {
          *d++ = cp & 0xFF;
          *d++ = (cp >> 8) & 0xFF;
        } else {
          *d++ = (cp >> 8) & 0xFF;
          *d++ = cp & 0xFF;
        }
      } else {
        uint32_t h = 0xD800 + ((cp - 0x10000) >> 10);
        uint32_t l = 0xDC00 + ((cp - 0x10000) & 0x3FF);
        if (enc == LIGHTER_ENC_UTF16LE) {
          *d++ = h & 0xFF;
          *d++ = (h >> 8) & 0xFF;
          *d++ = l & 0xFF;
          *d++ = (l >> 8) & 0xFF;
        } else {
          *d++ = (h >> 8) & 0xFF;
          *d++ = h & 0xFF;
          *d++ = (l >> 8) & 0xFF;
          *d++ = l & 0xFF;
        }
      }
    }
  }
  *out_size = (size_t)(d - dst);
}

#endif
