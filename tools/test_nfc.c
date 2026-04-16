/**
 * @file   test_nfc.c
 * @brief  Test NFC normalization against official NormalizationTest.txt
 * @usage  test_nfc <lighter.nfc> <ucd_dir>
 *
 * Loads the pre-built binary NFC tables (same path as the main lighterjson
 * binary uses) and tests them against the Unicode conformance suite.
 */

#include "unicode_nfc_runtime.h"
#include "unicode_nfc_shared.h"

/** Parse one hexadecimal code point field from a string. */
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

#define MAX_CP_SEQ 64 /* max code points per NormalizationTest line */
#define UTF8_BUF_SIZE 512
#define PATH_BUF_SIZE 4096
#define MAX_FAIL_PRINT 20 /* max number of failure lines to print */

static int parse_cp_sequence(const char* s, uint32_t* cps, int max) {
  int n = 0;
  while (n < max) {
    while (*s == ' ') {
      ++s;
    }
    if (!*s || *s == ';' || *s == '#' || *s == '\n' || *s == '\r') {
      break;
    }
    uint32_t cp;
    int c = nfc_parse_hex(s, &cp);
    if (!c) {
      break;
    }
    cps[n++] = cp;
    s += c;
  }
  return n;
}

/* Encode a code point sequence to UTF-8 */
static int cps_to_utf8(const uint32_t* cps, int ncps, uint8_t* buf, int bufsize) {
  int written = 0;
  for (int i = 0; i < ncps; ++i) {
    int n = nfc_utf8_encode(buf + written, cps[i]);
    written += n;
    if (written >= bufsize - 4) { /* leave room for one more 4-byte char */
      break;
    }
  }
  return written;
}

/* Decode UTF-8 back to code points */
static int utf8_to_cps(const uint8_t* buf, int len, uint32_t* cps, int max) {
  int n = 0;
  const uint8_t* p = buf;
  const uint8_t* end = buf + len;
  while (p < end && n < max) {
    uint32_t cp;
    int c = nfc_utf8_decode(p, end, &cp);
    if (!c) {
      break;
    }
    cps[n++] = cp;
    p += c;
  }
  return n;
}

static int cps_equal(const uint32_t* a, int na, const uint32_t* b, int nb) {
  if (na != nb) {
    return 0;
  }
  for (int i = 0; i < na; ++i) {
    if (a[i] != b[i]) {
      return 0;
    }
  }
  return 1;
}

static void print_cps(const uint32_t* cps, int n) {
  for (int i = 0; i < n; ++i) {
    fprintf(stderr, "%04X ", cps[i]);
  }
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <lighter.nfc> <ucd_dir>\n", argv[0]);
    return 1;
  }

  const char* nfc_path = argv[1];
  const char* ucd_dir = argv[2];

  NfcData* d = nfc_load_binary(nfc_path);
  if (!d) {
    fprintf(stderr, "Failed to load NFC binary from %s\n", nfc_path);
    return 1;
  }

  char path[PATH_BUF_SIZE];
  snprintf(path, sizeof(path), "%s/NormalizationTest.txt", ucd_dir);
  LighterMap map = {0};
  if (lighter_map_open(&map, path, 1) != 0) {
    fprintf(stderr, "Failed to read %s\n", path);
    return 1;
  }
  const char* data = (const char*)map.data;
  size_t fsize = map.size;

  int pass = 0, fail = 0, skip = 0;
  int line_num = 0;

  const char* p = data;
  const char* end = data + fsize;
  while (p < end) {
    ++line_num;
    if (*p == '#' || *p == '@' || *p == '\n' || *p == '\r') {
      while (p < end && *p != '\n') {
        ++p;
      }
      if (p < end) {
        ++p;
      }
      continue;
    }

    /* Parse: source; NFC; NFD; NFKC; NFKD; # comment */
    /* We test: toNFC(source) == NFC column (column 2) */
    uint32_t source[MAX_CP_SEQ], expected_nfc[MAX_CP_SEQ];
    int nsrc = parse_cp_sequence(p, source, MAX_CP_SEQ);

    /* Skip to field 2 (NFC) */
    const char* f = p;
    while (*f && *f != ';') {
      ++f;
    }
    if (*f == ';') {
      ++f;
    }
    int nnfc = parse_cp_sequence(f, expected_nfc, MAX_CP_SEQ);

    if (nsrc == 0 || nnfc == 0) {
      ++skip;
      while (p < end && *p != '\n') {
        ++p;
      }
      if (p < end) {
        ++p;
      }
      continue;
    }

    /* Encode source to UTF-8 */
    uint8_t utf8_buf[UTF8_BUF_SIZE];
    int utf8_len = cps_to_utf8(source, nsrc, utf8_buf, sizeof(utf8_buf));

    /* Normalize */
    uint8_t* new_end = nfc_normalize_utf8_incremental(d, utf8_buf, utf8_buf + utf8_len);
    int new_len = (int)(new_end - utf8_buf);

    /* Decode back */
    uint32_t result[MAX_CP_SEQ];
    int nresult = utf8_to_cps(utf8_buf, new_len, result, MAX_CP_SEQ);

    if (cps_equal(result, nresult, expected_nfc, nnfc)) {
      ++pass;
    } else {
      ++fail;
      if (fail <= MAX_FAIL_PRINT) {
        fprintf(stderr, "FAIL line %d: source=", line_num);
        print_cps(source, nsrc);
        fprintf(stderr, " expected=");
        print_cps(expected_nfc, nnfc);
        fprintf(stderr, " got=");
        print_cps(result, nresult);
        fprintf(stderr, "\n");
      }
    }

    while (p < end && *p != '\n') {
      ++p;
    }
    if (p < end) {
      ++p;
    }
  }

  lighter_map_close(&map);
  fprintf(stderr, "Results: %d pass, %d fail, %d skip\n", pass, fail, skip);
  return fail > 0 ? 1 : 0;
}
