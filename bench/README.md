# SIMD vectorization benchmarks

Per-site benchmarks for the SIMD code paths in lighterjson, covering AVX2
(x86_64), NEON (aarch64), and RVV (RISC-V). Each site can be toggled at
runtime via `LIGHTERJSON_SIMD_DISABLE` so you can measure each one's
contribution without recompiling.

## Sites

| Name          | Forms exercised                                        |
| ------------- | ------------------------------------------------------ |
| `whitespace`  | `skip_whitespace_{avx2,neon,rvv}`                      |
| `string`      | string-skip SIMD scan inside `lighter_do_string`       |
| `nfc`         | non-ASCII prescan inside `nfc_quick_check`             |
| `significand` | digit/delimiter scan in the number significand loop    |
| `exponent`    | digit scan in the number exponent loop                 |

Not every architecture has every site populated (e.g. RVV currently exposes
only `string` + `significand`; the rest fall through to scalar regardless of
the toggle). The bench script just shows ~0% delta for those.

## Environment variables

`LIGHTERJSON_SIMD_DISABLE` — comma-separated list of site names to disable.
Special value `all` disables every site. Unset or empty keeps runtime
detection as-is.

`LIGHTERJSON_RVV_DISABLE` — alias accepted for back-compat with the old
RVV-only bench harness; behaves identically to `LIGHTERJSON_SIMD_DISABLE`.

Examples:

```sh
# Run with scalar fallback for every SIMD site
LIGHTERJSON_SIMD_DISABLE=all ./lighterjson -q foo.json

# Measure how much the significand-loop vectorization saves on this workload
LIGHTERJSON_SIMD_DISABLE=significand ./lighterjson -q foo.json

# Disable everything except the whitespace scanner
LIGHTERJSON_SIMD_DISABLE=string,nfc,significand,exponent ./lighterjson -q foo.json
```

## Running the benchmark

```sh
# 1. Build lighterjson on the target hardware.
make

# 2. Download and prepare the corpus. Needs curl or wget and python3.
bench/setup.sh

# 3. Run the benchmark. Reports best-of-N wall time per configuration.
bench/bench_simd.sh
```

Tunables:

- `BENCH_RUNS=30 bench/bench_simd.sh` — increase replicates per configuration.
- `BENCH_MULT=50 bench/setup.sh` — increase corpus size multiplier if the
  runs are too short to measure reliably.

## Interpreting output

Each row reports the best (lowest) wall-time for that configuration and the
percentage delta vs. the all-enabled baseline.

- `baseline (all SIMD)` — every site enabled, same as running lighterjson normally.
- `all disabled` — scalar fallback throughout; the headline cost/benefit of SIMD.
- `no X` — X disabled, others enabled. Large positive % means X is **load-bearing**.
  Negative or near-zero means X is wasted dispatch overhead.
- `only X` — only X enabled, others disabled. Compare against the `all disabled`
  number: large negative means X is the dominant contributor on this workload.

A site that shows `no X` ≈ 0% **and** `only X` ≈ `all disabled` contributes
nothing measurable on this corpus and may be a candidate for removal.

## Corpus

Standard `serde-rs/json-benchmark` trio plus a synthetic NFC stress file:

- `twitter.json` — social-media-style records (mixed strings, ids, nested);
  ~15% non-ASCII (Japanese), but already in NFC, so the heavy normalization
  path is rarely taken
- `canada.json` — GeoJSON coordinate arrays (number-heavy, 0% non-ASCII)
- `citm_catalog.json` — event catalog with many short string keys
  (~70% whitespace from indentation)
- `nfc_stress.json` — generated locally; multilingual text (Latin diacritics,
  Vietnamese, Greek, Hangul) deliberately emitted in NFD form so combining
  marks are detached from base letters. Forces the non-ASCII prescan and
  the slow quick-check / canonical-decomposition / reordering paths.

Each is replicated 20× (by default) and saved as `*_big.json` so a single
run takes long enough to measure reliably. Tunables:
`BENCH_NFC_RECORDS=100000 bench/setup.sh` for the synthetic file size.

## Other benchmarks in this directory

- `bench_nfc.sh` — A/B comparison of NFC quick-check on/off. Measures
  whether the dedicated `nfc_quick_check` prescan still pays off vs.
  letting `nfc_normalize_utf8_incremental`'s built-in early-exit do the
  same job.
