CC = cc
CFLAGS = -O3 -Wall

all: lighterjson lighter.nfc

lighterjson: src/lighterjson.c src/lighter_bitfield.h src/lighter_common.h src/lighter_cpu.h src/lighter_memmap.h src/lighter_number.h src/lighter_string.h src/lighter_transcode.h src/unicode_nfc_shared.h src/unicode_nfc_runtime.h
	$(CC) $(CFLAGS) -Isrc -o lighterjson src/lighterjson.c

tools/gen_unicode_tables: tools/gen_unicode_tables.c src/unicode_nfc_builder.h src/unicode_nfc_shared.h
	$(CC) $(CFLAGS) -Isrc -o tools/gen_unicode_tables tools/gen_unicode_tables.c

UCD_URL = https://www.unicode.org/Public/UCD/latest/ucd
ucd/UnicodeData.txt:
	mkdir -p ucd
	curl -s -o $@ $(UCD_URL)/UnicodeData.txt
ucd/DerivedNormalizationProps.txt:
	mkdir -p ucd
	curl -s -o $@ $(UCD_URL)/DerivedNormalizationProps.txt
ucd/CompositionExclusions.txt:
	mkdir -p ucd
	curl -s -o $@ $(UCD_URL)/CompositionExclusions.txt
ucd/NormalizationTest.txt:
	mkdir -p ucd
	curl -s -o $@ $(UCD_URL)/NormalizationTest.txt

UCD_FILES = ucd/UnicodeData.txt ucd/DerivedNormalizationProps.txt ucd/CompositionExclusions.txt ucd/NormalizationTest.txt
ucd: $(UCD_FILES)

test_nfc: tools/test_nfc.c src/unicode_nfc_shared.h src/unicode_nfc_runtime.h src/unicode_nfc_builder.h
	$(CC) $(CFLAGS) -Isrc -o test_nfc tools/test_nfc.c

test: test_nfc lighter.nfc $(UCD_FILES)
	./test_nfc lighter.nfc ucd

test-json: lighterjson lighter.nfc
	./testdata/run_json_tests.sh

test-all: test test-json

# Cross-platform correctness test via QEMU under Docker. Slow (apt + emulation)
# but catches RVV intrinsic regressions without RISC-V hardware.
test-riscv:
	docker run --rm -v "$(CURDIR)":/src -e QEMU_CPU=rv64,v=true,vlen=128,vext_spec=v1.0 \
	  --platform linux/riscv64 riscv64/ubuntu:25.04 bash -c \
	  'apt-get update >/dev/null && apt-get install -y -qq gcc make python3 curl >/dev/null && \
	   cd /src && make clean && make test-json'

# Same as test-riscv but with V disabled in the QEMU CPU model, to verify the
# binary still runs correctly on non-V hardware (no SIGILL from RVV ops).
test-riscv-novec:
	docker run --rm -v "$(CURDIR)":/src --platform linux/riscv64 \
	  riscv64/ubuntu:25.04 bash -c \
	  'apt-get update >/dev/null && apt-get install -y -qq gcc make python3 curl >/dev/null && \
	   cd /src && make clean && make test-json'

lighter.nfc: tools/gen_unicode_tables $(UCD_FILES)
	./tools/gen_unicode_tables ucd > lighter.nfc

clean:
	rm -f lighterjson lighter.nfc tools/gen_unicode_tables test_nfc
	rm -rf ucd testdata/.out

.PHONY: all clean test test-json test-all test-riscv test-riscv-novec ucd
