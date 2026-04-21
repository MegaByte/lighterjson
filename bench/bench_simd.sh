#!/usr/bin/env bash
# Benchmark each individual SIMD vectorization site on the current host by
# toggling them one at a time via LIGHTERJSON_SIMD_DISABLE.
#
# Works on x86_64 (AVX2), aarch64 (NEON), and RISC-V (RVV) — the gates compile
# uniformly on every architecture; the script reports whichever sites the
# current binary actually exercises.
#
# Usage: bench/bench_simd.sh [LIGHTERJSON_BIN] [CORPUS_DIR]
#   defaults: ./lighterjson, bench/corpus
#
# Sites:
#   whitespace  — skip_whitespace_{avx2,neon,rvv}
#   string      — string-skip SIMD scan inside lighter_do_string
#   nfc         — non-ASCII prescan inside nfc_quick_check
#   significand — vectorized digit/delimiter scan in number significand loop
#   exponent    — vectorized digit scan in number exponent loop
#
# Note: not every arch has every site populated (e.g. RVV currently exposes
# only string + significand; the rest fall through to scalar regardless of
# the toggle). That's expected — the script just shows zero delta for those.
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

# Detect ISA label (purely cosmetic, for the header line).
ISA="$(uname -m 2>/dev/null || echo unknown)"
case "$ISA" in
  x86_64|amd64) ISA_LABEL="AVX2" ;;
  aarch64|arm64) ISA_LABEL="NEON" ;;
  riscv64) ISA_LABEL="RVV" ;;
  *) ISA_LABEL="$ISA" ;;
esac

# Best-of-N microseconds for one (binary, env, input) combo.
bench_one() {
  local env_val="$1"
  local input="$2"
  local best=99999999
  local i
  for ((i = 1; i <= RUNS; i++)); do
    cp "$input" "$WORK_FILE"
    local t
    t=$(LIGHTERJSON_SIMD_DISABLE="$env_val" python3 -c '
import os, subprocess, time
bin = os.environ["LIGHTER"]
work = os.environ["WORK_FILE"]
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
echo "ISA:         $ISA ($ISA_LABEL)"
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
  printf " %16s" "$(basename "$input" _big.json)"
done
echo ""
printf "%-22s" "----------------------"
for _ in "${INPUTS[@]}"; do
  printf " %16s" "----------------"
done
echo ""

# Baseline: everything enabled
printf "%-22s" "baseline (all SIMD)"
baseline=()
for input in "${INPUTS[@]}"; do
  t=$(bench_one "" "$input")
  baseline+=("$t")
  printf " %13d us" "$t"
done
echo ""

# Everything disabled (scalar fallback)
printf "%-22s" "all disabled"
for i in "${!INPUTS[@]}"; do
  t=$(bench_one "all" "${INPUTS[$i]}")
  pct=$(awk -v a="$t" -v b="${baseline[$i]}" 'BEGIN{printf "%+.1f", (a-b)*100/b}')
  printf " %9d (%5s%%)" "$t" "$pct"
done
echo ""

# Each site disabled individually
for site in "${SITES[@]}"; do
  printf "%-22s" "no $site"
  for i in "${!INPUTS[@]}"; do
    t=$(bench_one "$site" "${INPUTS[$i]}")
    pct=$(awk -v a="$t" -v b="${baseline[$i]}" 'BEGIN{printf "%+.1f", (a-b)*100/b}')
    printf " %9d (%5s%%)" "$t" "$pct"
  done
  echo ""
done

# Each site only (complementary: disable everything else)
for site in "${SITES[@]}"; do
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
    printf " %9d (%5s%%)" "$t" "$pct"
  done
  echo ""
done

echo ""
echo "Positive % means the config is slower than baseline; negative means faster."
echo "'no X' rows show what X contributes (large positive = X is load-bearing)."
echo "'only X' rows show X's contribution vs pure scalar (close to 0 = no win)."
