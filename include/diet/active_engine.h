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
#include <diet/replacement_rebuild.h>

namespace diet {
  namespace active_detail {
    template <class P, profile_unit = P::unit> struct family;
    template <class P> struct family<P, profile_unit::bit> {
      using type = streaming_sort_runtime_family<P>;
    };
    template <class P> struct family<P, profile_unit::byte> {
      using type = redundant_runtime_family<P>;
    };
    template <class P> inline constexpr bool rebuilding =
      std::same_as<typed_detail::default_sort_t<P>, unsorted<std::optional<std::string>>>;
    template <class P> using engine = std::conditional_t<rebuilding<P>,
      replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, typename family<P>::type>,
      typed_engine<P, wrapping_fingerprint_algebra, 256, typename family<P>::type>>;
  }

  // The registry chooses the physical unit. Bit registries use sort-owned
  // streamed records; byte registries retain their byte-aligned transport.
  // Both run the redundant scheduler and charge service at admission. The
  // single optional-string sort also rebuilds obsolete history as it shrinks.
  template <class P = string_policy> struct active_engine : active_detail::engine<P> {
    using base_type = active_detail::engine<P>;
    using cola_type = typename base_type::cola_type;
    using metadata_type = typename base_type::metadata_type;
    using runtime_snapshot = typename base_type::runtime_family::snapshot_type;
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
    // A code-stable extension can add a sort in a reserved hole. Its generic
    // executor reads the old replacement envelope but makes no cleanup-size
    // promise. Validate the entire old generation before projecting its table
    // metadata; pending structural merge jobs remain in the runtime frontier.
    static cola_type restore_checkpoint(runtime_snapshot runtime, std::span<std::byte const> bytes,
        std::string_view schema) {
      if (schema.empty()) throw std::invalid_argument("active checkpoint requires an expected schema");
      if constexpr (active_detail::rebuilding<P>)
        return cola_type::restore(std::move(runtime), metadata_type::decode(bytes), schema);
      else {
        metadata_type metadata;
        if (bytes.size() >= 56 && bytes.size() - 56 == schema.size()) {
          auto previous = replacement_metadata<>::decode(bytes);
          previous.validate(runtime.admissions());
          metadata = static_cast<metadata_type const &>(previous);
        } else if (bytes.size() >= 16 && bytes.size() - 16 == schema.size())
          metadata = metadata_type::decode(bytes);
        else throw std::invalid_argument("active checkpoint does not match the expected schema layout");
        return cola_type::restore(std::move(runtime), std::move(metadata), schema);
      }
    }
  private:
    explicit active_engine(base_type value) : base_type(std::move(value)) {}
  };
}
