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

echo ""
echo "Corpus ready in $CORPUS_DIR:"
ls -la "$CORPUS_DIR"
