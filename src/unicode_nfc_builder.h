/**
 * @file      unicode_nfc_builder.h
 * @brief     Unicode NFC build-time: UCD parser, trie and composition table builders.
 * @details   For gen_unicode_tables.c. UAX #15.
 */
#ifndef UNICODE_NFC_BUILDER_H
#define UNICODE_NFC_BUILDER_H
#include <stdlib.h>
#include <string.h>

#include "unicode_nfc_shared.h"

/** Parse one hexadecimal code point field from a UCD line. */
static int nfc_parse_hex(const char* s, uint32_t* cp) {
  *cp = 0;
  int i = 0;
  for (; s[i]; ++i) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      *cp = (*cp << 4) | (uint32_t)(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      *cp = (*cp << 4) | (uint32_t)(c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
      *cp = (*cp << 4) | (uint32_t)(c - 'a' + 10);
    } else {
      break;
    }
  }
  return i;
}

static const char* nfc_next_field(const char* s) {
  while (*s && *s != ';') {
    ++s;
  }
  return *s == ';' ? s + 1 : s;
}

static const char* nfc_skip_ws(const char* s) {
  while (*s == ' ' || *s == '\t') {
    ++s;
  }
  return s;
}

/* ── Trie builder ──────────────────────────────────────────────────── */

/** Compress one stage-2 table with row-displacement packing. */
static void nfc_compress_stage2(uint8_t* stage2_raw, uint16_t blocks, uint8_t def_val, uint16_t** offsets, uint8_t** val, uint8_t** chk) {
  /* Row displacement compression with a linear upper bound for the packed pool. */
  size_t pool_cap = (size_t)blocks * NFC_BLOCK_SIZE;
  *val = (uint8_t*)calloc(pool_cap, 1);
  *chk = (uint8_t*)malloc(pool_cap);
  memset(*chk, 0xFF, pool_cap); /* Initialize the check array to "invalid block". */
  *offsets = (uint16_t*)calloc(blocks, sizeof(uint16_t));

  /* Pack blocks with a greedy first-fit pass. */

  for (uint16_t b = 0; b < blocks; ++b) {
    const uint8_t* blk_data = stage2_raw + (size_t)b * NFC_BLOCK_SIZE;
    /* Find the first offset where every non-default byte can fit. */
    size_t off = 0;
    while (1) {
      int fits = 1;
      for (int i = 0; i < NFC_BLOCK_SIZE; ++i) {
        if (blk_data[i] != def_val) {    /* Non-default values need an empty slot. */
          if ((*chk)[off + i] != 0xFF) { /* This slot is already occupied. */
            fits = 0;
            break;
          }
        }
      }
      if (fits) {
        break;
      }
      ++off;
    }

    (*offsets)[b] = (uint16_t)off;
    /* Place the block at the chosen offset. */
    for (int i = 0; i < NFC_BLOCK_SIZE; ++i) {
      if (blk_data[i] != def_val) {
        (*val)[off + i] = blk_data[i];
        (*chk)[off + i] = (uint8_t)b;
      }
    }
  }

  /* The check array stays initialized to 0xFF for unmapped slots. Keep the
   * original allocation instead of shrinking the buffers here. */
}

/** Deduplicate NFC blocks and build one packed trie half. */
static void nfc_build_one_trie(const uint8_t* raw, uint8_t** stage1, uint16_t** s2_off, uint8_t** s2_val, uint8_t** s2_chk, uint16_t* blocks, uint8_t def) {
  *stage1 = (uint8_t*)calloc(NFC_BLOCK_COUNT, sizeof(uint8_t));
  uint8_t* tmp = (uint8_t*)malloc((size_t)NFC_BLOCK_COUNT * NFC_BLOCK_SIZE);
  uint16_t unique = 0;

  for (uint32_t b = 0; b < NFC_BLOCK_COUNT; ++b) {
    const uint8_t* blk = raw + b * NFC_BLOCK_SIZE;
    uint16_t found = unique;
    for (uint16_t u = 0; u < unique; ++u) {
      if (!memcmp(tmp + u * NFC_BLOCK_SIZE, blk, NFC_BLOCK_SIZE)) {
        found = u;
        break;
      }
    }
    if (found == unique) {
      if (unique >= 256) {
        fprintf(stderr, "Error: too many unique Trie blocks for uint8_t index (%u)\n", unique);
        exit(1);
      }
      memcpy(tmp + unique * NFC_BLOCK_SIZE, blk, NFC_BLOCK_SIZE);
      ++unique;
    }
    (*stage1)[b] = (uint8_t)found;
  }

  *blocks = unique;

  /* Compress stage 2. */
  nfc_compress_stage2(tmp, unique, def, s2_off, s2_val, s2_chk);

  free(tmp);
}

/** Build the combined CCC and quick-check trie structures. */
static void nfc_build_trie(NfcData* d, const uint8_t* raw_ccc, const uint8_t* raw_qc) {
  uint8_t* s1_ccc = NULL;
  uint8_t* s1_qc = NULL;

  /* Build stage 2 and recover the raw stage-1 block indices. */
  nfc_build_one_trie(raw_ccc, &s1_ccc, &d->stage2_ccc_off, &d->stage2_ccc_val, &d->stage2_ccc_chk, &d->stage2_blocks_ccc, 0);
  nfc_build_one_trie(raw_qc, &s1_qc, &d->stage2_qc_off, &d->stage2_qc_val, &d->stage2_qc_chk, &d->stage2_blocks_qc, NFC_QC_YES);

  /* Compress stage 1 into the combined three-stage trie by mapping each
   * (ccc_block, qc_block) pair to a compact pair id. */
  uint8_t pair_ccc[256];
  uint8_t pair_qc[256];
  int num_pairs = 0;

  /* Temporary array of pair ids, one per block. */
  uint8_t* block_pairs = (uint8_t*)malloc(NFC_BLOCK_COUNT);

  for (int i = 0; i < NFC_BLOCK_COUNT; ++i) {
    uint8_t c = s1_ccc[i];
    uint8_t q = s1_qc[i];
    int pid = -1;
    for (int j = 0; j < num_pairs; ++j) {
      if (pair_ccc[j] == c && pair_qc[j] == q) {
        pid = j;
        break;
      }
    }
    if (pid < 0) {
      if (num_pairs >= 256) {
        fprintf(stderr, "Error: Stage1 pair count > 256\n");
        exit(1);
      }
      pid = num_pairs;
      pair_ccc[pid] = c;
      pair_qc[pid] = q;
      ++num_pairs;
    }
    block_pairs[i] = (uint8_t)pid;
  }

  d->stage1_num_pairs = (uint8_t)num_pairs;
  d->pair_map_ccc = (uint8_t*)malloc(num_pairs);
  d->pair_map_qc = (uint8_t*)malloc(num_pairs);
  memcpy(d->pair_map_ccc, pair_ccc, num_pairs);
  memcpy(d->pair_map_qc, pair_qc, num_pairs);

  /* Compress pair ids into deduplicated chunks of 32 blocks. */
  int num_chunks_raw = NFC_BLOCK_COUNT / 32;        /* 136 */
  d->stage1_top = (uint8_t*)malloc(num_chunks_raw); /* Top index */

  /* Deduplicate identical chunks. */
  uint8_t* unique_chunk_data = (uint8_t*)malloc(num_chunks_raw * 32);
  int unique_chunks = 0;

  for (int c = 0; c < num_chunks_raw; ++c) {
    uint8_t* chunk = block_pairs + c * 32;
    int found_cid = -1;
    for (int u = 0; u < unique_chunks; ++u) {
      if (!memcmp(unique_chunk_data + u * 32, chunk, 32)) {
        found_cid = u;
        break;
      }
    }
    if (found_cid < 0) {
      found_cid = unique_chunks;
      memcpy(unique_chunk_data + unique_chunks * 32, chunk, 32);
      ++unique_chunks;
    }
    d->stage1_top[c] = (uint8_t)found_cid;
  }

  d->stage1_num_chunks = (uint16_t)unique_chunks;
  d->stage1_chunks = (uint8_t*)malloc(unique_chunks * 32);
  memcpy(d->stage1_chunks, unique_chunk_data, unique_chunks * 32);

  free(unique_chunk_data);
  free(block_pairs);
  free(s1_ccc);
  free(s1_qc);
}

/* ── Composition table builder ─────────────────────────────────────── */

/** Compare composition entries by starter, then combining code point. */
static int nfc_build_comp_cmp(const void* a, const void* b) {
  const NfcCompEntry* ea = (const NfcCompEntry*)a;
  const NfcCompEntry* eb = (const NfcCompEntry*)b;
  if (ea->starter != eb->starter) {
    return (ea->starter < eb->starter) ? -1 : 1;
  }
  return (ea->combining < eb->combining) ? -1 : 1;
}

/** Build and sort the NFC composition table from loaded UCD data. */
static void nfc_build_comp_table(NfcData* d, const uint8_t* raw_ccc, const uint8_t* comp_excl) {
  /* Count composable pairs */
  size_t count = 0;
  for (uint32_t cp = 0; cp < NFC_MAX_CP; ++cp) {
    if (comp_excl[cp]) {
      continue;
    }
    uint32_t idx = d->decomp_idx[cp];
    if (!idx) {
      continue;
    }
    if (d->decomp_data[idx] != 2) {
      continue; /* only pairs */
    }
    if (raw_ccc[d->decomp_data[idx + 1]] != 0) {
      continue; /* first must be starter */
    }
    ++count;
  }

  /* Allocate dense array */
  d->comp_table = (NfcCompEntry*)malloc(count * sizeof(NfcCompEntry));
  d->comp_size = count;
  size_t added = 0;

  /* Fill array */
  for (uint32_t cp = 0; cp < NFC_MAX_CP; ++cp) {
    if (comp_excl[cp]) {
      continue;
    }
    uint32_t idx = d->decomp_idx[cp];
    if (!idx) {
      continue;
    }
    if (d->decomp_data[idx] != 2) {
      continue;
    }
    uint32_t first = d->decomp_data[idx + 1];
    uint32_t second = d->decomp_data[idx + 2];
    if (raw_ccc[first] != 0) {
      continue;
    }

    d->comp_table[added].starter = first;
    d->comp_table[added].combining = second;
    d->comp_table[added].composed = cp;
    ++added;
  }

  /* Sort entries for binary search */
  qsort(d->comp_table, d->comp_size, sizeof(NfcCompEntry), nfc_build_comp_cmp);
}

/* ── Main UCD loader ───────────────────────────────────────────────── */

/** Advance p to the first byte of the next line or fend. */
static void nfc_skip_to_next_line(const char** p, const char* fend) {
  while (*p < fend && **p != '\n') {
    ++*p;
  }
  if (*p < fend) {
    ++*p;
  }
}

static NfcData* nfc_load_from_ucd(const char* ucd_dir) {
  NfcData* d = (NfcData*)calloc(1, sizeof(NfcData));
  uint8_t* raw_ccc = (uint8_t*)calloc(NFC_MAX_CP, 1);
  uint8_t* raw_qc = (uint8_t*)calloc(NFC_MAX_CP, 1);
  uint8_t* comp_excl = (uint8_t*)calloc(NFC_MAX_CP, 1);

  d->decomp_idx = (uint32_t*)calloc(NFC_MAX_CP, sizeof(uint32_t));
  size_t decomp_cap = 8192;
  d->decomp_data = (uint32_t*)malloc(decomp_cap * sizeof(uint32_t));
  d->decomp_data[0] = 0; /* sentinel: index 0 = no decomposition */
  d->decomp_data_len = 1;

  do {
    /* ── UnicodeData.txt ──────────────────────────────────────────── */
    {
      char path[4096];
      snprintf(path, sizeof(path), "%s/UnicodeData.txt", ucd_dir);
      LighterMap map = {0};
      if (lighter_map_open(&map, path, 1) != 0) {
        break;
      }
      const char* data = (const char*)map.data;
      size_t fsize = map.size;
      const char* p = data;
      const char* fend = data + fsize;
      while (p < fend) {
        uint32_t cp;
        const char* f;
        const char* f5;
        int consumed = nfc_parse_hex(p, &cp);

        if (!consumed || cp >= NFC_MAX_CP) {
          nfc_skip_to_next_line(&p, fend);
          continue;
        }

        /* Field 3: CCC */
        f = p;
        for (int i = 0; i < 3; ++i) {
          f = nfc_next_field(f);
        }
        {
          const char* s = nfc_skip_ws(f);
          uint32_t ccc = 0;
          while (*s >= '0' && *s <= '9') {
            ccc = ccc * 10 + (uint32_t)(*s++ - '0');
          }
          raw_ccc[cp] = (uint8_t)(ccc > 254 ? 254 : ccc);
        }

        /* Field 5: Decomposition mapping */
        f5 = f;
        for (int i = 0; i < 2; ++i) {
          f5 = nfc_next_field(f5);
        }
        {
          const char* s = nfc_skip_ws(f5);
          if (*s == '<') {
            nfc_skip_to_next_line(&p, fend);
            continue;
          }
          if (*s == ';' || *s == '\n' || *s == '\r') {
            nfc_skip_to_next_line(&p, fend);
            continue;
          }

          uint32_t parts[NFC_DECOMP_MAX];
          int nparts = 0;
          while (nparts < NFC_DECOMP_MAX) {
            s = nfc_skip_ws(s);
            if (!*s || *s == ';' || *s == '\n' || *s == '\r') {
              break;
            }
            uint32_t dcp;
            int c = nfc_parse_hex(s, &dcp);
            if (!c) {
              break;
            }
            parts[nparts++] = dcp;
            s += c;
          }
          if (nparts > 0) {
            size_t need = d->decomp_data_len + 1 + (size_t)nparts;
            while (need > decomp_cap) {
              decomp_cap *= 2;
              d->decomp_data = (uint32_t*)realloc(d->decomp_data, decomp_cap * sizeof(uint32_t));
            }
            d->decomp_idx[cp] = (uint32_t)d->decomp_data_len;
            d->decomp_data[d->decomp_data_len++] = (uint32_t)nparts;
            for (int i = 0; i < nparts; ++i) {
              d->decomp_data[d->decomp_data_len++] = parts[i];
            }
          }
        }

        nfc_skip_to_next_line(&p, fend);
      }
      lighter_map_close(&map);
    }

    /* ── DerivedNormalizationProps.txt (NFC_QC) ───────────────────── */
    {
      char path[4096];
      snprintf(path, sizeof(path), "%s/DerivedNormalizationProps.txt", ucd_dir);
      LighterMap map = {0};
      if (lighter_map_open(&map, path, 1) == 0) {
        const char* data = (const char*)map.data;
        size_t fsize = map.size;
        const char* p = data;
        const char* fend = data + fsize;
        while (p < fend) {
          uint32_t first, last;
          int c;
          const char* s = nfc_skip_ws(p);

          if (*s == '#' || *s == '\n' || *s == '\r') {
            nfc_skip_to_next_line(&p, fend);
            continue;
          }

          c = nfc_parse_hex(s, &first);
          if (!c) {
            nfc_skip_to_next_line(&p, fend);
            continue;
          }
          s += c;
          if (s[0] == '.' && s[1] == '.') {
            s += 2;
            c = nfc_parse_hex(s, &last);
            s += c;
          } else {
            last = first;
          }
          s = nfc_skip_ws(s);
          if (*s == ';') {
            ++s;
          }
          s = nfc_skip_ws(s);
          if (strncmp(s, "NFC_QC", 6)) {
            nfc_skip_to_next_line(&p, fend);
            continue;
          }
          s += 6;
          s = nfc_skip_ws(s);
          if (*s == ';') {
            ++s;
          }
          s = nfc_skip_ws(s);

          uint8_t qv = NFC_QC_YES;
          if (*s == 'N') {
            qv = NFC_QC_NO;
          } else if (*s == 'M') {
            qv = NFC_QC_MAYBE;
          } else {
            nfc_skip_to_next_line(&p, fend);
            continue;
          }

          for (uint32_t cp = first; cp <= last && cp < NFC_MAX_CP; ++cp) {
            raw_qc[cp] = qv;
          }

          nfc_skip_to_next_line(&p, fend);
        }
        lighter_map_close(&map);
      }
    }

    /* ── CompositionExclusions.txt ────────────────────────────────── */
    {
      char path[4096];
      snprintf(path, sizeof(path), "%s/CompositionExclusions.txt", ucd_dir);
      LighterMap map = {0};
      if (lighter_map_open(&map, path, 1) == 0) {
        const char* data = (const char*)map.data;
        size_t fsize = map.size;
        const char* p = data;
        const char* fend = data + fsize;
        while (p < fend) {
          uint32_t cp;
          int c;
          const char* s = nfc_skip_ws(p);

          if (*s == '#' || *s == '\n' || *s == '\r') {
            nfc_skip_to_next_line(&p, fend);
            continue;
          }
          c = nfc_parse_hex(s, &cp);
          if (c > 0 && cp < NFC_MAX_CP) {
            comp_excl[cp] = 1;
          }

          nfc_skip_to_next_line(&p, fend);
        }
        lighter_map_close(&map);
      }
    }

    /* Exclude singletons from composition */
    for (uint32_t cp = 0; cp < NFC_MAX_CP; ++cp) {
      uint32_t idx = d->decomp_idx[cp];
      if (idx && d->decomp_data[idx] == 1) {
        comp_excl[cp] = 1;
      }
    }

    /* Build data structures */
    nfc_build_trie(d, raw_ccc, raw_qc);
    nfc_build_comp_table(d, raw_ccc, comp_excl);

    free(raw_ccc);
    free(raw_qc);
    free(comp_excl);
    return d;

  } while (0);

  free(raw_ccc);
  free(raw_qc);
  free(comp_excl);
  free(d->decomp_idx);
  free(d->decomp_data);
  free(d);
  return NULL;
}

#endif /* UNICODE_NFC_BUILDER_H */
