#!/usr/bin/env sh
# Run lighterjson on testdata JSON files; verify output is valid JSON and
# (when Node and tools/comparejson.js exist) semantically equivalent to input.
# Usage: run_json_tests.sh [path/to/lighterjson]
# Default: ./lighterjson (run from repo root)

set -e
LIGHTER="${1:-./lighterjson}"
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
    number_edges|large_array|complex|scientific|float_formats_complex|huge_exponents|number_broad|stress) ;;  # number reformatting can change form
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

echo "JSON test suite (lighterjson: $LIGHTER)"
echo ""

# Single-document tests
for f in empty_object empty_array literals numbers strings nested whitespace mixed unicode single_value array_of_objects deep_nesting escapes number_edges large_array unicode_keys complex scientific zero_variants single_char_keys escapes_full float_formats float_formats_complex huge_exponents number_broad stress; do
  run_test "$f" "" "$TESTDATA/${f}.json" || true
done

# Deep semantic check for numeric outputs (catches wrong values even when form differs)
deep_numeric_check() {
  name="$1"
  input="$2"
  cp "$input" "$OUT_DIR/test.json"
  if ! "$LIGHTER" -q "$OUT_DIR/test.json" 2>/dev/null; then
    echo "  FAIL deep-$name (minify failed)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  if ! python3 - "$input" "$OUT_DIR/test.json" << 'PYEOF'
import json, sys, math
orig = json.load(open(sys.argv[1]))
got = json.load(open(sys.argv[2]))
def flat(x, out):
    if isinstance(x, list):
        for v in x: flat(v, out)
    elif isinstance(x, dict):
        for v in x.values(): flat(v, out)
    elif isinstance(x, (int, float)):
        out.append(float(x))
a, b = [], []
flat(orig, a); flat(got, b)
if len(a) != len(b): sys.exit(1)
for x, y in zip(a, b):
    if x != y and not (math.isnan(x) and math.isnan(y)):
        sys.exit(1)
sys.exit(0)
PYEOF
  then
    echo "  FAIL deep-$name (numeric values differ)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  echo "  PASS deep-$name"
  PASS=$((PASS + 1))
  return 0
}
deep_numeric_check "numbers" "$TESTDATA/numbers.json" || true
deep_numeric_check "number_broad" "$TESTDATA/number_broad.json" || true
deep_numeric_check "scientific" "$TESTDATA/scientific.json" || true
deep_numeric_check "float_formats" "$TESTDATA/float_formats.json" || true
deep_numeric_check "float_formats_complex" "$TESTDATA/float_formats_complex.json" || true
deep_numeric_check "zero_variants" "$TESTDATA/zero_variants.json" || true
deep_numeric_check "stress" "$TESTDATA/stress.json" || true

# Structural semantic check: string, object, array content should survive unchanged
deep_structural_check() {
  name="$1"
  input="$2"
  cp "$input" "$OUT_DIR/test.json"
  if ! "$LIGHTER" -q "$OUT_DIR/test.json" 2>/dev/null; then
    echo "  FAIL struct-$name (minify failed)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  if ! python3 - "$input" "$OUT_DIR/test.json" << 'PYEOF'
import json, sys
orig = json.load(open(sys.argv[1]))
got = json.load(open(sys.argv[2]))
def norm(x):
    if isinstance(x, dict):
        return {k: norm(v) for k, v in x.items()}
    if isinstance(x, list):
        return [norm(v) for v in x]
    if isinstance(x, float):
        return float(x)
    if isinstance(x, int):
        return float(x)
    return x
if norm(orig) != norm(got):
    sys.exit(1)
sys.exit(0)
PYEOF
  then
    echo "  FAIL struct-$name (structure/values differ)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  echo "  PASS struct-$name"
  PASS=$((PASS + 1))
  return 0
}
deep_structural_check "stress" "$TESTDATA/stress.json" || true
deep_structural_check "complex" "$TESTDATA/complex.json" || true
deep_structural_check "nested" "$TESTDATA/nested.json" || true
deep_structural_check "escapes_full" "$TESTDATA/escapes_full.json" || true

# Encoding roundtrip: write UTF-16LE/BE/UTF-32LE/BE with BOM, minify, read back.
encoding_roundtrip() {
  enc="$1"
  bom="$2"
  python3 - "$OUT_DIR/enc_${enc}.json" "$enc" "$bom" "$TESTDATA/stress.json" << 'PYEOF'
import json, sys
out_path, enc, bom_hex, src = sys.argv[1:5]
data = json.load(open(src))
bom = bytes.fromhex(bom_hex)
with open(out_path, "wb") as f:
    f.write(bom)
    f.write(json.dumps(data).encode(enc))
PYEOF
  if ! "$LIGHTER" -q "$OUT_DIR/enc_${enc}.json" 2>/dev/null; then
    echo "  FAIL enc-$enc (minify failed)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  if ! python3 - "$OUT_DIR/enc_${enc}.json" "$enc" "$TESTDATA/stress.json" << 'PYEOF'
import json, sys, math
path, enc, src = sys.argv[1:4]
raw = open(path, "rb").read()
# try with and without BOM
for offset in (0, 2, 4):
    try:
        decoded = raw[offset:].decode(enc)
        got = json.loads(decoded)
        break
    except Exception:
        continue
else:
    sys.exit(1)
orig = json.load(open(src))
def norm(x):
    if isinstance(x, dict): return {k:norm(v) for k,v in x.items()}
    if isinstance(x, list): return [norm(v) for v in x]
    if isinstance(x, (int,float)): return float(x)
    return x
if norm(orig) != norm(got):
    sys.exit(1)
PYEOF
  then
    echo "  FAIL enc-$enc (roundtrip mismatch)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  echo "  PASS enc-$enc"
  PASS=$((PASS + 1))
}

if command -v python3 >/dev/null 2>&1; then
  encoding_roundtrip "utf-16-le" "fffe" || true
  encoding_roundtrip "utf-16-be" "feff" || true
  encoding_roundtrip "utf-32-le" "fffe0000" || true
  encoding_roundtrip "utf-32-be" "0000feff" || true
  encoding_roundtrip "utf-8-sig" "efbbbf" || true
fi

# NDJSON: validate per-record after splitting on '\n'.
ndjson_check() {
  name="$1"
  opts="$2"
  src="$3"
  expected_records="$4"
  expected_blank_lines="$5"
  cp "$src" "$OUT_DIR/${name}.json"
  if ! "$LIGHTER" -q $opts "$OUT_DIR/${name}.json" 2>/dev/null; then
    echo "  FAIL $name (minify failed)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  if ! python3 - "$OUT_DIR/${name}.json" "$src" "$expected_records" "$expected_blank_lines" << 'PYEOF'
import json, sys
out_path, src_path, exp_records, exp_blank = sys.argv[1:5]
exp_records = int(exp_records)
exp_blank = int(exp_blank)
with open(out_path) as f:
    text = f.read()
with open(src_path) as f:
    orig_text = f.read()
# Split on '\n'; expected_records non-empty lines, expected_blank empty lines.
lines = text.split("\n")
# A trailing newline produces a final empty entry that is not a separate line.
if lines and lines[-1] == "":
    lines = lines[:-1]
records = [l for l in lines if l]
blanks = [l for l in lines if not l]
if len(records) != exp_records:
    print(f"record count {len(records)} != {exp_records}", file=sys.stderr)
    sys.exit(1)
if len(blanks) != exp_blank:
    print(f"blank-line count {len(blanks)} != {exp_blank}", file=sys.stderr)
    sys.exit(1)
# Each non-blank line must parse as JSON and equal the corresponding source record.
orig_lines = [l for l in orig_text.split("\n") if l.strip()]
def norm(x):
    if isinstance(x, dict): return {k:norm(v) for k,v in x.items()}
    if isinstance(x, list): return [norm(v) for v in x]
    if isinstance(x, (int,float)): return float(x)
    return x
if len(orig_lines) != len(records):
    sys.exit(1)
for o, g in zip(orig_lines, records):
    if norm(json.loads(o)) != norm(json.loads(g)):
        sys.exit(1)
sys.exit(0)
PYEOF
  then
    echo "  FAIL $name (NDJSON content mismatch)"
    FAIL=$((FAIL + 1))
    return 1
  fi
  echo "  PASS $name"
  PASS=$((PASS + 1))
}

if command -v python3 >/dev/null 2>&1; then
  ndjson_check "ndjson-n" "-n" "$TESTDATA/ndjson.json" 3 0 || true
  ndjson_check "ndjson-N" "-N" "$TESTDATA/ndjson.json" 3 0 || true
  # Blank-line preservation: -N keeps them, -n collapses them.
  printf '{"a":1}\n\n{"b":2}\n\n\n{"c":3}\n' > "$OUT_DIR/ndjson_blanks.in"
  ndjson_check "ndjson-n-blanks-collapsed" "-n" "$OUT_DIR/ndjson_blanks.in" 3 0 || true
  ndjson_check "ndjson-N-blanks-preserved" "-N" "$OUT_DIR/ndjson_blanks.in" 3 3 || true
  # Whitespace inside records gets stripped, separators preserved.
  printf '{ "a" : 1 }\n{ "b" : 2 }\n' > "$OUT_DIR/ndjson_ws.in"
  ndjson_check "ndjson-n-strip-inner-ws" "-n" "$OUT_DIR/ndjson_ws.in" 2 0 || true
fi

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
