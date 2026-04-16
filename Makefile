CC = cc
CFLAGS = -O3 -Wall

all: lighterjson

lighterjson: src/lighterjson.c src/lighter_bitfield.h src/lighter_common.h src/lighter_memmap.h src/lighter_number.h src/lighter_string.h src/unicode_nfc_shared.h src/unicode_nfc_runtime.h
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

ucd: ucd/UnicodeData.txt ucd/DerivedNormalizationProps.txt ucd/CompositionExclusions.txt ucd/NormalizationTest.txt

test_nfc: tools/test_nfc.c src/unicode_nfc_shared.h src/unicode_nfc_runtime.h src/unicode_nfc_builder.h
	$(CC) $(CFLAGS) -Isrc -o test_nfc tools/test_nfc.c

test: test_nfc lighter.nfc ucd
	./test_nfc lighter.nfc ucd

test-json: lighterjson lighter.nfc
	./testdata/run_json_tests.sh

test-all: test test-json

lighter.nfc: tools/gen_unicode_tables ucd
	./tools/gen_unicode_tables ucd > lighter.nfc

clean:
	rm -f lighterjson lighter.nfc tools/gen_unicode_tables test_nfc
	rm -rf ucd testdata/.out

.PHONY: all clean test test-json test-all ucd
