/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Borrows KV03 payload and Elias-Fano sections for fully GPU-side input parsing.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include <everett/sort_profile_file.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace everett_gpu {
  // Shared with parse_input.hlsl. Bit offsets address the original payload,
  // not a reconstructed key/value arena. The source owner must outlive GPU use.
  struct compressed_descriptor {
    std::uint32_t literal_bit, key_bytes, value_bit, value_bits;
    std::uint32_t retained_key_bits, parent_record, source, reserved;
    bool operator==(compressed_descriptor const &) const = default;
  };
  static_assert(sizeof(compressed_descriptor) == 8 * sizeof(std::uint32_t));

  struct compressed_blocks {
    std::span<std::byte const> payload;
    // Low words, high words, select samples, sparse exceptions. These borrow
    // existing portable file bytes; no EF selection or section copy occurs.
    std::array<std::span<std::byte const>, 4> ef_sections;
    std::uint32_t count = 0, extent = 0, terminal_key_bits = 0, block_count = 0;
    std::uint32_t universe = 0, low_width = 0, fixed_value_bits = 0, high_bits = 0;
    explicit compressed_blocks(everett::sort_profile_view<everett::string_policy> view) {
      if (view.size() >= (1u << 24) || view.data().size() >= (1u << 31) || view.data().offset())
        throw std::length_error("GPU compressed input extent");
      auto terminal = view.metadata().terminal_key_units;
      if ((view.size() && (!terminal || terminal - 1 > (1u << 30) || ((terminal - 1) & 7))) ||
          (!view.size() && terminal))
        throw std::invalid_argument("GPU compressed input terminal key");
      // The bounded parser implements precisely code0 + FC string + optional
      // string, EG0 controls, and 15-record cuts. No per-block or per-record CPU work.
      if (view.size() && (view.dictionary_size() != 1 || view.dictionary().size() != 1 ||
          view.dictionary().at(0) || view.dictionary_offsets()[0] != 0 || view.dictionary_offsets()[1] != 1))
        throw std::invalid_argument("GPU compressed input selector dictionary");
      count = static_cast<std::uint32_t>(view.size());
      extent = static_cast<std::uint32_t>(view.data().size());
      terminal_key_bits = terminal ? static_cast<std::uint32_t>(terminal - 1) : 0;
      block_count = static_cast<std::uint32_t>(view.block_count());
      payload = view.data().storage();
      auto ef = view.group_offsets();
      auto high_extent = (ef.universe() >> ef.low_width()) + ef.size();
      auto fixed = view.metadata().common_value_width.value_or(0);
      if (view.size() && view.metadata().common_value_width && !fixed)
        throw std::invalid_argument("GPU compressed input zero optional value width");
      if (ef.universe() >= (1u << 31) || ef.low_width() > 30 ||
          fixed > std::numeric_limits<std::uint32_t>::max() ||
          high_extent > std::numeric_limits<std::uint32_t>::max() ||
          !ef.low_words().is_little_endian() || !ef.high_words().is_little_endian() ||
          !ef.samples().words().is_little_endian() || !ef.sparse_words().is_little_endian())
        throw std::length_error("GPU compressed input EF scalar extent");
      universe = static_cast<std::uint32_t>(ef.universe());
      low_width = ef.low_width();
      fixed_value_bits = static_cast<std::uint32_t>(fixed);
      high_bits = static_cast<std::uint32_t>(high_extent);
      ef_sections = {ef.low_words().bytes(), ef.high_words().bytes(),
        ef.samples().bytes(), ef.sparse_words().bytes()};
    }

    // The caller imports the one source mapping that contains payload and all
    // nonempty sections, and keeps its owner alive. Offsets address that buffer.
    // references5 receives this constant-size packet, never expanded offsets.
    std::array<std::uint32_t, 12> ef_parameters(std::uintptr_t mapped_base) const {
      std::array<std::uint32_t, 12> result{};
      for (std::size_t i = 0; i != ef_sections.size(); ++i) {
        auto section = ef_sections[i];
        if (section.empty()) continue;
        auto address = reinterpret_cast<std::uintptr_t>(section.data());
        if (address < mapped_base || address - mapped_base > std::numeric_limits<std::uint32_t>::max() ||
            section.size() > std::numeric_limits<std::uint32_t>::max() - (address - mapped_base))
          throw std::length_error("GPU compressed input EF buffer range");
        result[i * 2] = static_cast<std::uint32_t>(address - mapped_base);
        result[i * 2 + 1] = static_cast<std::uint32_t>(section.size() / (i == 2 ? 16 : 8));
      }
      result[8] = universe; result[9] = low_width;
      result[10] = fixed_value_bits; result[11] = high_bits;
      return result;
    }
    template <class Native> explicit compressed_blocks(Native const & input) : compressed_blocks(input.view()) {}
  };

  // Test oracle only. Never call this record walk from a timed GPU preparation
  // path. It does not reconstruct keys; independent CPU cursors can verify the
  // key bytes selected by the returned retention/literal descriptors.
  inline std::vector<compressed_descriptor> compressed_oracle(
      everett::sort_profile_view<everett::string_policy> view, std::uint32_t source) {
    compressed_blocks checked(view);
    std::vector<compressed_descriptor> result;
    result.reserve(checked.count);
    if (!checked.count) return result;
    auto frame = view.encoded_at(0);
    std::uint64_t previous = 0;
    for (std::uint32_t i = 0; i != checked.count; ++i) {
      if (!frame.key_units || ((frame.key_units - 1) & 7) || frame.key_units - 1 > (1u << 30) ||
          frame.value.size() > (1u << 30) || (!i && frame.retained) || frame.retained > previous)
        throw std::invalid_argument("GPU compressed oracle framing");
      auto retained = frame.retained ? frame.retained - 1 : 0;
      auto literal = frame.literal[1];
      if (retained + literal.size() != frame.key_units - 1 ||
          literal.storage().data() != view.data().storage().data() ||
          frame.value.storage().data() != view.data().storage().data())
        throw std::invalid_argument("GPU compressed oracle source spans");
      result.push_back({static_cast<std::uint32_t>(literal.offset()), static_cast<std::uint32_t>((frame.key_units - 1) >> 3),
        static_cast<std::uint32_t>(frame.value.offset()), static_cast<std::uint32_t>(frame.value.size()),
        static_cast<std::uint32_t>(retained), 0, source, 0});
      previous = frame.key_units;
      if (i + 1 != checked.count) frame = view.next(frame);
    }
    if (frame.next_offset != checked.extent || frame.key_units - 1 != checked.terminal_key_bits)
      throw std::invalid_argument("GPU compressed oracle terminal framing");
    return result;
  }
}
