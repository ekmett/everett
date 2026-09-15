/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>

namespace everett {
  enum class profile_unit : std::uint8_t { byte, bit };
  enum class stream_role : std::uint8_t { native, borrowed };

  struct variable_values {};
  template <std::uint64_t N> struct fixed_values {
    static constexpr std::uint64_t width = N;
  };

  namespace policy_detail {
    template <class V> struct value_traits;
    template <> struct value_traits<variable_values> {
      static constexpr bool fixed = false;
      static constexpr std::optional<std::uint64_t> width = std::nullopt;
    };
    template <std::uint64_t N> struct value_traits<fixed_values<N>> {
      static constexpr bool fixed = true;
      static constexpr std::optional<std::uint64_t> width = N;
    };
  }

  // N in fixed_values<N> is measured in this policy's units; zero is valid.
  // Associated worlds, sorts and streams retain this same policy type.
  template <profile_unit Unit, class Values = variable_values, std::uint64_t GroupSize = 15>
  struct storage_policy {
    static_assert(Unit == profile_unit::byte || Unit == profile_unit::bit);
    static_assert(GroupSize >= 3 && GroupSize != std::numeric_limits<std::uint64_t>::max() &&
                  std::has_single_bit(GroupSize + 1), "group size must be 2^n - 1 and at least three");
    using value_layout = Values;
    static constexpr std::uint64_t group_size = GroupSize;
    static constexpr unsigned class_bits = static_cast<unsigned>(std::bit_width(GroupSize));
    static constexpr profile_unit unit = Unit;
    static constexpr unsigned bits_per_unit = Unit == profile_unit::byte ? 8 : 1;
    static constexpr bool fixed_width = policy_detail::value_traits<Values>::fixed;
    static constexpr std::optional<std::uint64_t> value_width = policy_detail::value_traits<Values>::width;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's policy support.
 */
