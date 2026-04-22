CC = cc
CFLAGS = -O3 -Wall

# Probe a single flag against the compiler; echo it if accepted, nothing otherwise.
# Used to opportunistically add flags without breaking older toolchains.
probe_flag = $(shell $(CC) $(1) -E - < /dev/null > /dev/null 2>&1 && echo $(1))

# Link-time optimization. Apple clang prefers -flto=thin; gcc/clang accept -flto.
# Set LTO=0 to disable (e.g. for toolchains without the LTO plugin).
LTO ?= 1
ifeq ($(LTO),1)
  LTOFLAGS := $(shell \
    if $(CC) -flto=thin -E - < /dev/null > /dev/null 2>&1; then \
      echo -flto=thin; \
    elif $(CC) -flto -E - < /dev/null > /dev/null 2>&1; then \
      echo -flto; \
    fi)
else
  LTOFLAGS =
endif

# Opportunistic optimization flags — each is auto-detected and silently dropped
# if the toolchain doesn't accept it. Rationale per flag:
#   -fvisibility=hidden:   lets LTO drop more unused symbols.
#   -fno-stack-protector:  lighterjson parses defensively; canaries are pure cost.
#   -fno-plt:              skips PLT thunks for shared-lib calls (ELF only).
#                          Shaves ~128 bytes on GCC/Linux. Skipped on Mach-O
#                          (Apple clang) where it bloats the binary by ~16KB
#                          for no benefit (Mach-O lazy binding works differently).
#
# Notes on flags evaluated and excluded:
#   -funroll-loops: clang at -O3 already auto-unrolls; the flag is a no-op.
#     GCC honors it and bloats the binary by ~50% (49KB → 100KB on this
#     codebase) without measurable speedup, sometimes a small regression.
IS_ELF := $(shell echo | $(CC) -E -dM - 2>/dev/null | grep -q '__ELF__' && echo 1)
OPTFLAGS := \
  $(call probe_flag,-fvisibility=hidden) \
  $(call probe_flag,-fno-stack-protector) \
  $(if $(IS_ELF),$(call probe_flag,-fno-plt))

# NATIVE=1 enables -march=native / -mcpu=native. The resulting binary is
# tied to the build host's exact ISA; not safe for distribution. Off by default.
NATIVE ?= 0
ifeq ($(NATIVE),1)
  NATIVEFLAGS := $(shell \
    if $(CC) -march=native -E - < /dev/null > /dev/null 2>&1; then \
      echo -march=native; \
    elif $(CC) -mcpu=native -E - < /dev/null > /dev/null 2>&1; then \
      echo -mcpu=native; \
    fi)
else
  NATIVEFLAGS =
endif

# OpenMP parallelizes batch processing of files in directory mode
# (OMP_NUM_THREADS controls parallelism). Auto-detect by probing the compiler;
# override with OMPFLAGS= to disable or OMPFLAGS="-fopenmp ..." to force.
# Apple clang needs explicit libomp paths (brew install libomp).
OMPFLAGS := $(shell \
  if $(CC) -fopenmp -dM -E - < /dev/null > /dev/null 2>&1; then \
    echo -fopenmp; \
  elif [ -f /opt/homebrew/opt/libomp/include/omp.h ] && \
       $(CC) -Xpreprocessor -fopenmp -I/opt/homebrew/opt/libomp/include -L/opt/homebrew/opt/libomp/lib -lomp -dM -E - < /dev/null > /dev/null 2>&1; then \
    echo "-Xpreprocessor -fopenmp -I/opt/homebrew/opt/libomp/include -L/opt/homebrew/opt/libomp/lib -lomp"; \
  fi)

# RELEASE=1 strips symbols and runs `strip` after link for the smallest binary.
# Default builds keep symbols for easier debugging.
RELEASE ?= 0
ifeq ($(RELEASE),1)
  LDFLAGS_RELEASE = -Wl,-S -Wl,-x
  STRIP_CMD = strip -x
else
  LDFLAGS_RELEASE =
  STRIP_CMD = :
endif

all: lighterjson lighter.nfc

lighterjson: src/lighterjson.c src/lighter_bitfield.h src/lighter_common.h src/lighter_cpu.h src/lighter_memmap.h src/lighter_number.h src/lighter_string.h src/lighter_transcode.h src/unicode_nfc_shared.h src/unicode_nfc_runtime.h
	$(CC) $(CFLAGS) $(LTOFLAGS) $(OPTFLAGS) $(NATIVEFLAGS) $(OMPFLAGS) $(LDFLAGS_RELEASE) -Isrc -o lighterjson src/lighterjson.c
	@$(STRIP_CMD) lighterjson 2>/dev/null || true

tools/gen_unicode_tables: tools/gen_unicode_tables.c src/unicode_nfc_builder.h src/unicode_nfc_shared.h
	$(CC) $(CFLAGS) $(LTOFLAGS) $(OPTFLAGS) $(NATIVEFLAGS) -Isrc -o tools/gen_unicode_tables tools/gen_unicode_tables.c

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
	$(CC) $(CFLAGS) $(LTOFLAGS) $(OPTFLAGS) $(NATIVEFLAGS) -Isrc -o test_nfc tools/test_nfc.c

test: test_nfc lighter.nfc $(UCD_FILES)
	./test_nfc lighter.nfc ucd

test-json: lighterjson lighter.nfc
	./testdata/run_json_tests.sh

test-all: test test-json

lighter.nfc: tools/gen_unicode_tables $(UCD_FILES)
	./tools/gen_unicode_tables ucd > lighter.nfc

clean:
	rm -f lighterjson lighter.nfc tools/gen_unicode_tables test_nfc
	rm -rf ucd testdata/.out

.PHONY: all clean test test-json test-all ucd
