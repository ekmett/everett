/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace everett {
  // A legitimate ring choice, Z/(2^64). Unsigned overflow is intentional.
  // Other policies need zero(), lift(u64), add(), subtract(), and multiply().
  struct wrapping_fingerprint_algebra {
    using element = std::uint64_t;
    static constexpr element zero() noexcept { return 0; }
    static constexpr element lift(std::uint64_t x) noexcept { return x; }
    static constexpr element add(element x, element y) noexcept { return x + y; }
    static constexpr element subtract(element x, element y) noexcept { return x - y; }
    static constexpr element multiply(element x, element y) noexcept { return x * y; }
  };

  // Stable byte hashing for the reference u64-valued store. This is a sanity
  // fingerprint, neither an authentication mechanism nor a physical file ID.
  struct u64_table_hash {
    static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
      x ^= x >> 30;
      x *= 0xbf58476d1ce4e5b9ULL;
      x ^= x >> 27;
      x *= 0x94d049bb133111ebULL;
      return x ^ (x >> 31);
    }

    std::uint64_t key(std::string_view text) const noexcept {
      std::uint64_t result = 0xcbf29ce484222325ULL;
      for (unsigned char c : text) {
        result ^= c;
        result *= 0x100000001b3ULL;
      }
      return mix(result);
    }

    std::uint64_t value(std::uint64_t x) const noexcept {
      return mix(x ^ 0xd6e8feb86659fd93ULL);
    }
  };

  template <class A = wrapping_fingerprint_algebra> struct table_fingerprint {
    using element = typename A::element;

    template <class V, class H>
    static element binding(std::string_view key, std::optional<V> const & value,
      H const & hash) {
      // h_V(Nothing) = 0 independently of the supplied value hash.
      return value ? A::multiply(A::lift(hash.key(key)), A::lift(hash.value(*value)))
                   : A::zero();
    }

    template <class V, class H>
    static element delta(std::string_view key, std::optional<V> const & before,
      std::optional<V> const & after, H const & hash) {
      auto old_hash = before ? A::lift(hash.value(*before)) : A::zero();
      auto new_hash = after ? A::lift(hash.value(*after)) : A::zero();
      return A::multiply(A::lift(hash.key(key)), A::subtract(new_hash, old_hash));
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's fingerprint support.
 */
