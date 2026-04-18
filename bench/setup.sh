#!/usr/bin/env bash
# Download real-world JSON corpora for benchmarking.
# Usage: bench/setup.sh [CORPUS_DIR]  (default: bench/corpus)
set -eu

CORPUS_DIR="${1:-$(cd "$(dirname "$0")" && pwd)/corpus}"
mkdir -p "$CORPUS_DIR"

# serde-rs benchmark trio: twitter (mixed), canada (number-heavy), citm_catalog (object-heavy).
BASE_URL="https://raw.githubusercontent.com/serde-rs/json-benchmark/master/data"

download() {
  local name="$1"
  local url="$BASE_URL/$name"
  local dest="$CORPUS_DIR/$name"
  if [ -s "$dest" ]; then
    echo "[skip] $name (already present: $(wc -c < "$dest") bytes)"
    return
  fi
  echo "[get ] $name"
  if command -v curl >/dev/null 2>&1; then
    curl -sSL -o "$dest" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$dest" "$url"
  else
    echo "ERROR: need curl or wget" >&2
    exit 1
  fi
}

download twitter.json
download canada.json
download citm_catalog.json

# Replicate each to ~20x so runs are long enough to measure reliably on fast CPUs.
# For RISC-V hardware (often slower clocks), the base file may already be plenty.
MULT="${BENCH_MULT:-20}"
scale_up() {
  local name="$1"
  local src="$CORPUS_DIR/$name"
  local dest="$CORPUS_DIR/${name%.json}_big.json"
  if [ -s "$dest" ]; then
    echo "[skip] scaled $name (already present: $(wc -c < "$dest") bytes)"
    return
  fi
  echo "[mult] $name × $MULT"
  python3 - "$src" "$dest" "$MULT" <<'PY'
import json, sys
src, dest, mult = sys.argv[1], sys.argv[2], int(sys.argv[3])
data = json.load(open(src))
json.dump([data] * mult, open(dest, "w"))
PY
}

scale_up twitter.json
scale_up canada.json
scale_up citm_catalog.json

# Synthetic NFC stress corpus: real-world JSON rarely contains denormalized text,
# so the NFC normalization path almost never runs on the trio above. This file
# is composed of NFD-decomposed Latin/Vietnamese/Greek with combining marks
# separated from base letters, forcing both the non-ASCII prescan AND the
# heavy quick-check / canonical-decomposition / reordering paths.
nfc_stress="$CORPUS_DIR/nfc_stress_big.json"
if [ -s "$nfc_stress" ]; then
  echo "[skip] nfc_stress (already present: $(wc -c < "$nfc_stress") bytes)"
else
  echo "[gen ] nfc_stress (NFD-denormalized text)"
  python3 - "$nfc_stress" "${BENCH_NFC_RECORDS:-50000}" <<'PY'
import json, sys, unicodedata, random
random.seed(20260417)

# Source phrases mixing scripts that exercise different NFC code paths:
#   Latin + diacritics  (precomposed in NFC, decomposed in NFD)
#   Vietnamese          (multiple stacked combining marks)
#   Greek + tonos
#   Hangul              (algorithmic LVT decomposition)
phrases = [
    "café résumé naïve façade jalapeño Zoë",
    "Tiếng Việt rất đẹp và phong phú",
    "Ωμέγα Άλφα Βήτα Γάμμα Δέλτα",
    "한국어 문자열 정규화 테스트",
    "Bjørn Ärlich São Paulo Köln Zürich München",
    "français português español italiano deutsch",
    "Ḟ ḟ Ġ ġ Ḣ ḣ Ṁ ṁ Ṗ ṗ Ṡ ṡ Ṫ ṫ",  # Latin Extended Additional
]

# NFD decomposes precomposed chars into base + combining marks, which is the
# worst case for the parser (NFC quick-check returns NO, full normalization runs).
nfd_phrases = [unicodedata.normalize("NFD", p) for p in phrases]

dest, n = sys.argv[1], int(sys.argv[2])
records = []
for i in range(n):
    p = random.choice(nfd_phrases)
    records.append({
        "id": i,
        "text": p,
        "tag": "nfd-stress",
        "extra": p[: max(1, len(p) // 2)],
    })
# ensure_ascii=False writes raw NFD UTF-8 bytes; otherwise json.dump escapes
# them to \uXXXX, and lighterjson decodes \uXXXX into NFC during parsing
# (defeating the test).
with open(dest, "w", encoding="utf-8") as f:
    json.dump(records, f, ensure_ascii=False)
PY
fi

echo ""
echo "Corpus ready in $CORPUS_DIR:"
ls -la "$CORPUS_DIR"
