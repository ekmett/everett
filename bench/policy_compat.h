/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Keeps benchmark policy parameters identical across registry API revisions.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#ifndef DIET_BENCH_POLICY_COMPAT_H
#define DIET_BENCH_POLICY_COMPAT_H

#include <diet/policy.h>
#include <type_traits>

namespace diet_bench {
#if __has_include(<diet/registry.h>)
  template <diet::profile_unit Unit, class Values = diet::variable_values,
            std::uint64_t GroupSize = 15, class BackspaceCode = diet::exponential_golomb<0>,
            std::uint64_t... CodecBlockSize>
  using policy = diet::storage_policy<diet::tip<diet::encoded_sort<std::conditional_t<
    Unit == diet::profile_unit::byte, diet::byte_encoding<Values>, diet::bit_encoding<Values>>>>,
    GroupSize, BackspaceCode, CodecBlockSize...>;
#else
  namespace detail {
    // The oldest snapshots predate independent codec widths. Delay applying
    // the optional fifth argument so their four-argument template still works.
    template <template <diet::profile_unit, class, std::uint64_t, class, std::uint64_t...> class Policy,
              diet::profile_unit Unit, class Values, std::uint64_t GroupSize,
              class BackspaceCode, std::uint64_t... CodecBlockSize>
    struct apply_policy {
      using type = Policy<Unit, Values, GroupSize, BackspaceCode, CodecBlockSize...>;
    };
  }
  template <diet::profile_unit Unit, class Values = diet::variable_values,
            std::uint64_t GroupSize = 15, class BackspaceCode = diet::exponential_golomb<0>,
            std::uint64_t... CodecBlockSize>
  using policy = typename detail::apply_policy<diet::storage_policy, Unit, Values,
    GroupSize, BackspaceCode, CodecBlockSize...>::type;
#endif
}

#endif
