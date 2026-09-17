// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#pragma once

#include <everett/policy.h>

namespace everett_experiment {
#if defined(EVERETT_EXPERIMENT_NEON)
  using architecture = simd::neon;
#elif defined(EVERETT_EXPERIMENT_AVX2)
  using architecture = simd::avx2;
#elif defined(EVERETT_EXPERIMENT_AVX512)
  using architecture = simd::avx512;
#elif defined(EVERETT_EXPERIMENT_SCALAR)
  using architecture = simd::scalar;
#else
#error "Select an explicit experiment CPU profile through CMake"
#endif

  template <class Registry = everett::unsorted<std::optional<std::string>>,
            std::uint64_t GroupSize = 15, class BackspaceCode = everett::exponential_golomb<0>,
            std::uint64_t CodecBlockSize = GroupSize>
  using policy = everett::backend_policy<architecture, Registry, GroupSize, BackspaceCode, CodecBlockSize>;
  using string_policy = policy<everett::string_registry>;
}
