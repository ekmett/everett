/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares the optional shader experiment's explicit sort capability.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include <everett/sort_profile_merge.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace everett_gpu {
// This descriptor selects an audited shader implementation; it does not
// compile an arbitrary C++ codec, selector or composition callback to HLSL.
// The ordinary CPU path remains the fallback when either trait is disabled.
struct sort_contract {
  std::string_view key_grammar, value_grammar, key_order, composition;
  std::string_view key_hash, value_hash, shader_family;
  std::uint32_t descriptor_abi;
  bool retain_tombstones, gpu_hash;
};

template <class Sort, class Compose> struct gpu_sort {
  static constexpr bool enabled = false;
};

template <>
struct gpu_sort<everett::unsorted<std::optional<std::string>>, everett::replace_native_value> {
  static constexpr bool enabled = true;
  static constexpr sort_contract contract{"everett.fc-byte-string-eg0/v1",
                                          "everett.optional-byte-string-eg0/v1",
                                          "everett.unsigned-byte-lex-proper-prefix/v1",
                                          "everett.keep-newer-arrow/v1",
                                          "everett.u64-table-string-key/v1",
                                          "everett.optional-string-state-hash/v1",
                                          "everett.kv03-string-replacement/v1",
                                          1,
                                          true,
                                          false};
  // Hash identifiers name the host semantic domain. No shader currently
  // computes the logical content signature; encoded arrows are preserved.
  using codec = everett::sort_codec<everett::unsorted<std::optional<std::string>>>;
  static_assert(std::is_same_v<typename codec::key_codec, everett::fc_string_key<>>);
  static_assert(
      std::is_same_v<typename codec::value_codec, everett::tombstone_value<everett::string_value<>>>);
};

// Registry eligibility additionally binds the actual selector protocol and
// physical policy. A familiar sort alone must not enable a different code tree,
// count codec, unit, K/W, custom selector or composition wrapper accidentally.
template <class Policy, class Selector, class Compose> struct gpu_registry {
  static constexpr bool enabled = false;
};

template <>
struct gpu_registry<everett::string_policy, everett::registry_selector<everett::string_registry>,
                    everett::replace_native_value>
    : gpu_sort<everett::unsorted<std::optional<std::string>>, everett::replace_native_value> {
  static constexpr std::string_view selector_id = "everett.string-registry-code0/v1";
  static constexpr std::string_view physical_id = "everett.KV03-bit-EG0-K15-W15/v1";
  static constexpr std::uint32_t selector_bits = 1, selector_code = 0;
  static constexpr std::uint32_t group_size = 15, codec_block_size = 15;
};
} // namespace everett_gpu
