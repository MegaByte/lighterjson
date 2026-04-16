/**
 * @file   gen_unicode_tables.c
 * @brief  Build-time tool: parse UCD files and emit binary NFC tables to stdout.
 * @usage  gen_unicode_tables <ucd_dir> > lighter.nfc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unicode_nfc_builder.h"
#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
#endif

/* RLE: escape 0x00 = literal 0xFF; escape n (3<=n<=255) + byte = run of n. */
#define RLE_ESCAPE 0xFF
#define RLE_LITERAL_FF 0x00
#define RLE_MIN_RUN 3
#define RLE_MAX_RUN 255
#define RLE_LITERAL_BYTES 2
#define RLE_RUN_BYTES 3

/* Binary layout (must match runtime; shared defines NFC_HEADER_SIZE, NFC_* layout). */
#define ALIGN_U16 2
#define ALIGN_U32 4
#define BITS_PER_BYTE 8
#define DECOMP_LEN_BYTES 1
#define DECOMP_CP_BYTES 3
#define U16_MAX 0xFFFFu

/* Returns malloc'd buffer; *out_len = compressed size. Caller free()s. */
static uint8_t* rle_encode(const uint8_t* src, size_t len, size_t* out_len) {
  size_t cap = len * 2 + 1; /* worst case: every byte RLE_ESCAPE -> 2x */
  uint8_t* out = (uint8_t*)malloc(cap);
  if (!out) {
    return NULL;
  }
  size_t o = 0;
  size_t i = 0;
  while (i < len) {
    uint8_t b = src[i];
    if (b == RLE_ESCAPE) {
      if (o + RLE_LITERAL_BYTES > cap) {
        free(out);
        return NULL;
      }
      out[o++] = RLE_ESCAPE;
      out[o++] = RLE_LITERAL_FF;
      i++;
      continue;
    }
    size_t run = 1;
    while (i + run < len && run < RLE_MAX_RUN && src[i + run] == b) {
      run++;
    }
    if (run >= RLE_MIN_RUN) {
      if (o + RLE_RUN_BYTES > cap) {
        free(out);
        return NULL;
      }
      out[o++] = RLE_ESCAPE;
      out[o++] = (uint8_t)run;
      out[o++] = b;
      i += run;
    } else {
      if (o + run > cap) {
        free(out);
        return NULL;
      }
      while (run--) {
        out[o++] = src[i++];
      }
    }
  }
  *out_len = o;
  return out;
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <ucd_dir>\n", argv[0]);
    return 1;
  }

  char* ucd_dir = argv[1];
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  NfcData* d = nfc_load_from_ucd(ucd_dir);
  if (!d) {
    fprintf(stderr, "Failed to load UCD from %s\n", ucd_dir);
    return 1;
  }

  fprintf(stderr,
          "Trie CCC: %u blocks, Trie QC: %u blocks, Decompositions: %zu "
          "entries, Compositions: %zu entries (comp table size %zu)\n",
          d->stage2_blocks_ccc, d->stage2_blocks_qc, d->decomp_data_len, d->comp_size, d->comp_size);

  /* Count sparse decomp entries for header */
  size_t nz = 0;
  for (uint32_t cp = 0; cp < NFC_MAX_CP; ++cp) {
    if (d->decomp_idx[cp]) {
      ++nz;
    }
  }

  /* Version 2: compact header + bit-packed stage1 chunks */
  size_t top_size = (NFC_BLOCK_COUNT + NFC_STAGE1_CHUNK_ENTRIES - 1u) / NFC_STAGE1_CHUNK_ENTRIES;
  unsigned s1_chunk_bits = 1;
  if (d->stage1_num_pairs > 1) {
    unsigned n = (unsigned)d->stage1_num_pairs;
    while ((1u << s1_chunk_bits) < n && s1_chunk_bits < BITS_PER_BYTE) {
      s1_chunk_bits++;
    }
  }
  size_t bytes_per_chunk = (NFC_STAGE1_CHUNK_ENTRIES * s1_chunk_bits + BITS_PER_BYTE - 1u) / BITS_PER_BYTE;
  size_t stage1_chunks_size = (size_t)d->stage1_num_chunks * bytes_per_chunk;

  size_t offset = NFC_HEADER_SIZE;

  uint32_t off_s1_top = (uint32_t)offset;
  offset += top_size;

  uint32_t off_s1_chunks = (uint32_t)offset;
  offset += stage1_chunks_size;

  uint32_t off_pair_ccc = (uint32_t)offset;
  offset += d->stage1_num_pairs;

  uint32_t off_pair_qc = (uint32_t)offset;
  offset += d->stage1_num_pairs;

  if (offset % ALIGN_U16 != 0) {
    ++offset;
  }

  uint32_t off_s2_ccc_off = (uint32_t)offset;
  offset += d->stage2_blocks_ccc * sizeof(uint16_t);
  if (offset % ALIGN_U16 != 0) {
    ++offset;
  }

  size_t size_ccc = 0;
  for (int i = 0; i < d->stage2_blocks_ccc; ++i) {
    size_t end = d->stage2_ccc_off[i] + NFC_BLOCK_SIZE;
    if (end > size_ccc) {
      size_ccc = end;
    }
  }
  size_t size_qc = 0;
  for (int i = 0; i < d->stage2_blocks_qc; ++i) {
    size_t end = d->stage2_qc_off[i] + NFC_BLOCK_SIZE;
    if (end > size_qc) {
      size_qc = end;
    }
  }

  /* RLE-compress stage2 pools */
  size_t ccc_val_rle = 0, ccc_chk_rle = 0, qc_val_rle = 0, qc_chk_rle = 0;
  uint8_t* rv = rle_encode(d->stage2_ccc_val, size_ccc, &ccc_val_rle);
  uint8_t* rc = rle_encode(d->stage2_ccc_chk, size_ccc, &ccc_chk_rle);
  uint8_t* qv = rle_encode(d->stage2_qc_val, size_qc, &qc_val_rle);
  uint8_t* qc = rle_encode(d->stage2_qc_chk, size_qc, &qc_chk_rle);
  if (!rv || !rc || !qv || !qc) {
    if (rv) {
      free(rv);
    }
    if (rc) {
      free(rc);
    }
    if (qv) {
      free(qv);
    }
    if (qc) {
      free(qc);
    }
    return 1;
  }

  uint32_t off_s2_ccc_val = (uint32_t)offset;
  offset += ccc_val_rle;

  uint32_t off_s2_ccc_chk = (uint32_t)offset;
  offset += ccc_chk_rle;

  uint32_t off_s2_qc_off = (uint32_t)offset;
  offset += d->stage2_blocks_qc * sizeof(uint16_t);
  if (offset % ALIGN_U16 != 0) {
    ++offset;
  }

  uint32_t off_s2_qc_val = (uint32_t)offset;
  offset += qc_val_rle;

  uint32_t off_s2_qc_chk = (uint32_t)offset;
  offset += qc_chk_rle;

  while (offset % ALIGN_U32 != 0) {
    ++offset;
  }

  uint32_t off_decomp_sparse = (uint32_t)offset;
  offset += nz * NFC_DECOMP_SPARSE_ENTRY_BYTES;

  size_t decomp_packed = 0;
  for (size_t i = 1; i < d->decomp_data_len;) {
    uint32_t len = d->decomp_data[i];
    decomp_packed += DECOMP_LEN_BYTES + DECOMP_CP_BYTES * (size_t)len;
    i += DECOMP_LEN_BYTES + (size_t)len;
  }
  uint32_t off_decomp_data = (uint32_t)offset;
  offset += decomp_packed;

  while (offset % ALIGN_U32 != 0) {
    ++offset;
  }

  uint32_t off_comp = (uint32_t)offset;
  offset += d->comp_size * NFC_COMP_ENTRY_BYTES;

  FILE* f = stdout;
  enum {
    H_MAGIC = 0,
    H_VERSION = 4,
    H_OFF_S1_TOP = 8,
    H_OFF_S1_CHUNKS = 12,
    H_OFF_PAIR_CCC = 16,
    H_OFF_PAIR_QC = 20,
    H_OFF_S2_CCC_OFF = 24,
    H_OFF_S2_CCC_VAL = 28,
    H_OFF_S2_CCC_CHK = 32,
    H_OFF_S2_QC_OFF = 36,
    H_OFF_S2_QC_VAL = 40,
    H_OFF_S2_QC_CHK = 44,
    H_OFF_DECOMP_SPARSE = 48,
    H_OFF_DECOMP_DATA = 52,
    H_OFF_COMP = 56,
    H_BLOCKS_CCC = 60,
    H_BLOCKS_QC = 62,
    H_SIZE_CCC = 64,
    H_SIZE_QC = 68,
    H_NZ = 72,
    H_COMP_SIZE = 74,
    H_NUM_CHUNKS = 76,
    H_NUM_PAIRS = 78,
    H_DECOMP_LEN = 80,
    H_DECOMP_PACKED = 84,
    H_RLE_CCC_VAL = 88,
    H_RLE_CCC_CHK = 90,
    H_RLE_QC_VAL = 92,
    H_RLE_QC_CHK = 94,
    SZ_U32 = 4,
    SZ_U16 = 2,
  };
  {
    uint8_t h[NFC_HEADER_SIZE];
    memcpy(h + H_MAGIC, (const uint8_t*)"\x43\x46\x4E\x4C", SZ_U32); /* LNFC little-endian */
    h[H_VERSION] = 2;
    h[H_VERSION + 1] = 0;
    h[H_VERSION + 2] = 0;
    h[H_VERSION + 3] = 0;
    memcpy(h + H_OFF_S1_TOP, &off_s1_top, SZ_U32);
    memcpy(h + H_OFF_S1_CHUNKS, &off_s1_chunks, SZ_U32);
    memcpy(h + H_OFF_PAIR_CCC, &off_pair_ccc, SZ_U32);
    memcpy(h + H_OFF_PAIR_QC, &off_pair_qc, SZ_U32);
    memcpy(h + H_OFF_S2_CCC_OFF, &off_s2_ccc_off, SZ_U32);
    memcpy(h + H_OFF_S2_CCC_VAL, &off_s2_ccc_val, SZ_U32);
    memcpy(h + H_OFF_S2_CCC_CHK, &off_s2_ccc_chk, SZ_U32);
    memcpy(h + H_OFF_S2_QC_OFF, &off_s2_qc_off, SZ_U32);
    memcpy(h + H_OFF_S2_QC_VAL, &off_s2_qc_val, SZ_U32);
    memcpy(h + H_OFF_S2_QC_CHK, &off_s2_qc_chk, SZ_U32);
    memcpy(h + H_OFF_DECOMP_SPARSE, &off_decomp_sparse, SZ_U32);
    memcpy(h + H_OFF_DECOMP_DATA, &off_decomp_data, SZ_U32);
    memcpy(h + H_OFF_COMP, &off_comp, SZ_U32);
    uint16_t u16;
    u16 = (uint16_t)d->stage2_blocks_ccc;
    memcpy(h + H_BLOCKS_CCC, &u16, SZ_U16);
    u16 = (uint16_t)d->stage2_blocks_qc;
    memcpy(h + H_BLOCKS_QC, &u16, SZ_U16);
    uint32_t u32 = (uint32_t)size_ccc;
    memcpy(h + H_SIZE_CCC, &u32, SZ_U32);
    u32 = (uint32_t)size_qc;
    memcpy(h + H_SIZE_QC, &u32, SZ_U32);
    u16 = (uint16_t)(nz > U16_MAX ? U16_MAX : (uint32_t)nz);
    memcpy(h + H_NZ, &u16, SZ_U16);
    u16 = (uint16_t)d->comp_size;
    memcpy(h + H_COMP_SIZE, &u16, SZ_U16);
    u16 = d->stage1_num_chunks;
    memcpy(h + H_NUM_CHUNKS, &u16, SZ_U16);
    u16 = (uint16_t)d->stage1_num_pairs;
    memcpy(h + H_NUM_PAIRS, &u16, SZ_U16);
    u32 = (uint32_t)d->decomp_data_len;
    memcpy(h + H_DECOMP_LEN, &u32, SZ_U32);
    u32 = (uint32_t)decomp_packed;
    memcpy(h + H_DECOMP_PACKED, &u32, SZ_U32);
    u16 = (uint16_t)(ccc_val_rle > U16_MAX ? U16_MAX : (uint32_t)ccc_val_rle);
    memcpy(h + H_RLE_CCC_VAL, &u16, SZ_U16);
    u16 = (uint16_t)(ccc_chk_rle > U16_MAX ? U16_MAX : (uint32_t)ccc_chk_rle);
    memcpy(h + H_RLE_CCC_CHK, &u16, SZ_U16);
    u16 = (uint16_t)(qc_val_rle > U16_MAX ? U16_MAX : (uint32_t)qc_val_rle);
    memcpy(h + H_RLE_QC_VAL, &u16, SZ_U16);
    u16 = (uint16_t)(qc_chk_rle > U16_MAX ? U16_MAX : (uint32_t)qc_chk_rle);
    memcpy(h + H_RLE_QC_CHK, &u16, SZ_U16);
    fwrite(h, 1, NFC_HEADER_SIZE, f);
  }
  fwrite(d->stage1_top, 1, top_size, f);
  /* Bit-packed stage1 chunks: s1_chunk_bits per entry */
  {
    const uint8_t* src = d->stage1_chunks;
    uint8_t* buf = (uint8_t*)malloc(stage1_chunks_size);
    if (!buf) {
      fclose(f);
      return 1;
    }
    memset(buf, 0, stage1_chunks_size);
    size_t out = 0;
    unsigned mask = (1u << s1_chunk_bits) - 1u;
    for (uint16_t c = 0; c < d->stage1_num_chunks; ++c) {
      unsigned bit = 0;
      for (int i = 0; i < NFC_STAGE1_CHUNK_ENTRIES; ++i) {
        unsigned val = (unsigned)src[c * NFC_STAGE1_CHUNK_ENTRIES + i] & mask;
        unsigned byte_off = bit / BITS_PER_BYTE;
        unsigned bit_off = bit % BITS_PER_BYTE;
        buf[out + byte_off] |= (uint8_t)(val << bit_off);
        if (bit_off + s1_chunk_bits > BITS_PER_BYTE) {
          buf[out + byte_off + 1] |= (uint8_t)(val >> (BITS_PER_BYTE - bit_off));
        }
        bit += s1_chunk_bits;
      }
      out += bytes_per_chunk;
    }
    fwrite(buf, 1, stage1_chunks_size, f);
    free(buf);
  }
  fwrite(d->pair_map_ccc, 1, d->stage1_num_pairs, f);
  fwrite(d->pair_map_qc, 1, d->stage1_num_pairs, f);

  if (ftell(f) % ALIGN_U16 != 0) {
    fputc(0, f);
  }

  fwrite(d->stage2_ccc_off, sizeof(uint16_t), d->stage2_blocks_ccc, f);
  if (ftell(f) % ALIGN_U16 != 0) {
    fputc(0, f);
  }
  fwrite(rv, 1, ccc_val_rle, f);
  fwrite(rc, 1, ccc_chk_rle, f);
  free(rv);
  free(rc);

  fwrite(d->stage2_qc_off, sizeof(uint16_t), d->stage2_blocks_qc, f);
  if (ftell(f) % ALIGN_U16 != 0) {
    fputc(0, f);
  }
  fwrite(qv, 1, qc_val_rle, f);
  fwrite(qc, 1, qc_chk_rle, f);
  free(qv);
  free(qc);

  while (ftell(f) % ALIGN_U32 != 0) {
    fputc(0, f);
  }

  /* Pack decomp sparse: DECOMP_SPARSE_ENTRY_BYTES per entry */
  for (uint32_t cp = 0; cp < NFC_MAX_CP; ++cp) {
    if (d->decomp_idx[cp]) {
      uint32_t idx = d->decomp_idx[cp];
      uint8_t buf[NFC_DECOMP_SPARSE_ENTRY_BYTES];
      buf[0] = (uint8_t)(cp);
      buf[1] = (uint8_t)(cp >> 8);
      buf[2] = (uint8_t)(cp >> 16);
      buf[3] = (uint8_t)(idx);
      buf[4] = (uint8_t)(idx >> 8);
      buf[5] = (uint8_t)(idx >> 16);
      fwrite(buf, 1, NFC_DECOMP_SPARSE_ENTRY_BYTES, f);
    }
  }

  /* Pack decomp_data: 1 byte length + DECOMP_CP_BYTES per code point per record (skip idx 0) */
  for (size_t i = 1; i < d->decomp_data_len;) {
    uint32_t len = d->decomp_data[i];
    fputc((int)(len & 0xFF), f);
    for (uint32_t j = 0; j < len; ++j) {
      uint32_t cp = d->decomp_data[i + DECOMP_LEN_BYTES + j];
      uint8_t b[DECOMP_CP_BYTES] = {(uint8_t)(cp), (uint8_t)(cp >> 8), (uint8_t)(cp >> 16)};
      fwrite(b, 1, DECOMP_CP_BYTES, f);
    }
    i += DECOMP_LEN_BYTES + (size_t)len;
  }

  while (ftell(f) % ALIGN_U32 != 0) {
    fputc(0, f);
  }

  /* Pack comp table: 3×COMP_CP_BITS code points per entry */
  for (size_t i = 0; i < d->comp_size; ++i) {
    uint64_t packed = (uint64_t)d->comp_table[i].starter | ((uint64_t)d->comp_table[i].combining << NFC_COMP_CP_BITS) |
                      ((uint64_t)d->comp_table[i].composed << (2 * NFC_COMP_CP_BITS));
    uint8_t buf[NFC_COMP_ENTRY_BYTES];
    buf[0] = (uint8_t)(packed);
    buf[1] = (uint8_t)(packed >> 8);
    buf[2] = (uint8_t)(packed >> 16);
    buf[3] = (uint8_t)(packed >> 24);
    buf[4] = (uint8_t)(packed >> 32);
    buf[5] = (uint8_t)(packed >> 40);
    buf[6] = (uint8_t)(packed >> 48);
    buf[7] = (uint8_t)(packed >> 56);
    fwrite(buf, 1, NFC_COMP_ENTRY_BYTES, f);
  }

  return 0;
}
