/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's CRC32C support.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/backend.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace everett {
  // Reflected Castagnoli CRC with conventional initial/final complement.
  // Generated kernels live in the compiled library, outside consumer BMIs.
  std::uint32_t crc32c(std::span<std::byte const> bytes,
                       std::uint32_t previous_crc = 0) noexcept;
  namespace crc32c_detail {
    std::uint32_t run(simd::neon, std::span<std::byte const>, std::uint32_t) noexcept;
    std::uint32_t run(simd::avx2, std::span<std::byte const>, std::uint32_t) noexcept;
    std::uint32_t run(simd::avx512, std::span<std::byte const>, std::uint32_t) noexcept;
  }
  // Native policies link their matching compiled archive. The baseline
  // function remains safe to call before native feature admission.
  template <simd::architecture Arch>
  std::uint32_t crc32c(std::span<std::byte const> bytes, std::uint32_t previous_crc = 0) noexcept {
    if constexpr (std::same_as<Arch, simd::scalar>) return crc32c(bytes, previous_crc);
    else return crc32c_detail::run(Arch{}, bytes, previous_crc);
  }
  // Combine finalized checksums without rereading either input.
  std::uint32_t crc32c_combine(std::uint32_t first, std::uint32_t second,
                               std::uint64_t second_bytes) noexcept;
}
