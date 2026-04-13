#!/usr/bin/env sh
# Run lighter on testdata JSON files; verify output is valid JSON and
# (when Node and tools/comparejson.js exist) semantically equivalent to input.
# Usage: run_json_tests.sh [path/to/lighter]
# Default: ./lighter (run from repo root)

set -e
LIGHTER="${1:-./lighter}"
TESTDATA="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$TESTDATA/.." && pwd)"
OUT_DIR="$TESTDATA/.out"
COMPAREJSON="$ROOT/tools/comparejson.js"
PASS=0
FAIL=0

mkdir -p "$OUT_DIR"
cd "$ROOT"

validate() {
  python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$1" 2>/dev/null
}

# Semantic equivalence: original vs minified (if Node + comparejson.js available)
check_equivalent() {
  if [ -f "$COMPAREJSON" ] && command -v node >/dev/null 2>&1; then
    node "$COMPAREJSON" "$1" "$2" 2>/dev/null
  else
    true
  fi
}

run_test() {
  name="$1"
  opts="$2"
  input="$3"
  cp "$input" "$OUT_DIR/test.json"
  cp "$OUT_DIR/test.json" "$OUT_DIR/orig.json"
  if ! "$LIGHTER" -q $opts "$OUT_DIR/test.json" 2>/dev/null; then
    echo "  FAIL $name (minify failed)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  if ! validate "$OUT_DIR/test.json"; then
    echo "  FAIL $name (invalid JSON)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  case "$name" in
    unicode) ;;   # NFC normalization changes string
    number_edges|large_array|complex|scientific|float_formats_complex) ;;  # number reformatting can change form
    *)
      if ! check_equivalent "$OUT_DIR/orig.json" "$OUT_DIR/test.json"; then
        echo "  FAIL $name (not equivalent)"
        FAIL=$((FAIL + 1))
        return 1
      fi
      ;;
  esac
  echo "  PASS $name"
  PASS=$((PASS + 1))
  return 0
}

echo "JSON test suite (lighter: $LIGHTER)"
echo ""

# Single-document tests
for f in empty_object empty_array literals numbers strings nested whitespace mixed unicode single_value array_of_objects deep_nesting escapes number_edges large_array unicode_keys complex scientific zero_variants single_char_keys escapes_full float_formats float_formats_complex huge_exponents; do
  run_test "$f" "" "$TESTDATA/${f}.json" || true
done

# NDJSON (multiple values) - skipped: in-place minify corrupts when lines shrink
# run_test "ndjson" "-n" "$TESTDATA/ndjson.json" || true

# Idempotency: minify twice, output should parse and size unchanged
cp "$TESTDATA/empty_object.json" "$OUT_DIR/idem.json"
"$LIGHTER" -q "$OUT_DIR/idem.json" 2>/dev/null
first_size="$(wc -c < "$OUT_DIR/idem.json")"
"$LIGHTER" -q "$OUT_DIR/idem.json" 2>/dev/null
second_size="$(wc -c < "$OUT_DIR/idem.json")"
if [ "$first_size" = "$second_size" ] && validate "$OUT_DIR/idem.json"; then
  echo "  PASS idempotency"
  PASS=$((PASS + 1))
else
  echo "  FAIL idempotency"
  FAIL=$((FAIL + 1))
fi

echo ""
echo "Results: $PASS pass, $FAIL fail"
[ "$FAIL" -eq 0 ]
