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
#include <type_traits>

namespace diet {
  enum class profile_unit : std::uint8_t { byte, bit };
  enum class stream_role : std::uint8_t { native, borrowed };

  enum class bit_backspace_code : std::uint8_t { exponential_golomb = 0, golomb = 1 };
  template <std::uint64_t Order = 0> struct exponential_golomb {
    static_assert(Order <= 63, "exponential-Golomb order must be at most 63");
  };
  template <std::uint64_t M> struct golomb {
    static_assert(M != 0, "Golomb modulus must be positive");
  };

  struct variable_values {};
  template <std::uint64_t N> struct fixed_values {
    static constexpr std::uint64_t width = N;
  };

  namespace policy_detail {
    template <class Code> struct backspace_traits;
    template <std::uint64_t Order> struct backspace_traits<exponential_golomb<Order>> {
      static_assert(Order <= 63, "exponential-Golomb order must be at most 63");
      static constexpr bit_backspace_code code = bit_backspace_code::exponential_golomb;
      static constexpr std::uint64_t parameter = Order;
    };
    template <std::uint64_t M> struct backspace_traits<golomb<M>> {
      static_assert(M != 0, "Golomb modulus must be positive");
      static constexpr bit_backspace_code code = bit_backspace_code::golomb;
      static constexpr std::uint64_t parameter = M;
    };
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
  // Associated colas, sorts and streams retain this same policy type.
  template <profile_unit Unit, class Values = variable_values, std::uint64_t GroupSize = 15,
            class BackspaceCode = exponential_golomb<0>, std::uint64_t CodecBlockSize = GroupSize>
  struct storage_policy {
    static_assert(Unit == profile_unit::byte || Unit == profile_unit::bit);
    static_assert(GroupSize >= 3 && GroupSize != std::numeric_limits<std::uint64_t>::max() &&
                  std::has_single_bit(GroupSize + 1), "group size must be 2^n - 1 and at least three");
    static_assert(Unit == profile_unit::bit || std::is_same_v<BackspaceCode, exponential_golomb<0>>,
                  "byte profiles use varints and require the default backspace policy");
    static_assert(CodecBlockSize && CodecBlockSize <= std::numeric_limits<std::uint32_t>::max(),
                  "codec block size must fit a positive 32-bit count");
    using value_layout = Values;
    using backspace_encoding = BackspaceCode;
    static constexpr bit_backspace_code backspace_code = policy_detail::backspace_traits<BackspaceCode>::code;
    static constexpr std::uint64_t backspace_parameter = policy_detail::backspace_traits<BackspaceCode>::parameter;
    static constexpr std::uint64_t group_size = GroupSize;
    static constexpr std::uint64_t codec_block_size = CodecBlockSize;
    static constexpr unsigned class_bits = static_cast<unsigned>(std::bit_width(GroupSize));
    static constexpr profile_unit unit = Unit;
    static constexpr unsigned unit_shift = Unit == profile_unit::byte ? 3 : 0;
    static constexpr unsigned bits_per_unit = 1u << unit_shift;
    static constexpr bool fixed_width = policy_detail::value_traits<Values>::fixed;
    static constexpr std::optional<std::uint64_t> value_width = policy_detail::value_traits<Values>::width;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's policy support.
 */
