/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Shares explicit SIMD architecture traits with Everett's kernels.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <simd/attributes.h>
#include <simd/vec.h>
#include <simd/integer.h>

#include <cstddef>
#include <type_traits>

namespace everett::backend_detail {
  // These are compile-time instruction profiles, not CPU feature probes.
  // Every architecture-independent wire representation keeps the same layout.
  template <simd::architecture Arch> inline constexpr std::size_t register_bytes =
    std::is_same_v<Arch, simd::avx512> ? 64 :
    std::is_same_v<Arch, simd::avx2> ? 32 :
    std::is_same_v<Arch, simd::neon> ? 16 : 1;
}
