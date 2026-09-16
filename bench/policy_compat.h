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

#ifndef EVERETT_BENCH_POLICY_COMPAT_H
#define EVERETT_BENCH_POLICY_COMPAT_H

#include <everett/policy.h>
#include <type_traits>

namespace everett_bench {
#if __has_include(<everett/registry.h>)
  template <everett::profile_unit Unit, class Values = everett::variable_values,
            std::uint64_t GroupSize = 15, class BackspaceCode = everett::exponential_golomb<0>,
            std::uint64_t... CodecBlockSize>
  using policy = everett::storage_policy<everett::tip<everett::encoded_sort<std::conditional_t<
    Unit == everett::profile_unit::byte, everett::byte_encoding<Values>, everett::bit_encoding<Values>>>>,
    GroupSize, BackspaceCode, CodecBlockSize...>;
#else
  namespace detail {
    // The oldest snapshots predate independent codec widths. Delay applying
    // the optional fifth argument so their four-argument template still works.
    template <template <everett::profile_unit, class, std::uint64_t, class, std::uint64_t...> class Policy,
              everett::profile_unit Unit, class Values, std::uint64_t GroupSize,
              class BackspaceCode, std::uint64_t... CodecBlockSize>
    struct apply_policy {
      using type = Policy<Unit, Values, GroupSize, BackspaceCode, CodecBlockSize...>;
    };
  }
  template <everett::profile_unit Unit, class Values = everett::variable_values,
            std::uint64_t GroupSize = 15, class BackspaceCode = everett::exponential_golomb<0>,
            std::uint64_t... CodecBlockSize>
  using policy = typename detail::apply_policy<everett::storage_policy, Unit, Values,
    GroupSize, BackspaceCode, CodecBlockSize...>::type;
#endif
}

#endif
