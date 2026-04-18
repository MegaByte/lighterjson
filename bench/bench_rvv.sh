#!/usr/bin/env bash
# Benchmark each individual RVV vectorization site on the current host by
# toggling them one at a time via LIGHTERJSON_RVV_DISABLE.
#
# Usage: bench/bench_rvv.sh [LIGHTERJSON_BIN] [CORPUS_DIR]
#   defaults: ./lighterjson, bench/corpus
#
# Sites:
#   whitespace  — skip_whitespace_rvv            (lighterjson.c)
#   string      — lighter_simd_rvv_string_skip   (lighter_string.h)
#   nfc         — nfc_scan_high_rvv              (unicode_nfc_runtime.h)
#   significand — lighter_significand_chunk_rvv  (lighter_number.h)
#   exponent    — lighter_exponent_chunk_rvv     (lighter_number.h)
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIGHTER="${1:-$SCRIPT_DIR/../lighterjson}"
CORPUS_DIR="${2:-$SCRIPT_DIR/corpus}"
RUNS="${BENCH_RUNS:-15}"
WORK_FILE="${TMPDIR:-/tmp}/lj_bench_work.json"

if [ ! -x "$LIGHTER" ]; then
  echo "ERROR: $LIGHTER not executable. Build with 'make' first." >&2
  exit 1
fi

if [ ! -d "$CORPUS_DIR" ]; then
  echo "ERROR: corpus dir $CORPUS_DIR missing. Run bench/setup.sh first." >&2
  exit 1
fi

SITES=(whitespace string nfc significand exponent)

# Best-of-N microseconds for one (binary, env, input) combo.
bench_one() {
  local env_val="$1"
  local input="$2"
  local best=99999999
  local i
  for ((i = 1; i <= RUNS; i++)); do
    cp "$input" "$WORK_FILE"
    local t
    t=$(LIGHTERJSON_RVV_DISABLE="$env_val" python3 -c '
import os, subprocess, time
bin = os.environ["LIGHTER"]
work = os.environ["WORK_FILE"]
env = {**os.environ}
s = time.perf_counter()
subprocess.check_call([bin, "-q", work])
print(int((time.perf_counter() - s) * 1e6))
')
    if [ "$t" -lt "$best" ]; then
      best=$t
    fi
  done
  echo "$best"
}

export LIGHTER WORK_FILE

echo "lighterjson: $LIGHTER"
echo "corpus:      $CORPUS_DIR"
echo "runs/combo:  $RUNS (best-of)"
echo ""

INPUTS=()
for name in twitter_big.json canada_big.json citm_catalog_big.json nfc_stress_big.json; do
  if [ -s "$CORPUS_DIR/$name" ]; then
    INPUTS+=("$CORPUS_DIR/$name")
  fi
done
if [ ${#INPUTS[@]} -eq 0 ]; then
  echo "ERROR: no *_big.json files in $CORPUS_DIR. Run bench/setup.sh." >&2
  exit 1
fi

# Header
printf "%-22s" "configuration"
for input in "${INPUTS[@]}"; do
  printf " %12s" "$(basename "$input" _big.json)"
done
echo ""
printf "%-22s" "----------------------"
for _ in "${INPUTS[@]}"; do
  printf " %12s" "------------"
done
echo ""

# Baseline: everything enabled
printf "%-22s" "baseline (all RVV)"
baseline=()
for input in "${INPUTS[@]}"; do
  t=$(bench_one "" "$input")
  baseline+=("$t")
  printf " %9d us" "$t"
done
echo ""

# Everything disabled (scalar fallback)
printf "%-22s" "all disabled"
for i in "${!INPUTS[@]}"; do
  t=$(bench_one "all" "${INPUTS[$i]}")
  pct=$(awk -v a="$t" -v b="${baseline[$i]}" 'BEGIN{printf "%+.1f", (a-b)*100/b}')
  printf " %9d us (%s%%)" "$t" "$pct"
done
echo ""

# Each site disabled individually
for site in "${SITES[@]}"; do
  printf "%-22s" "no $site"
  for i in "${!INPUTS[@]}"; do
    t=$(bench_one "$site" "${INPUTS[$i]}")
    pct=$(awk -v a="$t" -v b="${baseline[$i]}" 'BEGIN{printf "%+.1f", (a-b)*100/b}')
    printf " %9d us (%s%%)" "$t" "$pct"
  done
  echo ""
done

# Each site disabled individually, complementary: only that site enabled
for site in "${SITES[@]}"; do
  # Disable all sites except the target: build the list excluding $site
  disable=""
  for s in "${SITES[@]}"; do
    if [ "$s" != "$site" ]; then
      disable="${disable:+$disable,}$s"
    fi
  done
  printf "%-22s" "only $site"
  for i in "${!INPUTS[@]}"; do
    t=$(bench_one "$disable" "${INPUTS[$i]}")
    pct=$(awk -v a="$t" -v b="${baseline[$i]}" 'BEGIN{printf "%+.1f", (a-b)*100/b}')
    printf " %9d us (%s%%)" "$t" "$pct"
  done
  echo ""
done

echo ""
echo "Positive % means the config is slower than baseline; negative means faster."
echo "'no X' rows show what X contributes (negative = removing X helps, i.e. X is harmful)."
echo "'only X' rows show X's contribution vs scalar (negative = X helps over scalar)."
