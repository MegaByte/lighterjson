#ifndef LIGHTER_CPU_H
#define LIGHTER_CPU_H

#include <stdlib.h>
#include <string.h>

#include "lighter_common.h"

/* Architecture detection. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define LIGHTER_PLATFORM_X86 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
  #define LIGHTER_PLATFORM_ARM64 1
#endif

#if defined(__riscv) || defined(__riscv__)
  #define LIGHTER_PLATFORM_RISCV 1
#endif

/* Per-SIMD-site toggle for benchmarking. Each call site gets a function-local
 * static int cache; the comma-separated LIGHTERJSON_SIMD_DISABLE env var is
 * parsed once on first call. Names: "whitespace", "string", "nfc",
 * "significand", "exponent", or "all" to disable every site. Used to gate
 * AVX2/NEON/RVV paths uniformly so bench_simd.sh works on any architecture.
 *
 * The env-var-named variant LIGHTERJSON_RVV_DISABLE is also recognized for
 * back-compat with the old RVV-only bench harness. */
static inline int lighter_simd_parse_site(const char* name) {
  const char* env = getenv("LIGHTERJSON_SIMD_DISABLE");
  if (!env || !*env) {
    env = getenv("LIGHTERJSON_RVV_DISABLE");
    if (!env || !*env) {
      return 1;
    }
  }
  size_t name_len = strlen(name);
  const char* p = env;
  while (*p) {
    const char* comma = strchr(p, ',');
    size_t span = comma ? (size_t)(comma - p) : strlen(p);
    if ((span == 3 && memcmp(p, "all", 3) == 0) || (span == name_len && memcmp(p, name, name_len) == 0)) {
      return 0;
    }
    p += span + (comma ? 1 : 0);
  }
  return 1;
}

#define LIGHTER_SIMD_SITE_ENABLED(name_literal)                       \
  ({                                                                  \
    static int _lighter_simd_site_cache = -1;                         \
    if (_lighter_simd_site_cache < 0) {                               \
      _lighter_simd_site_cache = lighter_simd_parse_site(name_literal); \
    }                                                                 \
    _lighter_simd_site_cache;                                         \
  })

#if LIGHTER_PLATFORM_X86

  #if defined(_MSC_VER)
    #include <intrin.h>
    #define LIGHTER_TARGET_AVX2
  #elif defined(__GNUC__) || defined(__clang__)
    #include <immintrin.h>
    #define LIGHTER_TARGET_AVX2 __attribute__((target("avx2")))
  #else
    #define LIGHTER_TARGET_AVX2
  #endif

/** Return non-zero when AVX2 is available on the current x86 CPU. */
static inline int lighter_cpu_supports_avx2(void) {
  #if defined(_MSC_VER)
  int cpu_info[4];
  __cpuid(cpu_info, 0);
  if (cpu_info[0] >= 7) {
    __cpuidex(cpu_info, 7, 0);
    return (cpu_info[1] & (1 << 5)) != 0;
  }
  return 0;
  #elif defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2");
  #else
  return 0;
  #endif
}

#else

  #define LIGHTER_PLATFORM_X86 0
  #define LIGHTER_TARGET_AVX2
/** Return 0 when x86 runtime probing is unavailable on this build. */
static inline int lighter_cpu_supports_avx2(void) {
  return 0;
}

#endif /* LIGHTER_PLATFORM_X86 */

#if LIGHTER_PLATFORM_ARM64
  #if defined(__GNUC__) || defined(__clang__)
    #include <arm_neon.h>
  #endif
/** Return non-zero when NEON is available on the current ARM64 CPU. */
static inline int lighter_cpu_supports_neon(void) {
  return 1;
}
#else
/** Return 0 when ARM64 runtime probing is unavailable on this build. */
static inline int lighter_cpu_supports_neon(void) {
  return 0;
}
#endif

#if LIGHTER_PLATFORM_RISCV
  /* Make RVV intrinsics callable from individual functions without requiring the
   * whole TU to be compiled with -march=...v. The pragma temporarily enables V
   * so <riscv_vector.h> exposes its types/intrinsics; per-function attribute
   * (LIGHTER_TARGET_RVV) restricts codegen of the actual vector ops to those
   * functions, leaving the rest of the TU at the base ISA. */
  #if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
    #pragma GCC push_options
    #pragma GCC target("arch=+v")
    #include <riscv_vector.h>
    #pragma GCC pop_options
    #define LIGHTER_TARGET_RVV __attribute__((target("arch=+v")))
  #elif defined(__clang__) && __has_attribute(target)
    #pragma clang attribute push(__attribute__((target("v"))), apply_to = function)
    #include <riscv_vector.h>
    #pragma clang attribute pop
    #define LIGHTER_TARGET_RVV __attribute__((target("v")))
  #elif defined(__has_include) && __has_include(<riscv_vector.h>) && defined(__riscv_vector)
    /* Older toolchains: only usable when the whole TU is built with -march=...v. */
    #include <riscv_vector.h>
    #define LIGHTER_TARGET_RVV
  #else
    #define LIGHTER_TARGET_RVV
    #define LIGHTER_NO_RVV_INTRINSICS 1
  #endif

  #include <sys/syscall.h>
  #include <unistd.h>

  /* Define hwprobe constants if missing */
  #ifndef RISCV_HWPROBE_KEY_IMA_EXT_0
    #define RISCV_HWPROBE_KEY_IMA_EXT_0 4
  #endif
  #ifndef RISCV_HWPROBE_IMA_V
    #define RISCV_HWPROBE_IMA_V (1 << 2)
  #endif

struct lighter_riscv_hwprobe {
  int64_t key;
  uint64_t value;
};

  /* RVV site gate is the same as the generic SIMD gate; preserved for callers. */
  #define LIGHTER_RVV_SITE_ENABLED(name_literal) LIGHTER_SIMD_SITE_ENABLED(name_literal)

/** Return non-zero when RVV is available on the current RISC-V CPU. */
static inline int lighter_cpu_supports_rvv(void) {
  #if defined(__linux__) && defined(__NR_riscv_hwprobe) && !defined(LIGHTER_NO_RVV_INTRINSICS)
  struct lighter_riscv_hwprobe pair;
  pair.key = RISCV_HWPROBE_KEY_IMA_EXT_0;
  if (syscall(__NR_riscv_hwprobe, &pair, 1, 0, NULL, 0) == 0) {
    return (pair.value & RISCV_HWPROBE_IMA_V) != 0;
  }
  #endif
  return 0;
}
#else
  #define LIGHTER_TARGET_RVV
/** Return 0 when RISC-V runtime probing is unavailable on this build. */
static inline int lighter_cpu_supports_rvv(void) {
  return 0;
}
/** Stub: alias to the generic SIMD gate. */
  #define LIGHTER_RVV_SITE_ENABLED(name_literal) LIGHTER_SIMD_SITE_ENABLED(name_literal)
#endif

#endif /* LIGHTER_CPU_H */
