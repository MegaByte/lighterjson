# RVV vectorization benchmarks

Per-site benchmarks for the RISC-V Vector (RVV) code paths in lighterjson.
Each of the five RVV intrinsic sites can be toggled independently at runtime
via `LIGHTERJSON_RVV_DISABLE`, so you can measure each one's contribution
without recompiling.

## RVV sites

| Name          | Function                         | File                     |
| ------------- | -------------------------------- | ------------------------ |
| `whitespace`  | `skip_whitespace_rvv`            | `src/lighterjson.c`      |
| `string`      | `lighter_simd_rvv_string_skip`   | `src/lighter_string.h`   |
| `nfc`         | `nfc_scan_high_rvv`              | `src/unicode_nfc_runtime.h` |
| `significand` | `lighter_significand_chunk_rvv`  | `src/lighter_number.h`   |
| `exponent`    | `lighter_exponent_chunk_rvv`     | `src/lighter_number.h`   |

## Environment variable

`LIGHTERJSON_RVV_DISABLE` — comma-separated list of site names to disable.
Special value `all` disables every site. Unset or empty keeps runtime
detection as-is.

Examples:

```sh
# Run with scalar fallback for every RVV site
LIGHTERJSON_RVV_DISABLE=all ./lighterjson -q foo.json

# Measure how much the significand-loop vectorization costs / saves
LIGHTERJSON_RVV_DISABLE=significand ./lighterjson -q foo.json

# Disable everything except the whitespace scanner
LIGHTERJSON_RVV_DISABLE=string,nfc,significand,exponent ./lighterjson -q foo.json
```

## Running the benchmark

```sh
# 1. Build lighterjson on the target RISC-V hardware (must have V extension).
make

# 2. Download and prepare the corpus. Needs curl or wget and python3.
bench/setup.sh

# 3. Run the benchmark. Reports best-of-N wall time per configuration.
bench/bench_rvv.sh
```

Tunables:

- `BENCH_RUNS=30 bench/bench_rvv.sh` — increase replicates per configuration.
- `BENCH_MULT=50 bench/setup.sh` — increase corpus size multiplier if the
  runs are too short to measure reliably.

## Interpreting output

Each row reports the best (lowest) wall-time for that configuration and the
percentage delta vs. the all-enabled baseline.

- `baseline (all RVV)` — every site enabled, same as running lighterjson normally.
- `all disabled` — scalar fallback throughout; the headline cost/benefit of RVV.
- `no X` — X disabled, others enabled. Negative % means X is **hurting**
  performance (disabling it sped things up).
- `only X` — only X enabled, others disabled. Negative % means X helps over
  pure scalar.

A site that shows `no X` ≈ 0% **and** `only X` ≈ `all disabled` contributes
nothing measurable on this corpus and may be a candidate for removal.

## Corpus

Standard `serde-rs/json-benchmark` trio:

- `twitter.json` — social-media-style records (mixed strings, ids, nested)
- `canada.json` — GeoJSON coordinate arrays (number-heavy)
- `citm_catalog.json` — event catalog with many short string keys

Each is replicated 20× (by default) and saved as `*_big.json` so a single
run takes long enough to measure reliably.
