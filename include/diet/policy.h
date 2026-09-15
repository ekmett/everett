/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's policy support.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/registry.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace diet {
  enum class stream_role : std::uint8_t { native, borrowed };

  enum class bit_backspace_code : std::uint8_t { exponential_golomb = 0, golomb = 1 };
  template <std::uint64_t Order = 0> struct exponential_golomb {
    static_assert(Order <= 63, "exponential-Golomb order must be at most 63");
  };
  template <std::uint64_t M> struct golomb {
    static_assert(M != 0, "Golomb modulus must be positive");
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
  }

  // The registry chooses units and the common value width, if any. Sort
  // encoding widths are converted to those units, including byte leaves
  // below bit discriminators. Associated colas and streams retain this policy.
  template <class Registry = unsorted<std::optional<std::string>>, std::uint64_t GroupSize = 15,
            class BackspaceCode = exponential_golomb<0>, std::uint64_t CodecBlockSize = GroupSize>
  struct storage_policy {
    using registry_type = Registry;
    using registry = registry_traits<Registry>;
    static constexpr profile_unit unit = registry::unit;
    static_assert(GroupSize >= 3 && GroupSize != std::numeric_limits<std::uint64_t>::max() &&
                  std::has_single_bit(GroupSize + 1), "group size must be 2^n - 1 and at least three");
    static_assert(unit == profile_unit::bit || std::is_same_v<BackspaceCode, exponential_golomb<0>>,
                  "byte profiles use varints and require the default backspace policy");
    static_assert(CodecBlockSize && CodecBlockSize <= std::numeric_limits<std::uint32_t>::max(),
                  "codec block size must fit a positive 32-bit count");
    using backspace_encoding = BackspaceCode;
    static constexpr bit_backspace_code backspace_code = policy_detail::backspace_traits<BackspaceCode>::code;
    static constexpr std::uint64_t backspace_parameter = policy_detail::backspace_traits<BackspaceCode>::parameter;
    static constexpr std::uint64_t group_size = GroupSize;
    static constexpr std::uint64_t codec_block_size = CodecBlockSize;
    static constexpr unsigned class_bits = static_cast<unsigned>(std::bit_width(GroupSize));
    static constexpr unsigned unit_shift = unit == profile_unit::byte ? 3 : 0;
    static constexpr unsigned bits_per_unit = 1u << unit_shift;
    static constexpr bool fixed_width = registry::fixed_width;
    static constexpr std::optional<std::uint64_t> value_width = registry::value_width;
  };

  // The ordinary string table uses bit addressing and leaves one subtree for
  // future sorts. Its sampling and count-code defaults need no tuning.
  using string_registry = bin<tip<unsorted<std::optional<std::string>>>, sort_undefined>;
  using string_policy = storage_policy<string_registry>;
}
