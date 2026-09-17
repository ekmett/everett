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


#include <cstddef>
#include <cstdint>
#include <span>

import everett;

// A second importer checks the installed archive and common entity identity.
std::uint32_t crc32c_from_other_translation_unit(std::span<std::byte const> bytes) {
  return everett::crc32c(bytes);
}
