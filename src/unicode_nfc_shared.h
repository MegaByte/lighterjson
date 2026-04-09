/**
 * @file      unicode_nfc_shared.h
 * @brief     Unicode NFC — shared types and constants.
 * @details   Used by unicode_nfc_runtime.h (lighter_string) and
 *            unicode_nfc_builder.h (gen_unicode_tables). UAX #15.
 */

#ifndef UNICODE_NFC_SHARED_H
#define UNICODE_NFC_SHARED_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lighter_memmap.h"

#if defined(__GNUC__) || defined(__clang__)
#define NFC_UNUSED __attribute__((unused))
#else
#define NFC_UNUSED
#endif

/* Check that offset + len <= size with no overflow. */
static inline int nfc_bounds_ok(size_t offset, size_t len, size_t size) {
  return len <= size && offset <= size - len;
}

/* ── Constants ─────────────────────────────────────────────────────── */

#define NFC_MAX_CP 0x110000u
#define NFC_BLOCK_SHIFT 8
#define NFC_BLOCK_SIZE (1 << NFC_BLOCK_SHIFT)
#define NFC_BLOCK_MASK (NFC_BLOCK_SIZE - 1)
#define NFC_BLOCK_COUNT (NFC_MAX_CP >> NFC_BLOCK_SHIFT)

typedef enum {
  NFC_QC_YES = 0,
  NFC_QC_MAYBE = 1,
  NFC_QC_NO = 2,
} NfcQc;

#define NFC_SEG_MAX 128

#define HANGUL_SBASE 0xAC00u
#define HANGUL_LBASE 0x1100u
#define HANGUL_VBASE 0x1161u
#define HANGUL_TBASE 0x11A7u
#define HANGUL_LCOUNT 19
#define HANGUL_VCOUNT 21
#define HANGUL_TCOUNT 28
#define HANGUL_NCOUNT (HANGUL_VCOUNT * HANGUL_TCOUNT)
#define HANGUL_SCOUNT (HANGUL_LCOUNT * HANGUL_NCOUNT)

#define NFC_DECOMP_MAX 2
/* decomp_data on disk: 1 byte length + 3 bytes per code point per record. */
#define NFC_DECOMP_RECORD_LEN_BYTES 1
#define NFC_DECOMP_CP_BYTES 3

/* Sparse decomp table: 3 bytes cp + 3 bytes idx per entry (must match binary format). */
#define NFC_DECOMP_SPARSE_ENTRY_BYTES 6
/* Stage1 trie: 32 blocks per chunk (bit-packed in binary). */
#define NFC_STAGE1_CHUNK_ENTRIES 32
#define NFC_STAGE1_CHUNK_BITS 5
/* Binary format header size (must match gen_unicode_tables output). */
#define NFC_HEADER_SIZE 96
/* Comp table: 8 bytes per entry on disk (3×21-bit code points). */
#define NFC_COMP_ENTRY_BYTES 8
#define NFC_COMP_CP_BITS 21
#define NFC_COMP_CP_MASK ((1u << NFC_COMP_CP_BITS) - 1u)
/* Comp hash table size bounds (runtime loader). */
#define NFC_COMP_HASH_MIN 256u
#define NFC_COMP_HASH_MAX 2048u

typedef struct {
  uint32_t starter;
  uint32_t combining;
  uint32_t composed;
} NfcCompEntry;

typedef struct {
  uint8_t* stage1_top;
  uint8_t* stage1_chunks;
  uint8_t* pair_map_ccc;
  uint8_t* pair_map_qc;
  uint16_t stage1_num_chunks;
  uint8_t stage1_num_pairs;
  uint16_t* stage2_ccc_off;
  uint8_t* stage2_ccc_val;
  uint8_t* stage2_ccc_chk;
  uint16_t stage2_blocks_ccc;
  uint16_t* stage2_qc_off;
  uint8_t* stage2_qc_val;
  uint8_t* stage2_qc_chk;
  uint16_t stage2_blocks_qc;
  uint32_t* decomp_idx;
  const uint8_t* decomp_sparse;
  size_t decomp_sparse_count;
  uint32_t* decomp_data;
  size_t decomp_data_len;
  NfcCompEntry* comp_table;
  size_t comp_size;
  uint8_t* ccc_dense;
  uint8_t* qc_dense;
  NfcCompEntry* comp_hash;
  size_t comp_hash_cap;
  LighterMap map; /* read-only mapping for binary loader; kept for lifetime of NfcData */
} NfcData;

static inline uint32_t nfc_decomp_sparse_cp(const NfcData* d, size_t i) {
  const uint8_t* p = d->decomp_sparse + (i * (size_t)NFC_DECOMP_SPARSE_ENTRY_BYTES);
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static inline uint32_t nfc_decomp_sparse_idx(const NfcData* d, size_t i) {
  const uint8_t* p = d->decomp_sparse + (i * (size_t)NFC_DECOMP_SPARSE_ENTRY_BYTES) + 3;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

#endif /* UNICODE_NFC_SHARED_H */
