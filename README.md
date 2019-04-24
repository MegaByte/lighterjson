# lighterjson
Optimal JSON minifier

## Command line usage
    lighterjson [options] inputfile

## Options
    -p N Numeric precision (number of decimal places; can be negative)
    -q   Suppress output

## Notes
LighterJSON minifies JSON files in place. It removes all whitespace. It also converts all strings and numbers to their most compact representation.

If passed a directory, all .json files contained within will be processed recursively.

Numbers can be rounded to specific decimal places using the -p switch. Use negative numbers to represent places greater than ones.

JSON technically supports numbers of unlimited size, but due to implementation complexity, the supported exponent range is [-9223372036854775807, 9223372036854775807].

Files must be UTF-8. Not all cases of ill-formed files are currently handled. Make sure to backup before running.

It depends on standard POSIX headers, so it works best in POSIX-compliant operating systems. However, it can also be built for Windows by using a Cygwin-based toolchain.

## Author
Aaron Kaluszka <<megabyte@kontek.net>>
