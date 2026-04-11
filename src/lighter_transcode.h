/**
 * @file   lighter_transcode.h
 * @brief  Lightweight, multi-encoding transcoding (UTF-16/32 <-> UTF-8).
 */

#ifndef LIGHTER_TRANSCODE_H
#define LIGHTER_TRANSCODE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
  LIGHTER_ENC_UTF8,
  LIGHTER_ENC_UTF8_BOM,
  LIGHTER_ENC_UTF16LE,
  LIGHTER_ENC_UTF16BE,
  LIGHTER_ENC_UTF32LE,
  LIGHTER_ENC_UTF32BE,
  LIGHTER_ENC_UNKNOWN
} LighterEncoding;

static inline LighterEncoding lighter_detect_encoding(const uint8_t* data, size_t size) {
  if (size >= 4) {
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xFE && data[3] == 0xFF)
      return LIGHTER_ENC_UTF32BE;
    if (data[0] == 0xFF && data[1] == 0xFE && data[2] == 0x00 && data[3] == 0x00)
      return LIGHTER_ENC_UTF32LE;
  }
  if (size >= 3) {
    if (data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
      return LIGHTER_ENC_UTF8_BOM;
  }
  if (size >= 2) {
    if (data[0] == 0xFE && data[1] == 0xFF)
      return LIGHTER_ENC_UTF16BE;
    if (data[0] == 0xFF && data[1] == 0xFE)
      return LIGHTER_ENC_UTF16LE;
  }
  return LIGHTER_ENC_UTF8;
}

static inline int lighter_encode_utf8_single(uint32_t cp, uint8_t* out) {
  if (cp <= 0x7F) {
    if (out)
      out[0] = (uint8_t)cp;
    return 1;
  } else if (cp <= 0x7FF) {
    if (out) {
      out[0] = (uint8_t)(0xC0 | (cp >> 6));
      out[1] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    return 2;
  } else if (cp <= 0xFFFF) {
    if (out) {
      out[0] = (uint8_t)(0xE0 | (cp >> 12));
      out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    return 3;
  } else if (cp <= 0x10FFFF) {
    if (out) {
      out[0] = (uint8_t)(0xF0 | (cp >> 18));
      out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
      out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
      out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    return 4;
  }
  /* Invalid code point -> replacement character U+FFFD */
  if (out) {
    out[0] = 0xEF;
    out[1] = 0xBF;
    out[2] = 0xBD;
  }
  return 3;
}

static inline int lighter_decode_utf8_single(const uint8_t* src, size_t size, size_t* r, uint32_t* cp) {
  if (*r >= size)
    return -1;
  uint8_t c = src[(*r)++];
  if (c <= 0x7F) {
    *cp = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0 && *r < size) {
    *cp = ((uint32_t)(c & 0x1F) << 6) | (src[(*r)++] & 0x3F);
    return 2;
  } else if ((c & 0xF0) == 0xE0 && *r + 1 < size) {
    *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(src[(*r)++] & 0x3F) << 6) | (src[(*r)++] & 0x3F);
    return 3;
  } else if ((c & 0xF8) == 0xF0 && *r + 2 < size) {
    *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(src[(*r)++] & 0x3F) << 12) | ((uint32_t)(src[(*r)++] & 0x3F) << 6) | (src[(*r)++] & 0x3F);
    return 4;
  }
  return -1;
}

static inline void lighter_encode_utf16_single(uint32_t cp, LighterEncoding enc, uint8_t* dst, size_t* w) {
  if (cp > 0x10FFFF)
    cp = 0xFFFD;
  if (cp <= 0xFFFF) {
    uint16_t u = (uint16_t)cp;
    if (enc == LIGHTER_ENC_UTF16LE) {
      dst[(*w)++] = u & 0xFF;
      dst[(*w)++] = (u >> 8) & 0xFF;
    } else {
      dst[(*w)++] = (u >> 8) & 0xFF;
      dst[(*w)++] = u & 0xFF;
    }
  } else {
    cp -= 0x10000;
    uint16_t hi = (uint16_t)(0xD800 + (cp >> 10));
    uint16_t lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
    if (enc == LIGHTER_ENC_UTF16LE) {
      dst[(*w)++] = hi & 0xFF;
      dst[(*w)++] = (hi >> 8) & 0xFF;
      dst[(*w)++] = lo & 0xFF;
      dst[(*w)++] = (lo >> 8) & 0xFF;
    } else {
      dst[(*w)++] = (hi >> 8) & 0xFF;
      dst[(*w)++] = hi & 0xFF;
      dst[(*w)++] = (lo >> 8) & 0xFF;
      dst[(*w)++] = lo & 0xFF;
    }
  }
}

/**
 * Forward transcoding: any to UTF8.
 * If data is already UTF8 with BOM, it skips the BOM.
 */
static inline int lighter_transcode_to_utf8(const uint8_t* src, size_t size, uint8_t* dst, LighterEncoding enc, size_t* out_size) {
  if (enc == LIGHTER_ENC_UTF8) {
    if (src != dst)
      memmove(dst, src, size);
    *out_size = size;
    return 0;
  }
  if (enc == LIGHTER_ENC_UTF8_BOM) {
    memmove(dst, src + 3, size - 3);
    *out_size = size - 3;
    return 0;
  }

  size_t w = 0;
  size_t r;
  if (enc == LIGHTER_ENC_UTF16LE || enc == LIGHTER_ENC_UTF16BE) {
    r = 2;  // Skip BOM
    while (r + 1 < size) {
      uint32_t u = (enc == LIGHTER_ENC_UTF16LE) ? (src[r] | (uint32_t)src[r + 1] << 8) : ((uint32_t)src[r] << 8 | src[r + 1]);
      r += 2;
      if (u >= 0xD800 && u <= 0xDBFF && r + 1 < size) {
        uint32_t low = (enc == LIGHTER_ENC_UTF16LE) ? (src[r] | (uint32_t)src[r + 1] << 8) : ((uint32_t)src[r] << 8 | src[r + 1]);
        if (low >= 0xDC00 && low <= 0xDFFF) {
          u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
          r += 2;
        }
      }
      w += lighter_encode_utf8_single(u, dst + w);
    }
  } else if (enc == LIGHTER_ENC_UTF32LE || enc == LIGHTER_ENC_UTF32BE) {
    r = 4;  // Skip BOM
    while (r + 3 < size) {
      uint32_t u = (enc == LIGHTER_ENC_UTF32LE) ? (src[r] | (uint32_t)src[r + 1] << 8 | (uint32_t)src[r + 2] << 16 | (uint32_t)src[r + 3] << 24)
                                                : ((uint32_t)src[r] << 24 | (uint32_t)src[r + 1] << 16 | (uint32_t)src[r + 2] << 8 | src[r + 3]);
      r += 4;
      w += lighter_encode_utf8_single(u, dst + w);
    }
  }
  *out_size = w;
  return 0;
}

/**
 * Reverse transcoding: UTF8 to original encoding (currently only UTF16 supported for fallback).
 */
static inline int lighter_transcode_from_utf8(const uint8_t* src, size_t src_size, LighterEncoding target_enc, uint8_t* dst, size_t* out_size) {
  if (target_enc == LIGHTER_ENC_UTF8 || target_enc == LIGHTER_ENC_UTF8_BOM) {
    size_t bom_off = (target_enc == LIGHTER_ENC_UTF8_BOM) ? 3 : 0;
    if (bom_off) {
      dst[0] = 0xEF;
      dst[1] = 0xBB;
      dst[2] = 0xBF;
    }
    memcpy(dst + bom_off, src, src_size);
    *out_size = src_size + bom_off;
    return 0;
  }

  size_t r = 0;
  size_t w = 0;
  /* Add BOM */
  if (target_enc == LIGHTER_ENC_UTF16LE) {
    dst[w++] = 0xFF;
    dst[w++] = 0xFE;
  } else if (target_enc == LIGHTER_ENC_UTF16BE) {
    dst[w++] = 0xFE;
    dst[w++] = 0xFF;
  } else if (target_enc == LIGHTER_ENC_UTF32LE) {
    dst[w++] = 0xFF;
    dst[w++] = 0xFE;
    dst[w++] = 0x00;
    dst[w++] = 0x00;
  } else if (target_enc == LIGHTER_ENC_UTF32BE) {
    dst[w++] = 0x00;
    dst[w++] = 0x00;
    dst[w++] = 0xFE;
    dst[w++] = 0xFF;
  }

  while (r < src_size) {
    uint32_t cp;
    if (lighter_decode_utf8_single(src, src_size, &r, &cp) < 0)
      break;
    if (target_enc == LIGHTER_ENC_UTF16LE || target_enc == LIGHTER_ENC_UTF16BE) {
      lighter_encode_utf16_single(cp, target_enc, dst, &w);
    } else if (target_enc == LIGHTER_ENC_UTF32LE || target_enc == LIGHTER_ENC_UTF32BE) {
      if (target_enc == LIGHTER_ENC_UTF32LE) {
        dst[w++] = cp & 0xFF;
        dst[w++] = (cp >> 8) & 0xFF;
        dst[w++] = (cp >> 16) & 0xFF;
        dst[w++] = (cp >> 24) & 0xFF;
      } else {
        dst[w++] = (cp >> 24) & 0xFF;
        dst[w++] = (cp >> 16) & 0xFF;
        dst[w++] = (cp >> 8) & 0xFF;
        dst[w++] = cp & 0xFF;
      }
    }
  }
  *out_size = w;
  return 0;
}

#endif /* LIGHTER_TRANSCODE_H */
