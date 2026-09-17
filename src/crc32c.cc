/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compiles the pinned CRC32C kernels behind Everett's module interface.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/crc32c.h>
#include <everett/detail/crc32c_impl.h>

namespace everett {
  // Combine finalized CRCs of A and B into CRC(A || B) without rereading either
  // input. second_bytes is B's physical byte length. The CRC of empty B is 0.
  // Lengths span all 64 bits; no bytes-to-bits multiplication can overflow.
  std::uint32_t crc32c_combine(std::uint32_t first, std::uint32_t second,
                                    std::uint64_t second_bytes) noexcept {
    return crc32c_detail::shift(first, second_bytes) ^ second;
  }

  // Reflected Castagnoli CRC32C, with the conventional initial/final complement.
  // Pinned Corsix-generated kernels are selected by the compiler target, without
  // runtime feature probes, allocation, or instructions beyond that target.
  // previous_crc is the finalized result of the preceding chunk, or zero for
  // a new stream. Empty chunks preserve it; callers do not invert the state.
  std::uint32_t crc32c(std::span<std::byte const> bytes,
                              std::uint32_t previous_crc) noexcept {
#include <everett/detail/crc32c_dispatch.inc>
  }
}
