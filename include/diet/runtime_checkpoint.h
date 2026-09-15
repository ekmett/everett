/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Selects runtime checkpoint codecs and encodes binary admission intervals.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/cola_runtime.h>
#include <diet/sqlite_catalog.h>

namespace diet {
  struct runtime_checkpoint {
    std::vector<cola_runtime_interval> intervals;
    std::vector<std::byte> semantic;
  };

  namespace runtime_store_detail {
    inline constexpr std::array<std::byte, 8> magic{
      std::byte{'D'}, std::byte{'I'}, std::byte{'E'}, std::byte{'T'},
      std::byte{'R'}, std::byte{'T'}, std::byte{0}, std::byte{1}};

    template <class P> std::vector<std::byte> checkpoint(cola_runtime_snapshot<P> const & source,
        std::span<std::byte const> semantic) {
      std::vector<std::byte> out(magic.begin(), magic.end());
      catalog_detail::number(out, source.runs().size());
      for (auto const & run : source.runs()) {
        catalog_detail::number(out, run.first); catalog_detail::number(out, run.last);
      }
      catalog_detail::binary(out, semantic);
      return out;
    }
    inline runtime_checkpoint checkpoint(std::span<std::byte const> encoded) {
      if (encoded.size() < magic.size() || !std::equal(magic.begin(), magic.end(), encoded.begin()))
        throw std::invalid_argument("unsupported Diet runtime checkpoint");
      catalog_detail::outcome_reader input{encoded.subspan(magic.size())};
      auto count = input.number();
      if (count > 65 || count > input.data.size() / 16)
        throw std::invalid_argument("invalid Diet runtime interval count");
      runtime_checkpoint result;
      result.intervals.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i != count; ++i) {
        auto first = input.number(), last = input.number();
        result.intervals.push_back({first, last});
      }
      auto size = input.number();
      if (size != input.data.size()) throw std::invalid_argument("invalid Diet runtime semantic extent");
      result.semantic.assign(input.data.begin(), input.data.end());
      return result;
    }
  }

  template <class Snapshot> struct decoded_runtime_checkpoint {
    Snapshot snapshot;
    std::vector<std::byte> semantic;
  };
  template <class Family> struct runtime_storage_codec;
  template <class P> struct runtime_storage_codec<binary_runtime_family<P>> {
    using snapshot_type = cola_runtime_snapshot<P>;
    template <class Pair, class Native> static void collect(snapshot_type const & source, Pair pair, Native) {
      pair(source.query_root().head());
    }
    template <class Pair, class Native> static std::vector<std::byte> encode(snapshot_type const & source,
        std::span<std::byte const> semantic, Pair, Native) { return runtime_store_detail::checkpoint(source, semantic); }
    template <class Resolver> static decoded_runtime_checkpoint<snapshot_type> decode(
        std::span<std::byte const> data, blob_identity const & head, Resolver & resolver) {
      auto value = runtime_store_detail::checkpoint(data);
      return {snapshot_type::restore(resolver.pair(head), value.intervals), std::move(value.semantic)};
    }
  };
}
