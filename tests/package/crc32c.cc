/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests CRC32C linkage across standalone consumer translation units.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/crc32c.h>

#include <cstddef>
#include <cstdint>
#include <span>

// A second translation unit checks that the installed implementation remains
// header-only without duplicate definitions or an undeclared link dependency.
std::uint32_t crc32c_from_other_translation_unit(std::span<std::byte const> bytes) {
  return diet::crc32c(bytes);
}
