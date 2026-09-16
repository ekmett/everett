/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks changed values and tombstones through generated offset readers.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#define main search_smoke_main
#include "bench.cc"
#undef main

namespace {
  template <bool Bit> void chronology(unsigned width, bool random) {
    constexpr unsigned count = 2048;
    fixture<Bit> data(count, width, random, 16);
    using P = typename fixture<Bit>::P;
    using native = typename fixture<Bit>::native;
    using blob = typename fixture<Bit>::blob;
    auto replacement = [](u64 ordinal) -> std::optional<std::string> {
      if (!(ordinal % 51)) return std::nullopt;
      return value(ordinal ^ 0xbadc0ffee0000000ull);
    };
    auto source = [&] {
      if constexpr (Bit) {
        sort_profile_writer<P> writer;
        for (auto const & [k, ordinal] : data.records)
          if (!(ordinal % 17)) writer.template append<strings>(k, replacement(ordinal));
        return writer.finish();
      } else {
        profile_native_writer<P> writer;
        for (auto const & [k, ordinal] : data.records)
          if (!(ordinal % 17)) writer.append(profile_record{typed_detail::key<P, strings>(k),
              typed_detail::value<P, strings>(replacement(ordinal))});
        return writer.finish();
      }
    }();
    auto bytes = [&] {
      if constexpr (Bit) return encoded_sort_sections<P>::from(source).materialize();
      else return encode_native_sections(source).materialize();
    }();
    auto id = data.identity(); auto path = data.directory.path / (id.hex() + ".kv"); write(path, bytes);
    auto mapped = std::make_shared<native const>(native::open(path));
    auto head = data.index(mapped, id, data.head);
    auto [empty, empty_id] = data.save(1, true);
    while (head->virtual_size() > P::group_size) head = data.index(empty, empty_id, head);
    auto root = cola_query_root<P, blob>::adopt_prepared(head);
    for (auto const & [k, ordinal] : data.records) {
      auto expected = ordinal % 17 ? std::optional(value(ordinal)) : replacement(ordinal);
      require(data.get(root, k) == expected, "newest replacement/tombstone differs");
      require(!data.get(root, key(2 * ordinal + 1, width, random)), "unexpected neighboring key");
    }
  }
}

int main() {
  try {
    chronology<false>(16, false); chronology<false>(128, true);
    chronology<true>(16, false); chronology<true>(128, true);
    std::cout << "16384 mapped changed-value/tombstone/miss checks passed\n";
  } catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
}
