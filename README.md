# lighterjson

Optimal JSON minifier

## Command line usage

```
lighterjson [options] inputfile
```

## Options

```
-p N Numeric precision (number of decimal places; can be negative)
-n   Process NDJSON/JSON Lines
-N   Process NDJSON, preserving empty lines
-U   Disable Unicode normalization
-q   Suppress output
```

## Notes

LighterJSON minifies regular and newline-delimited JSON files in place. It removes all whitespace. It also converts all strings and numbers to their most compact representation. JSON strings are automatically normalized to Unicode Normalization Form C (NFC).

NFC normalization uses tables loaded from the file `lighter.nfc` in the current directory when needed. Generate it with the latest Unicode standard using `make lighter.nfc`

If passed a directory, all .json files contained within will be processed recursively. With NDJSON mode (`-n` or `-N`), .jsonl and .ndjson files are also processed.

Numbers can be rounded to specific decimal places using the -p switch. Use negative numbers to represent places greater than ones.

JSON technically supports numbers of unlimited size, but due to implementation complexity, the supported exponent range is [-9223372036854775807, 9223372036854775807].

Files must be UTF-8. Not all cases of ill-formed files are currently handled. Make sure to backup before running.

## Author
Aaron Kaluszka <<megabyte@kontek.net>>
