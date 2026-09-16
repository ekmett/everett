/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Counts search operations in a separately compiled audit binary.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once
#include <array>
#include <cstdint>

namespace search_audit {
  enum operation { ef_select, rank, project, byte_frame, bit_frame, compare, materialize, count };
  inline bool enabled = false;
  inline std::array<std::uint64_t, count> calls{};
  inline void hit(operation op) { if (enabled) ++calls[op]; }
}
