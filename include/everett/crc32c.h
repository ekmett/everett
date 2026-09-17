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

#include <cstddef>
#include <cstdint>
#include <span>

namespace everett {
  // Reflected Castagnoli CRC with conventional initial/final complement.
  // Generated kernels live in the compiled library, outside consumer BMIs.
  std::uint32_t crc32c(std::span<std::byte const> bytes,
                       std::uint32_t previous_crc = 0) noexcept;
  // Combine finalized checksums without rereading either input.
  std::uint32_t crc32c_combine(std::uint32_t first, std::uint32_t second,
                               std::uint64_t second_bytes) noexcept;
}
