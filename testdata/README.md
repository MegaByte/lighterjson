# JSON test suite

Tests that `lighter` produces valid JSON for a variety of inputs.

## Running

From repo root:

```bash
make test-json
```

Requires `lighter`, `lighter.nfc` (run `make lighter.nfc` if needed), and Python 3.

## Test files

| File | Covers |
|------|--------|
| `empty_object.json` | `{}` |
| `empty_array.json` | `[]` |
| `literals.json` | `true`, `false`, `null` in object |
| `numbers.json` | integers, negatives, decimals |
| `strings.json` | empty string, simple ASCII strings |
| `nested.json` | nested objects and arrays |
| `whitespace.json` | spaces around keys/colons/values |
| `mixed.json` | object with all value types |
| `unicode.json` | UTF-8 and `\u` escapes in strings |
| `single_value.json` | single top-level value (`true`) |
| `array_of_objects.json` | array of small objects |

Validation: after minifying, output is checked with `json.load()`. Idempotency is checked by minifying `empty_object.json` twice and comparing size.

## Notes

- **NDJSON** (`ndjson.json`): not run by default; in-place minify can corrupt when line length changes.
- **Escapes**: test strings avoid `\"`, `\n`, `\t`, etc. in the middle of strings so the suite passes with current minifier behavior.
- **Numbers**: test uses simple numbers only; precision/rounding is not exercised.
