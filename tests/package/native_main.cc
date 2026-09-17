// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <cstdint>

import everett;

#if defined(__AVX2__) || defined(__AVX512F__) || defined(__FMA__)
#error Native module linkage must not grant ISA flags to the baseline caller
#endif

int native_identity(everett::rank15_view const &);

int main() {
#if defined(EVERETT_PACKAGE_AVX2) || defined(EVERETT_PACKAGE_AVX512)
  __builtin_cpu_init();
  if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma") ||
      !__builtin_cpu_supports("bmi2")) return 77;
#if defined(EVERETT_PACKAGE_AVX512)
  if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512dq") ||
      !__builtin_cpu_supports("avx512bw") || !__builtin_cpu_supports("avx512vl")) return 77;
#endif
#endif
  std::array<std::uint8_t, 2> classes{7, 1};
  auto ranks = everett::rank15_index::build(classes, 16);
  return native_identity(ranks.view());
}
