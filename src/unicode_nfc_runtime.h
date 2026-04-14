/**
 * @file      unicode_nfc_runtime.h
 * @brief     Unicode NFC runtime: load binary, quick check, normalize.
 * @details   For lighter_string.h. UAX #15.
 */
#ifndef UNICODE_NFC_RUNTIME_H
#define UNICODE_NFC_RUNTIME_H
#include <stdlib.h>
#include <string.h>

#include "lighter_cpu.h"
#include "unicode_nfc_shared.h"

/* UTF-8 boundaries and masks (RFC 3629). */
#define UTF8_ASCII_MAX 0x80u
#define UTF8_LEAD_2 0xC0u
#define UTF8_LEAD_3 0xE0u
#define UTF8_LEAD_4 0xF0u
#define UTF8_CONT 0x80u
#define UTF8_CONT_MASK 0x3Fu
#define UTF8_PAYLOAD_2 0x1Fu /* 5 bits for 2-byte lead */
#define UTF8_PAYLOAD_3 0x0Fu /* 4 bits for 3-byte lead */
#define UTF8_PAYLOAD_4 0x07u /* 3 bits for 4-byte lead */
#define UTF8_REPLACEMENT 0xFFFDu
#define UTF8_2BYTE_MAX 0x800u
#define UTF8_3BYTE_MAX 0x10000u
/* Quick-check fast path: bytes < this imply NFC-safe range (U+0000–U+02FF). */
#define NFC_QC_FAST_PATH_BYTE 0xCCu
/* RLE (must match gen_unicode_tables): escape byte; min run length for encoded run. */
#define NFC_RLE_ESCAPE 0xFFu
#define NFC_RLE_MIN_RUN 3
/* CCC value used to block reordering across starters in composition. */
#define NFC_CCC_BLOCKED 255
/* Binary format: version byte; decomp_data length is 24-bit on disk. */
#define NFC_BINARY_VERSION 2
#define NFC_DECOMP_IDX_BITS 24
#define NFC_DECOMP_IDX_MAX (1u << NFC_DECOMP_IDX_BITS)
/* Binary loader sanity limits (reject corrupt or future-format data). */
#define NFC_BIN_MAX_S2_BLOCKS 512u
#define NFC_BIN_MAX_POOL_SIZE (1024u * 1024u)
#define NFC_BIN_MAX_DECOMP_LEN (4u * 1024u * 1024u)
#define NFC_BIN_MAX_COMP_SIZE 500000u
#define NFC_BIN_MAX_U8 256u
#define NFC_STAGE1_MAX_CHUNK_BITS 8

/* ── UTF-8 codec ───────────────────────────────────────────────────── */

static inline int nfc_utf8_decode(const uint8_t* p, const uint8_t* end, uint32_t* cp) {
  if (p >= end) {
    return 0;
  }
  uint8_t b = p[0];
  if (b < UTF8_ASCII_MAX) {
    *cp = b;
    return 1;
  }
  if (b < UTF8_LEAD_2) {
    *cp = UTF8_REPLACEMENT;
    return 1;
  }
  if (b < UTF8_LEAD_3) {
    if (p + 2 > end) {
      *cp = UTF8_REPLACEMENT;
      return 1;
    }
    *cp = ((uint32_t)(b & UTF8_PAYLOAD_2) << 6) | (p[1] & UTF8_CONT_MASK);
    return 2;
  }
  if (b < UTF8_LEAD_4) {
    if (p + 3 > end) {
      *cp = UTF8_REPLACEMENT;
      return 1;
    }
    *cp = ((uint32_t)(b & UTF8_PAYLOAD_3) << 12) | ((uint32_t)(p[1] & UTF8_CONT_MASK) << 6) | (p[2] & UTF8_CONT_MASK);
    return 3;
  }
  if (p + 4 > end) {
    *cp = UTF8_REPLACEMENT;
    return 1;
  }
  *cp = ((uint32_t)(b & UTF8_PAYLOAD_4) << 18) | ((uint32_t)(p[1] & UTF8_CONT_MASK) << 12) | ((uint32_t)(p[2] & UTF8_CONT_MASK) << 6) | (p[3] & UTF8_CONT_MASK);
  return 4;
}

static inline int nfc_utf8_encode(uint8_t* p, uint32_t cp) {
  if (cp < UTF8_ASCII_MAX) {
    p[0] = (uint8_t)cp;
    return 1;
  }
  if (cp < UTF8_2BYTE_MAX) {
    p[0] = UTF8_LEAD_2 | (uint8_t)(cp >> 6);
    p[1] = UTF8_CONT | (uint8_t)(cp & UTF8_CONT_MASK);
    return 2;
  }
  if (cp < UTF8_3BYTE_MAX) {
    p[0] = UTF8_LEAD_3 | (uint8_t)(cp >> 12);
    p[1] = UTF8_CONT | (uint8_t)((cp >> 6) & UTF8_CONT_MASK);
    p[2] = UTF8_CONT | (uint8_t)(cp & UTF8_CONT_MASK);
    return 3;
  }
  p[0] = UTF8_LEAD_4 | (uint8_t)(cp >> 18);
  p[1] = UTF8_CONT | (uint8_t)((cp >> 12) & UTF8_CONT_MASK);
  p[2] = UTF8_CONT | (uint8_t)((cp >> 6) & UTF8_CONT_MASK);
  p[3] = UTF8_CONT | (uint8_t)(cp & UTF8_CONT_MASK);
  return 4;
}

/* ── Table lookups ─────────────────────────────────────────────────── */

static inline uint8_t nfc_get_ccc(const NfcData* d, uint32_t cp) {
  if (cp >= NFC_MAX_CP) {
    return 0;
  }
  if (d->ccc_dense) {
    return d->ccc_dense[cp];
  }
  uint32_t block_idx = cp >> NFC_BLOCK_SHIFT;
  uint8_t chunk_idx = d->stage1_top[block_idx >> NFC_STAGE1_CHUNK_BITS];
  uint8_t pair_id = d->stage1_chunks[((size_t)chunk_idx << NFC_STAGE1_CHUNK_BITS) + (block_idx & (NFC_STAGE1_CHUNK_ENTRIES - 1))];
  uint8_t blk = d->pair_map_ccc[pair_id];
  uint32_t off = d->stage2_ccc_off[blk] + (cp & NFC_BLOCK_MASK);
  return (d->stage2_ccc_chk[off] == blk) ? d->stage2_ccc_val[off] : 0;
}

static inline uint8_t nfc_get_qc(const NfcData* d, uint32_t cp) {
  if (cp >= NFC_MAX_CP) {
    return NFC_QC_YES;
  }
  if (d->qc_dense) {
    return d->qc_dense[cp];
  }
  uint32_t block_idx = cp >> NFC_BLOCK_SHIFT;
  uint8_t chunk_idx = d->stage1_top[block_idx >> NFC_STAGE1_CHUNK_BITS];
  uint8_t pair_id = d->stage1_chunks[((size_t)chunk_idx << NFC_STAGE1_CHUNK_BITS) + (block_idx & (NFC_STAGE1_CHUNK_ENTRIES - 1))];
  uint8_t blk = d->pair_map_qc[pair_id];
  uint32_t off = d->stage2_qc_off[blk] + (cp & NFC_BLOCK_MASK);
  return (d->stage2_qc_chk[off] == blk) ? d->stage2_qc_val[off] : NFC_QC_YES;
}

static inline int nfc_build_decomp_idx(NfcData* d) {
  if (d->decomp_idx) {
    return 1;
  }
  d->decomp_idx = (uint32_t*)calloc(NFC_MAX_CP, sizeof(uint32_t));
  if (!d->decomp_idx) {
    return 0;
  }
  for (size_t i = 0; i < d->decomp_sparse_count; ++i) {
    uint32_t cp = nfc_decomp_sparse_cp(d, i);
    uint32_t idx = nfc_decomp_sparse_idx(d, i);
    d->decomp_idx[cp] = idx;
  }
  free((void*)d->decomp_sparse);
  d->decomp_sparse = NULL;
  d->decomp_sparse_count = 0;
  return 1;
}

/* Canonical decomposition of one code point. Returns length (0 = none). */
static inline int nfc_decompose_one(const NfcData* d, uint32_t cp, uint32_t* out) {
  /* Hangul syllable decomposition */
  if (cp >= HANGUL_SBASE && cp < HANGUL_SBASE + HANGUL_SCOUNT) {
    uint32_t si = cp - HANGUL_SBASE;
    uint32_t ti = si % HANGUL_TCOUNT;
    if (ti) {
      out[0] = HANGUL_SBASE + si - ti; /* LV part */
      out[1] = HANGUL_TBASE + ti;      /* T part */
    } else {
      out[0] = HANGUL_LBASE + si / HANGUL_NCOUNT;
      out[1] = HANGUL_VBASE + (si % HANGUL_NCOUNT) / HANGUL_TCOUNT;
    }
    return 2;
  }
  if (cp >= NFC_MAX_CP) {
    return 0;
  }

  uint32_t idx = 0;
  if (d->decomp_idx) {
    idx = d->decomp_idx[cp];
  } else if (d->decomp_sparse && d->decomp_sparse_count > 0) {
    size_t l = 0;
    size_t h = d->decomp_sparse_count;
    while (l < h) {
      size_t m = l + (h - l) / 2;
      uint32_t mcp = nfc_decomp_sparse_cp(d, m);
      if (mcp < cp) {
        l = m + 1;
      } else if (mcp > cp) {
        h = m;
      } else {
        idx = nfc_decomp_sparse_idx(d, m);
        break;
      }
    }
  }

  if (!idx) {
    return 0;
  }
  uint32_t len = d->decomp_data[idx];
  for (uint32_t i = 0; i < len; ++i) {
    out[i] = d->decomp_data[idx + 1 + i];
  }
  return (int)len;
}

/* ── Composition Logic ─────────────────────────────────────────────── */

static uint32_t nfc_compose_pair(const NfcData* d, uint32_t starter, uint32_t combining) {
  /* Hangul L+V or LV+T? */
  if (starter >= HANGUL_LBASE && starter < HANGUL_LBASE + HANGUL_LCOUNT) {
    /* L+V -> LV */
    uint32_t l_idx = starter - HANGUL_LBASE;
    uint32_t v_idx = combining - HANGUL_VBASE;
    if (v_idx < HANGUL_VCOUNT) {
      return HANGUL_SBASE + (l_idx * HANGUL_VCOUNT + v_idx) * HANGUL_TCOUNT;
    }
  }
  if (starter >= HANGUL_SBASE && starter < HANGUL_SBASE + HANGUL_SCOUNT) {
    /* LV+T -> LVT */
    uint32_t s_idx = starter - HANGUL_SBASE;
    if ((s_idx % HANGUL_TCOUNT) == 0) { /* Is LV syllable? */
      uint32_t t_idx = combining - HANGUL_TBASE;
      if (t_idx > 0 && t_idx < HANGUL_TCOUNT) {
        return starter + t_idx;
      }
    }
  }

  /* Hash lookup (binary loader builds this) */
  if (d->comp_hash && d->comp_hash_cap) {
    size_t cap = d->comp_hash_cap;
    size_t i = ((size_t)starter * 31u + (size_t)combining) % cap;
    for (;;) {
      const NfcCompEntry* e = &d->comp_hash[i];
      if (e->composed == 0 && e->starter == 0 && e->combining == 0) {
        return 0;
      }
      if (e->starter == starter && e->combining == combining) {
        return e->composed;
      }
      i = (i + 1u) % cap;
    }
  }

  /* Fallback: binary search */
  if (!d->comp_table || d->comp_size == 0) {
    return 0;
  }
  int l = 0, r = (int)d->comp_size - 1;
  while (l <= r) {
    int m = l + (r - l) / 2;
    const NfcCompEntry* e = &d->comp_table[m];
    if (e->starter == starter) {
      if (e->combining == combining) {
        return e->composed;
      }
      if (e->combining < combining) {
        l = m + 1;
      } else {
        r = m - 1;
      }
    } else if (e->starter < starter) {
      l = m + 1;
    } else {
      r = m - 1;
    }
  }
  return 0;
}

/* ── Recursive full canonical decomposition ────────────────────────── */

static inline int nfc_full_decompose(const NfcData* d, uint32_t cp, uint32_t* buf, int max) {
  uint32_t parts[NFC_DECOMP_MAX];
  int n = nfc_decompose_one(d, cp, parts);
  if (!n) {
    if (max < 1) {
      return 0;
    }
    buf[0] = cp;
    return 1;
  }
  int total = 0;
  for (int i = 0; i < n && total < max; ++i) {
    total += nfc_full_decompose(d, parts[i], buf + total, max - total);
  }
  return total;
}

static int nfc_full_decompose_append(const NfcData* d, uint32_t cp, uint32_t** buf, size_t* len, size_t* cap, uint32_t* stack_buf, size_t stack_cap) {
  uint32_t parts[NFC_DECOMP_MAX];
  int n = nfc_decompose_one(d, cp, parts);
  if (!n) {
    if (*len == *cap) {
      size_t new_cap = (*cap == 0) ? stack_cap : (*cap * 2u);
      if (new_cap < stack_cap) {
        new_cap = stack_cap;
      }
      uint32_t* new_buf = NULL;
      if (*buf == stack_buf) {
        new_buf = (uint32_t*)malloc(new_cap * sizeof(uint32_t));
        if (new_buf && *len) {
          memcpy(new_buf, stack_buf, (*len) * sizeof(uint32_t));
        }
      } else {
        new_buf = (uint32_t*)realloc(*buf, new_cap * sizeof(uint32_t));
      }
      if (!new_buf) {
        return 0;
      }
      *buf = new_buf;
      *cap = new_cap;
    }
    (*buf)[(*len)++] = cp;
    return 1;
  }
  for (int i = 0; i < n; ++i) {
    if (!nfc_full_decompose_append(d, parts[i], buf, len, cap, stack_buf, stack_cap)) {
      return 0;
    }
  }
  return 1;
}

/* ── Canonical ordering (insertion sort by CCC, stable) ────────────── */

static void nfc_canonical_order(const NfcData* d, uint32_t* buf, int len) {
  for (int i = 1; i < len; ++i) {
    uint32_t cp = buf[i];
    uint8_t cc = nfc_get_ccc(d, cp);
    if (!cc) {
      continue; /* starters stay */
    }
    int j = i - 1;
    while (j >= 0 && nfc_get_ccc(d, buf[j]) > cc) {
      buf[j + 1] = buf[j];
      --j;
    }
    buf[j + 1] = cp;
  }
}

/* ── Canonical composition ─────────────────────────────────────────── */

static int nfc_compose_buf(const NfcData* d, uint32_t* buf, int len) {
  if (len < 2) {
    return len;
  }
  int starter = -1;
  uint8_t last_cc = NFC_CCC_BLOCKED;

  /* Find the first starter */
  if (nfc_get_ccc(d, buf[0]) == 0) {
    starter = 0;
    last_cc = 0;
  }

  int out = 1;
  for (int i = 1; i < len; ++i) {
    uint32_t cp = buf[i];
    uint8_t cc = nfc_get_ccc(d, cp);
    int composed = 0;

    if (starter >= 0) {
      /* Not blocked: either cp is a starter (cc == 0) adjacent to last starter
         (last_cc == 0), or cp is a combining mark with cc > last_cc */
      if ((last_cc == 0 && cc == 0) || (last_cc < cc)) {
        uint32_t c = nfc_compose_pair(d, buf[starter], cp);
        if (c) {
          buf[starter] = c;
          composed = 1;
          /* Don't update last_cc: composed char replaces starter */
        }
      }
    }

    if (!composed) {
      if (cc == 0) {
        starter = out;
        last_cc = 0;
      } else {
        last_cc = cc;
      }
      buf[out++] = cp;
    }
  }
  return out;
}

/* ── Quick Check: does this UTF-8 segment need NFC normalization? ── */

/* Lazy loader (defined at end of file); loads only when slow path is needed. */
static inline NfcData* nfc_get_or_load(const char* path);

/* Returns NFC_QC_YES if the byte range is definitely in NFC,
   NFC_QC_NO or NFC_QC_MAYBE otherwise.
   Fast path: if every byte < NFC_QC_FAST_PATH_BYTE, segment is NFC-safe (no load).
   Loads NFC data from path only when the slow path is needed. */
static inline int nfc_quick_check(const char* path, const uint8_t* start, const uint8_t* end, int has_avx512, int has_avx2, int has_neon, int has_rvv) {
  const uint8_t* p = start;
  int found_high = 0;

#if LIGHTER_PLATFORM_X86
  if (has_avx512) {
    while (p + 64 <= end) {
      __m512i chunk = _mm512_loadu_si512((const void*)p);
      if (_mm512_test_epi8_mask(chunk, _mm512_set1_epi8(0x80)) != 0) {
        found_high = 1;
        break;
      }
      p += 64;
    }
  } else if (has_avx2) {
    while (p + 32 <= end) {
      __m256i chunk = _mm256_loadu_si256((const __m256i*)p);
      if (_mm256_movemask_epi8(chunk) != 0) {
        found_high = 1;
        break;
      }
      p += 32;
    }
  } else
#endif
#if LIGHTER_PLATFORM_ARM64
      if (has_neon) {
    while (p + 16 <= end) {
      uint8x16_t chunk = vld1q_u8((const uint8_t*)p);
      /* Any byte >= 0x80 means non-ASCII; vmaxvq_u8 gives the max byte in one instruction. */
      if (vmaxvq_u8(chunk) >= 0x80) {
        found_high = 1;
        break;
      }
      p += 16;
    }
  } else
#endif
#if LIGHTER_PLATFORM_RISCV
      if (has_rvv) {
    while (p < end) {
      size_t n = end - p;
      size_t vl = __riscv_vsetvli(n, __RISCV_E8, __RISCV_M1, __RISCV_TA, __RISCV_MA);
      vuint8m1_t chunk = __riscv_vle8_v_u8m1(p, vl);
      vbool8_t mask = __riscv_vmsgtu_vx_u8m1_b8(chunk, 127, vl);
      if (__riscv_vfirst_m_b8(mask, vl) >= 0) {
        found_high = 1;
        break;
      }
      p += vl;
    }
  } else
#endif
  {
    /* Fast-path: strictly 7-bit ASCII */
    while (p + 8 <= end) {
      uint64_t v;
      memcpy(&v, p, 8);
      if (v & 0x8080808080808080ULL) {
        break;
      }
      p += 8;
    }
  }
  if (!found_high) {
    for (const uint8_t* check = p; check < end; ++check) {
      if (*check >= 0x80) {
        found_high = 1;
        break;
      }
    }
  }
  if (!found_high) {
    return NFC_QC_YES;
  }

  {
    const NfcData* d = nfc_get_or_load(path);
    if (!d) {
      return NFC_QC_YES;
    }
    uint8_t last_ccc = 0;
    int result = NFC_QC_YES;
    p = start;
    while (p < end) {
      uint32_t cp;
      int n = nfc_utf8_decode(p, end, &cp);
      if (!n) {
        break;
      }
      p += n;
      uint8_t ccc = nfc_get_ccc(d, cp);
      if (last_ccc > ccc && ccc != 0) {
        return NFC_QC_NO;
      }
      uint8_t qc = nfc_get_qc(d, cp);
      if (qc == NFC_QC_NO) {
        return NFC_QC_NO;
      }
      if (qc == NFC_QC_MAYBE) {
        result = NFC_QC_MAYBE;
      }
      last_ccc = ccc;
    }
    return result;
  }
}

/* ── Incremental NFC: normalize between starters ──────────────────── */

/* Smarter in-place normalization that processes only segments that need it.
   A "segment" is a starter followed by all subsequent non-starters up to
   (but not including) the next starter.
   Returns new end pointer. */
static uint8_t* nfc_normalize_utf8_incremental(const NfcData* d, uint8_t* start, uint8_t* end) {
  if (!d) {
    return end;
  }
  (void)nfc_build_decomp_idx((NfcData*)d);
  /* Byte scan fast path */
  {
    const uint8_t* p = start;
    int needs_check = 0;
    for (; p < end; ++p) {
      if (*p >= NFC_QC_FAST_PATH_BYTE) {
        needs_check = 1;
        break;
      }
    }
    if (!needs_check) {
      return end;
    }
  }

  uint8_t* rp = start;        /* read pointer */
  uint8_t* wp = start;        /* write pointer */
  uint8_t* seg_begin = start; /* start of current segment for normalization */

  /* We process the string in segments bounded by NFC-stable points.
     A stable point is a starter (CCC=0) with NFC_QC=YES where the
     previous character also had CCC=0. */

  uint32_t seg_stack[NFC_SEG_STACK_MAX];
  uint32_t* seg_buf = seg_stack;
  size_t seg_cap = NFC_SEG_STACK_MAX;
  uint32_t decomp_stack[NFC_DECOMP_STACK_MAX];
  uint32_t* decomp = decomp_stack;
  size_t decomp_cap = NFC_DECOMP_STACK_MAX;
  int seg_len = 0;
  int needs_norm = 0;
  uint8_t last_ccc = 0;
  int raw_fallback = 0;

  while (rp < end) {
    /* Scan one code point */
    uint32_t cp;
    int n = nfc_utf8_decode(rp, end, &cp);
    if (!n) {
      break;
    }

    uint8_t ccc = nfc_get_ccc(d, cp);
    uint8_t qc = nfc_get_qc(d, cp);

    /* Detect if this is a new stable boundary */
    int is_stable = (ccc == 0 && qc == NFC_QC_YES && last_ccc == 0 && seg_len > 0);

    if (is_stable) {
      /* Flush previous segment */
      if (needs_norm) {
        size_t dlen = 0;
        decomp = decomp_stack;
        decomp_cap = NFC_DECOMP_STACK_MAX;
        for (int i = 0; i < seg_len; ++i) {
          if (!nfc_full_decompose_append(d, seg_buf[i], &decomp, &dlen, &decomp_cap, decomp_stack, NFC_DECOMP_STACK_MAX)) {
            raw_fallback = 1;
            goto finish;
          }
        }
        nfc_canonical_order(d, decomp, (int)dlen);
        dlen = (size_t)nfc_compose_buf(d, decomp, (int)dlen);
        for (size_t i = 0; i < dlen; ++i) {
          wp += nfc_utf8_encode(wp, decomp[i]);
        }
        if (decomp != decomp_stack) {
          free(decomp);
          decomp = decomp_stack;
        }
      } else {
        /* Copy segment bytes as-is */
        size_t bytes = (size_t)(rp - seg_begin);
        if (wp != seg_begin) {
          memmove(wp, seg_begin, bytes);
        }
        wp += bytes;
      }
      /* Start new segment at current code point */
      seg_begin = rp;
      seg_len = 0;
      needs_norm = 0;
    }

    /* Accumulate into current segment */
    if ((size_t)seg_len == seg_cap) {
      size_t new_cap = seg_cap * 2u;
      uint32_t* new_buf = NULL;
      if (seg_buf == seg_stack) {
        new_buf = (uint32_t*)malloc(new_cap * sizeof(uint32_t));
        if (new_buf && seg_len) {
          memcpy(new_buf, seg_stack, (size_t)seg_len * sizeof(uint32_t));
        }
      } else {
        new_buf = (uint32_t*)realloc(seg_buf, new_cap * sizeof(uint32_t));
      }
      if (!new_buf) {
        raw_fallback = 1;
        goto finish;
      }
      seg_buf = new_buf;
      seg_cap = new_cap;
    }
    seg_buf[seg_len++] = cp;
    if (qc != NFC_QC_YES || (last_ccc > ccc && ccc != 0)) {
      needs_norm = 1;
    }
    last_ccc = ccc;
    rp += n;
  }

  /* Flush final segment */
  if (seg_len > 0) {
    if (needs_norm) {
      size_t dlen = 0;
      decomp = decomp_stack;
      decomp_cap = NFC_DECOMP_STACK_MAX;
      for (int i = 0; i < seg_len; ++i) {
        if (!nfc_full_decompose_append(d, seg_buf[i], &decomp, &dlen, &decomp_cap, decomp_stack, NFC_DECOMP_STACK_MAX)) {
          raw_fallback = 1;
          goto finish;
        }
      }
      nfc_canonical_order(d, decomp, (int)dlen);
      dlen = (size_t)nfc_compose_buf(d, decomp, (int)dlen);
      for (size_t i = 0; i < dlen; ++i) {
        wp += nfc_utf8_encode(wp, decomp[i]);
      }
      if (decomp != decomp_stack) {
        free(decomp);
        decomp = decomp_stack;
      }
    } else {
      size_t bytes = (size_t)(rp - seg_begin);
      if (wp != seg_begin) {
        memmove(wp, seg_begin, bytes);
      }
      wp += bytes;
    }
  }

finish:
  if (raw_fallback) {
    size_t bytes = (size_t)(end - seg_begin);
    if (wp != seg_begin) {
      memmove(wp, seg_begin, bytes);
    }
    wp += bytes;
  }
  if (seg_buf != seg_stack) {
    free(seg_buf);
  }
  if (decomp != decomp_stack) {
    free(decomp);
  }
  return wp;
}
/* RLE decode: escape 0x00 = literal escape; escape n (3<=n<=255) + byte = run. */
static int nfc_rle_decode(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len) {
  size_t si = 0, di = 0;
  while (si < src_len && di < dst_len) {
    uint8_t b = src[si++];
    if (b == NFC_RLE_ESCAPE) {
      if (si >= src_len) {
        return -1;
      }
      uint8_t n = src[si++];
      if (n == 0) {
        dst[di++] = (uint8_t)NFC_RLE_ESCAPE;
      } else if (n >= NFC_RLE_MIN_RUN) {
        if (si >= src_len || di + n > dst_len) {
          return -1;
        }
        b = src[si++];
        for (uint8_t i = 0; i < n; i++) {
          dst[di++] = b;
        }
      } else {
        return -1;
      }
    } else {
      dst[di++] = b;
    }
  }
  return (di == dst_len && si == src_len) ? 0 : -1;
}

/* ── Binary Loader ───────────────────────────────────────────────────
 * V2 format (96-byte header): s2 pools RLE-compressed; 4× packed sizes at 88.
 * Sparse decomp indices 24-bit on disk. Load builds dense CCC/QC, decomp_idx, comp hash.
 */

static NFC_UNUSED NfcData* nfc_load_binary(const char* path) {
  NfcData* d = (NfcData*)calloc(1, sizeof(NfcData));
  if (!d) {
    return NULL;
  }
  if (lighter_map_open(&d->map, path, 1) != 0) {
    free(d);
    return NULL;
  }
  char* data = (char*)d->map.data;
  size_t size = d->map.size;
  const uint8_t *decomp_data_src = NULL, *comp_src = NULL;
  size_t out_idx = 0, decomp_remaining = 0, hash_cap = 0;
  uint8_t* decomp_buf = NULL;
  size_t off = 0, len = 0, sz = size, top_size = 0;
  unsigned chunk_bits = 0;
  size_t stage1_chunks_byte_size = 0;

  if (size < NFC_HEADER_SIZE) {
    lighter_map_close(&d->map);
    free(d);
    return NULL;
  }
  const uint8_t* h = (const uint8_t*)data;
  if (h[0] != 0x43 || h[1] != 0x46 || h[2] != 0x4E || h[3] != 0x4C || h[4] != NFC_BINARY_VERSION) { /* LNFC */
    lighter_map_close(&d->map);
    free(d);
    return NULL;
  }

  uint32_t off_s1_top, off_s1_chunks, off_pair_ccc, off_pair_qc;
  uint32_t off_s2_ccc_off, off_s2_ccc_val, off_s2_ccc_chk;
  uint32_t off_s2_qc_off, off_s2_qc_val, off_s2_qc_chk;
  uint32_t off_decomp_sparse, off_decomp_data, off_comp;
  uint32_t s2_blocks_ccc, s2_blocks_qc, s2_ccc_size, s2_qc_size;
  uint32_t decomp_sparse_count, decomp_data_len, decomp_data_packed_size, comp_size;
  uint32_t s1_num_chunks, s1_num_pairs;
  uint32_t s2_ccc_val_packed, s2_ccc_chk_packed, s2_qc_val_packed, s2_qc_chk_packed;

  memcpy(&off_s1_top, h + 8, 4);
  memcpy(&off_s1_chunks, h + 12, 4);
  memcpy(&off_pair_ccc, h + 16, 4);
  memcpy(&off_pair_qc, h + 20, 4);
  memcpy(&off_s2_ccc_off, h + 24, 4);
  memcpy(&off_s2_ccc_val, h + 28, 4);
  memcpy(&off_s2_ccc_chk, h + 32, 4);
  memcpy(&off_s2_qc_off, h + 36, 4);
  memcpy(&off_s2_qc_val, h + 40, 4);
  memcpy(&off_s2_qc_chk, h + 44, 4);
  memcpy(&off_decomp_sparse, h + 48, 4);
  memcpy(&off_decomp_data, h + 52, 4);
  memcpy(&off_comp, h + 56, 4);
  s2_blocks_ccc = (uint32_t)(h[60] | (h[61] << 8));
  s2_blocks_qc = (uint32_t)(h[62] | (h[63] << 8));
  memcpy(&s2_ccc_size, h + 64, 4);
  memcpy(&s2_qc_size, h + 68, 4);
  decomp_sparse_count = (uint32_t)(h[72] | (h[73] << 8));
  comp_size = (uint32_t)(h[74] | (h[75] << 8));
  s1_num_chunks = (uint32_t)(h[76] | (h[77] << 8));
  s1_num_pairs = (uint32_t)(h[78] | (h[79] << 8));
  memcpy(&decomp_data_len, h + 80, 4);
  memcpy(&decomp_data_packed_size, h + 84, 4);
  s2_ccc_val_packed = (uint32_t)(h[88] | (h[89] << 8));
  s2_ccc_chk_packed = (uint32_t)(h[90] | (h[91] << 8));
  s2_qc_val_packed = (uint32_t)(h[92] | (h[93] << 8));
  s2_qc_chk_packed = (uint32_t)(h[94] | (h[95] << 8));

  /* Sanity limits: reject corrupt or future-format data, allow UCD growth */
  if (s2_blocks_ccc > 512u || s2_blocks_qc > 512u || s2_ccc_size > 1024u * 1024u || s2_qc_size > 1024u * 1024u || decomp_sparse_count > NFC_MAX_CP ||
      decomp_data_len > 4u * 1024u * 1024u || decomp_data_len < 1u || comp_size > 500000u || s1_num_chunks > 256u || s1_num_pairs > 256u) {
    goto bin_fail;
  }
  if (decomp_data_len >= NFC_DECOMP_IDX_MAX) {
    goto bin_fail;
  }
  /* RLE packed sizes: must be plausible (RLE can expand but not arbitrarily) */
  if (s2_ccc_val_packed > 2u * s2_ccc_size || s2_ccc_chk_packed > 2u * s2_ccc_size || s2_qc_val_packed > 2u * s2_qc_size ||
      s2_qc_chk_packed > 2u * s2_qc_size) {
    goto bin_fail;
  }

  d->stage2_blocks_ccc = (uint16_t)s2_blocks_ccc;
  d->stage2_blocks_qc = (uint16_t)s2_blocks_qc;
  d->decomp_sparse_count = decomp_sparse_count;
  d->decomp_data_len = decomp_data_len;
  d->comp_size = comp_size;
  d->stage1_num_chunks = (uint16_t)s1_num_chunks;
  d->stage1_num_pairs = (uint8_t)s1_num_pairs;

  top_size = (NFC_BLOCK_COUNT + NFC_STAGE1_CHUNK_ENTRIES - 1u) / NFC_STAGE1_CHUNK_ENTRIES;

  off = off_s1_top;
  len = top_size;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }

  chunk_bits = 1;
  if (s1_num_pairs > 1) {
    unsigned n = (unsigned)s1_num_pairs;
    while ((1u << chunk_bits) < n && chunk_bits < NFC_STAGE1_MAX_CHUNK_BITS) {
      chunk_bits++;
    }
  }
  stage1_chunks_byte_size = (size_t)s1_num_chunks * ((NFC_STAGE1_CHUNK_ENTRIES * chunk_bits + 7u) / 8u);
  off = off_s1_chunks;
  len = stage1_chunks_byte_size;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_pair_ccc;
  len = d->stage1_num_pairs;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_pair_qc;
  len = d->stage1_num_pairs;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_s2_ccc_off;
  len = (size_t)d->stage2_blocks_ccc * sizeof(uint16_t);
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_s2_ccc_val;
  len = s2_ccc_val_packed;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_s2_ccc_chk;
  len = s2_ccc_chk_packed;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_s2_qc_off;
  len = (size_t)d->stage2_blocks_qc * sizeof(uint16_t);
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_s2_qc_val;
  len = s2_qc_val_packed;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_s2_qc_chk;
  len = s2_qc_chk_packed;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_decomp_sparse;
  len = (size_t)decomp_sparse_count * NFC_DECOMP_SPARSE_ENTRY_BYTES;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_decomp_data;
  len = (size_t)decomp_data_packed_size;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }
  off = off_comp;
  len = (size_t)comp_size * NFC_COMP_ENTRY_BYTES;
  if (!nfc_bounds_ok(off, len, sz)) {
    goto bin_fail;
  }

  d->stage1_top = (uint8_t*)(data + off_s1_top);
  d->pair_map_ccc = (uint8_t*)(data + off_pair_ccc);
  d->pair_map_qc = (uint8_t*)(data + off_pair_qc);

  d->stage2_ccc_off = (uint16_t*)(data + off_s2_ccc_off);

  d->stage2_ccc_val = (uint8_t*)malloc(s2_ccc_size);
  d->stage2_ccc_chk = (uint8_t*)malloc(s2_ccc_size);
  if (!d->stage2_ccc_val || !d->stage2_ccc_chk) {
    goto bin_fail;
  }
  if (nfc_rle_decode((const uint8_t*)(data + off_s2_ccc_val), s2_ccc_val_packed, d->stage2_ccc_val, s2_ccc_size) != 0) {
    goto bin_fail;
  }
  if (nfc_rle_decode((const uint8_t*)(data + off_s2_ccc_chk), s2_ccc_chk_packed, d->stage2_ccc_chk, s2_ccc_size) != 0) {
    goto bin_fail;
  }

  d->stage2_qc_off = (uint16_t*)(data + off_s2_qc_off);
  d->stage2_qc_val = (uint8_t*)malloc(s2_qc_size);
  d->stage2_qc_chk = (uint8_t*)malloc(s2_qc_size);
  if (!d->stage2_qc_val || !d->stage2_qc_chk) {
    goto bin_fail;
  }
  if (nfc_rle_decode((const uint8_t*)(data + off_s2_qc_val), s2_qc_val_packed, d->stage2_qc_val, s2_qc_size) != 0) {
    goto bin_fail;
  }
  if (nfc_rle_decode((const uint8_t*)(data + off_s2_qc_chk), s2_qc_chk_packed, d->stage2_qc_chk, s2_qc_size) != 0) {
    goto bin_fail;
  }

  {
    size_t bytes_per_chunk = (NFC_STAGE1_CHUNK_ENTRIES * chunk_bits + 7u) / 8u;
    const uint8_t* src = (const uint8_t*)(data + off_s1_chunks);
    d->stage1_chunks = (uint8_t*)malloc((size_t)s1_num_chunks * NFC_STAGE1_CHUNK_ENTRIES);
    if (!d->stage1_chunks) {
      goto bin_fail;
    }
    unsigned mask = (1u << chunk_bits) - 1u;
    for (uint32_t c = 0; c < s1_num_chunks; ++c) {
      size_t in = c * bytes_per_chunk;
      for (int i = 0; i < NFC_STAGE1_CHUNK_ENTRIES; ++i) {
        unsigned bit = (unsigned)i * chunk_bits;
        unsigned byte_off = bit / 8;
        unsigned bit_off = bit % 8;
        unsigned val = (unsigned)src[in + byte_off] >> bit_off;
        if (bit_off + chunk_bits > 8 && byte_off + 1 < bytes_per_chunk) {
          val |= (unsigned)(src[in + byte_off + 1] & ((1u << (bit_off + chunk_bits - 8)) - 1u)) << (8 - bit_off);
        }
        d->stage1_chunks[c * NFC_STAGE1_CHUNK_ENTRIES + i] = (uint8_t)(val & mask);
      }
    }
  }

  /* Unpack decomp_data (on disk: 1 byte len + 3 bytes per cp per record) */
  d->decomp_data = (uint32_t*)malloc((size_t)decomp_data_len * sizeof(uint32_t));
  if (!d->decomp_data) {
    goto bin_fail;
  }
  d->decomp_data[0] = 0;
  decomp_data_src = (const uint8_t*)(data + off_decomp_data);
  out_idx = 1;
  decomp_remaining = (size_t)decomp_data_packed_size;
  while (decomp_remaining > 0 && out_idx < (size_t)decomp_data_len) {
    if (decomp_remaining < NFC_DECOMP_RECORD_LEN_BYTES) {
      goto bin_fail;
    }
    uint32_t rec_len = (uint32_t)decomp_data_src[0];
    if (rec_len == 0 || rec_len > NFC_DECOMP_MAX) {
      goto bin_fail;
    }
    size_t rec_bytes = NFC_DECOMP_RECORD_LEN_BYTES + NFC_DECOMP_CP_BYTES * (size_t)rec_len;
    if (decomp_remaining < rec_bytes) {
      goto bin_fail;
    }
    d->decomp_data[out_idx++] = rec_len;
    for (uint32_t j = 0; j < rec_len; ++j) {
      size_t off = NFC_DECOMP_RECORD_LEN_BYTES + (size_t)j * NFC_DECOMP_CP_BYTES;
      d->decomp_data[out_idx++] = (uint32_t)decomp_data_src[off] | ((uint32_t)decomp_data_src[off + 1] << 8) | ((uint32_t)decomp_data_src[off + 2] << 16);
    }
    decomp_data_src += rec_bytes;
    decomp_remaining -= rec_bytes;
  }
  if (out_idx != (size_t)decomp_data_len) {
    goto bin_fail;
  }

  /* Keep decomp sparse as NFC_DECOMP_SPARSE_ENTRY_BYTES per entry in memory (no unpack) */
  if (decomp_sparse_count > 0) {
    decomp_buf = (uint8_t*)malloc((size_t)decomp_sparse_count * NFC_DECOMP_SPARSE_ENTRY_BYTES);
    if (!decomp_buf) {
      goto bin_fail;
    }
    memcpy(decomp_buf, data + off_decomp_sparse, (size_t)decomp_sparse_count * NFC_DECOMP_SPARSE_ENTRY_BYTES);
  }
  d->decomp_sparse = decomp_buf;

  /* Unpack comp table (3×21-bit per entry, 8 bytes on disk) */
  d->comp_table = (NfcCompEntry*)malloc((size_t)comp_size * sizeof(NfcCompEntry));
  if (!d->comp_table) {
    goto bin_fail;
  }
  comp_src = (const uint8_t*)(data + off_comp);
  for (uint32_t i = 0; i < comp_size; ++i) {
    uint64_t v = (uint64_t)comp_src[0] | ((uint64_t)comp_src[1] << 8) | ((uint64_t)comp_src[2] << 16) | ((uint64_t)comp_src[3] << 24) |
                 ((uint64_t)comp_src[4] << 32) | ((uint64_t)comp_src[5] << 40) | ((uint64_t)comp_src[6] << 48) | ((uint64_t)comp_src[7] << 56);
    d->comp_table[i].starter = (uint32_t)(v & NFC_COMP_CP_MASK);
    d->comp_table[i].combining = (uint32_t)((v >> NFC_COMP_CP_BITS) & NFC_COMP_CP_MASK);
    d->comp_table[i].composed = (uint32_t)((v >> (2 * NFC_COMP_CP_BITS)) & NFC_COMP_CP_MASK);
    comp_src += NFC_COMP_ENTRY_BYTES;
  }

  /* Build speed structures: dense CCC/QC, decomp_idx, comp hash */
  d->ccc_dense = (uint8_t*)malloc(NFC_MAX_CP);
  d->qc_dense = (uint8_t*)malloc(NFC_MAX_CP);
  if (!d->ccc_dense || !d->qc_dense) {
    goto bin_fail;
  }
  for (uint32_t cp = 0; cp < NFC_MAX_CP; cp++) {
    uint32_t block_idx = cp >> NFC_BLOCK_SHIFT;
    uint8_t chunk_idx = d->stage1_top[block_idx >> NFC_STAGE1_CHUNK_BITS];
    uint8_t pair_id = d->stage1_chunks[((size_t)chunk_idx << NFC_STAGE1_CHUNK_BITS) + (block_idx & (NFC_STAGE1_CHUNK_ENTRIES - 1))];
    uint8_t ccc_blk = d->pair_map_ccc[pair_id];
    uint32_t ccc_off = d->stage2_ccc_off[ccc_blk] + (cp & NFC_BLOCK_MASK);
    d->ccc_dense[cp] = (d->stage2_ccc_chk[ccc_off] == ccc_blk) ? d->stage2_ccc_val[ccc_off] : 0;
    uint8_t qc_blk = d->pair_map_qc[pair_id];
    uint32_t qc_off = d->stage2_qc_off[qc_blk] + (cp & NFC_BLOCK_MASK);
    d->qc_dense[cp] = (d->stage2_qc_chk[qc_off] == qc_blk) ? d->stage2_qc_val[qc_off] : NFC_QC_YES;
  }
  /* Size hash: next power of 2 >= 2*comp_size, between NFC_COMP_HASH_MIN and NFC_COMP_HASH_MAX */
  hash_cap = NFC_COMP_HASH_MIN;
  while (hash_cap < (size_t)comp_size * 2u && hash_cap < NFC_COMP_HASH_MAX) {
    hash_cap *= 2;
  }
  d->comp_hash = (NfcCompEntry*)calloc(hash_cap, sizeof(NfcCompEntry));
  if (!d->comp_hash) {
    goto bin_fail;
  }
  d->comp_hash_cap = hash_cap;
  for (uint32_t i = 0; i < comp_size; i++) {
    uint32_t st = d->comp_table[i].starter, cb = d->comp_table[i].combining, co = d->comp_table[i].composed;
    size_t j = ((size_t)st * 31u + (size_t)cb) % hash_cap;
    while (d->comp_hash[j].composed != 0 || d->comp_hash[j].starter != 0 || d->comp_hash[j].combining != 0) {
      j = (j + 1u) % hash_cap;
    }
    d->comp_hash[j].starter = st;
    d->comp_hash[j].combining = cb;
    d->comp_hash[j].composed = co;
  }
  free(d->comp_table);
  d->comp_table = NULL;
  d->comp_size = 0;

  return d;

bin_fail:
  lighter_map_close(&d->map);
  if (d) {
    free(d->stage1_chunks);
    free(d->stage2_ccc_val);
    free(d->stage2_ccc_chk);
    free(d->stage2_qc_val);
    free(d->stage2_qc_chk);
    free(d->decomp_data);
    free((void*)d->decomp_sparse);
    free(d->comp_table);
    free(d->comp_hash);
    free(d->ccc_dense);
    free(d->qc_dense);
    free(d->decomp_idx);
    free(d);
  }
  return NULL;
}

/* Lazy load: call from slow path only (e.g. when quick-check fast path fails or we normalize). */
static NfcData* nfc_cached = NULL;
static int nfc_load_tried = 0;
static inline NfcData* nfc_get_or_load(const char* path) {
  if (!nfc_load_tried) {
    nfc_load_tried = 1;
    nfc_cached = nfc_load_binary(path);
    if (!nfc_cached) {
      fprintf(stderr, "lighter: warning: %s missing or invalid; NFC normalization disabled\n", path);
    }
  }
  return nfc_cached;
}

#endif /* UNICODE_NFC_RUNTIME_H */
