/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Selects the charged runtime and streamed output used by ordinary named taps.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/sort_runtime_context.h>
#include <diet/sort_runtime_store.h>

namespace diet {
  namespace active_detail {
    template <class P, profile_unit = P::unit> struct family;
    template <class P> struct family<P, profile_unit::bit> {
      using type = streaming_sort_runtime_family<P>;
    };
    template <class P> struct family<P, profile_unit::byte> {
      using type = redundant_runtime_family<P>;
    };
    template <class P> using engine = typed_engine<P, wrapping_fingerprint_algebra, 256, typename family<P>::type>;
  }

  // The registry chooses the physical unit. Bit registries use sort-owned
  // streamed records; byte registries retain their byte-aligned transport.
  // Both run the redundant scheduler and charge service at admission.
  template <class P = string_policy> struct active_engine : active_detail::engine<P> {
    using base_type = active_detail::engine<P>;
    using cola_type = typename base_type::cola_type;
    using base_type::base_type;
    active_engine() = default;
    active_engine(active_engine const &) = delete;
    active_engine & operator=(active_engine const &) = delete;
    active_engine(active_engine &&) noexcept(std::is_nothrow_move_constructible_v<base_type>) = default;
    active_engine & operator=(active_engine &&) noexcept(std::is_nothrow_move_assignable_v<base_type>) = default;
    static active_engine from_snapshot(cola_type value) {
      return active_engine(base_type::from_snapshot(std::move(value)));
    }
    template <class Storage> static active_engine from_snapshot(cola_type value, Storage storage)
      requires requires { base_type::from_snapshot(std::move(value), std::move(storage)); } {
      return active_engine(base_type::from_snapshot(std::move(value), std::move(storage)));
    }
  private:
    explicit active_engine(base_type value) : base_type(std::move(value)) {}
  };
}
