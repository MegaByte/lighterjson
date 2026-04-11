#ifndef LIGHTER_CPU_H
#define LIGHTER_CPU_H

#include "lighter_common.h"

/* Architecture Detection */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define LIGHTER_PLATFORM_X86 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define LIGHTER_PLATFORM_ARM64 1
#endif

#if defined(__riscv) || defined(__riscv__)
#define LIGHTER_PLATFORM_RISCV 1
#endif

#if LIGHTER_PLATFORM_X86

#if defined(_MSC_VER)
  #include <intrin.h>
  #define LIGHTER_TARGET_AVX2 
  #define LIGHTER_TARGET_AVX512 
#elif defined(__GNUC__) || defined(__clang__)
  #include <immintrin.h>
  #define LIGHTER_TARGET_AVX2 __attribute__((target("avx2")))
  #define LIGHTER_TARGET_AVX512 __attribute__((target("avx512bw")))
#else
  #define LIGHTER_TARGET_AVX2
  #define LIGHTER_TARGET_AVX512
#endif

static inline int lighter_cpu_supports_avx2(void) {
#if defined(_MSC_VER)
  int cpuInfo[4];
  __cpuid(cpuInfo, 0);
  if (cpuInfo[0] >= 7) {
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 5)) != 0;
  }
  return 0;
#elif defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2");
#else
  return 0;
#endif
}

static inline int lighter_cpu_supports_avx512bw(void) {
#if defined(_MSC_VER)
  int cpuInfo[4];
  __cpuid(cpuInfo, 0);
  if (cpuInfo[0] >= 7) {
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 30)) != 0; /* bit 30 in EBX for AVX512BW */
  }
  return 0;
#elif defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx512bw");
#else
  return 0;
#endif
}

#else

#define LIGHTER_PLATFORM_X86 0
#define LIGHTER_TARGET_AVX2
#define LIGHTER_TARGET_AVX512
static inline int lighter_cpu_supports_avx2(void) { return 0; }
static inline int lighter_cpu_supports_avx512bw(void) { return 0; }

#endif /* LIGHTER_PLATFORM_X86 */

#if LIGHTER_PLATFORM_ARM64
  #if defined(__GNUC__) || defined(__clang__)
    #include <arm_neon.h>
  #endif
  static inline int lighter_cpu_supports_neon(void) { return 1; }
#else
  static inline int lighter_cpu_supports_neon(void) { return 0; }
#endif

#if LIGHTER_PLATFORM_RISCV
  #if defined(__has_include)
    #if __has_include(<riscv_vector.h>)
      #include <riscv_vector.h>
    #endif
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

  static inline int lighter_cpu_supports_rvv(void) {
#if defined(__linux__) && defined(__NR_riscv_hwprobe)
    struct lighter_riscv_hwprobe pair;
    pair.key = RISCV_HWPROBE_KEY_IMA_EXT_0;
    if (syscall(__NR_riscv_hwprobe, &pair, 1, 0, NULL, 0) == 0) {
      return (pair.value & RISCV_HWPROBE_IMA_V) != 0;
    }
#endif
    return 0;
  }
#else
  static inline int lighter_cpu_supports_rvv(void) { return 0; }
#endif

#endif /* LIGHTER_CPU_H */
