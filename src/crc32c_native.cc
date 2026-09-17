// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <everett/crc32c.h>
#include <everett/detail/crc32c_impl.h>

namespace everett::crc32c_detail {
  std::uint32_t run(EVERETT_CRC_ARCH, std::span<std::byte const> bytes,
      std::uint32_t previous_crc) noexcept {
#include <everett/detail/crc32c_dispatch.inc>
  }
}
