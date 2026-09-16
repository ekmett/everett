/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Borrows byte-profile native sections for bounded GPU parsing.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once
#include <everett/sections.h>
#include "../gpu_merge/gpu_sort.h"
#include "../gpu_merge/prepared_input.h"

namespace everett_byte_gpu {
  using policy = everett::storage_policy<>;
  using native = everett::mapped_native<policy>;
  using descriptor = everett_gpu::compressed_descriptor;

  // Explicit opt-in for this exact policy and composition only. The bit
  // experiment's gpu_registry contract remains a separate implementation.
  template <class P, class Compose> struct gpu_registry {
    static constexpr bool enabled = false;
  };
  template <> struct gpu_registry<policy, everett::replace_native_value> {
    static constexpr bool enabled = true;
    static constexpr everett_gpu::sort_contract contract{
      "everett.raw-byte-string-FC-LEB128/v1", "everett.byte-tag-optional-string/v2",
      "everett.unsigned-byte-lex-proper-prefix/v1", "everett.keep-newer-arrow/v1",
      "everett.u64-table-string-key/v1", "everett.optional-string-state-hash/v1",
      "everett.KV02-tagless-byte-K15-W15/v1", 1, true, false};
  };

  // The packet's abstract offset units are bytes. Descriptor offsets remain
  // bits, matching the existing prefix-owner and comparison shader ABI.
  struct compressed_blocks {
    std::span<std::byte const> payload;
    std::array<std::span<std::byte const>, 4> ef_sections;
    std::uint32_t count = 0, extent = 0, terminal_key_bits = 0, block_count = 0;
    std::uint32_t universe = 0, low_width = 0, fixed_value_bytes = 0, high_bits = 0;
    explicit compressed_blocks(native const & input) {
      auto view = input.view();
      auto const & metadata = view.metadata();
      auto ef = view.group_offsets();
      if (view.size() >= (1u << 24) || metadata.extent >= (1u << 28) ||
          metadata.terminal_key_units >= (1u << 27))
        throw std::length_error("byte GPU native extent");
      auto fixed = metadata.common_value_width.value_or(0);
      if ((view.size() && metadata.common_value_width && !fixed) || fixed >= (1u << 27))
        throw std::invalid_argument("byte GPU optional value width");
      auto high_extent = (ef.universe() >> ef.low_width()) + ef.size();
      if (ef.universe() >= (1u << 28) || ef.low_width() > 30 ||
          high_extent > std::numeric_limits<std::uint32_t>::max() ||
          !ef.low_words().is_little_endian() || !ef.high_words().is_little_endian() ||
          !ef.samples().words().is_little_endian() || !ef.sparse_words().is_little_endian())
        throw std::length_error("byte GPU EF extent or byte order");
      count = std::uint32_t(view.size());
      extent = std::uint32_t(metadata.extent);
      terminal_key_bits = std::uint32_t(metadata.terminal_key_units << 3);
      block_count = std::uint32_t(view.block_count());
      payload = view.bytes();
      universe = std::uint32_t(ef.universe());
      low_width = ef.low_width();
      fixed_value_bytes = std::uint32_t(fixed);
      high_bits = std::uint32_t(high_extent);
      ef_sections = {ef.low_words().bytes(), ef.high_words().bytes(),
        ef.samples().bytes(), ef.sparse_words().bytes()};
    }
    std::array<std::uint32_t, 12> ef_parameters(std::uintptr_t mapped_base) const {
      std::array<std::uint32_t, 12> result{};
      for (std::size_t i = 0; i != ef_sections.size(); ++i) {
        auto section = ef_sections[i];
        if (section.empty()) continue;
        auto address = reinterpret_cast<std::uintptr_t>(section.data());
        if (address < mapped_base || address - mapped_base > UINT32_MAX ||
            section.size() > UINT32_MAX - (address - mapped_base))
          throw std::length_error("byte GPU EF buffer range");
        result[i * 2] = std::uint32_t(address - mapped_base);
        result[i * 2 + 1] = std::uint32_t(section.size() / (i == 2 ? 16 : 8));
      }
      result[8] = universe; result[9] = low_width;
      result[10] = fixed_value_bytes; result[11] = high_bits;
      return result;
    }
  };
}
