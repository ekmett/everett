/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks streamed resolved rows against replacement and chronological-arrow oracles.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/typed_scan.h>

#include <cassert>
#include <iostream>
#include <map>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  template <class F> void rejects(F && fn) {
    bool rejected = false;
    try { fn(); } catch (std::exception const &) { rejected = true; }
    assert(rejected);
  }
  template <class World> void verify(World snapshot, std::map<std::string, std::string> const & expected) {
    auto rows = scan(snapshot);
    assert(rows.step(0) == 0 && rows.consumed() == 0);
    auto row = expected.begin();
    std::uint64_t total = 0;
    while (!rows.done()) {
      total += rows.step(1);
      if (rows.has_row()) {
        assert(rows.step(1) == 0); // A ready row keeps its place.
        auto item = rows.take_row();
        assert(row != expected.end() && item.key == row->first && item.value == row->second);
        ++row;
      }
    }
    assert(row == expected.end() && rows.consumed() == total && !rows.next());
    rejects([&] { rows.take_row(); });
  }
  template <class P> void replacement() {
    typed_engine<P> engine("scan/strings/1");
    std::map<std::string, std::string> expected;
    verify(engine.snapshot(), expected);
    std::uint64_t random = 0x769dfe352;
    for (unsigned i = 0; i != 513; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      std::string key = random % 5 ? "common-prefix/" + std::to_string(random % 71) :
        std::string(static_cast<std::size_t>(random % 7), '\0');
      if ((random & 7) == 0 && expected.contains(key)) {
        engine.contribute(typed_engine<P>::erase(key)); expected.erase(key);
      } else {
        std::string value(static_cast<std::size_t>(random % 11), char(i & 127));
        engine.contribute(typed_engine<P>::put(key, value)); expected[key] = value;
      }
      if (i % 37 == 0) verify(engine.snapshot(), expected);
    }
    auto old = engine.snapshot();
    auto rows = scan(old);
    rows.step(1);
    auto moved = std::move(rows);
    engine.contribute(typed_engine<P>::put("later", "change"));
    auto oracle = expected.begin();
    while (auto row = moved.next()) {
      assert(oracle != expected.end() && row->key == oracle->first && row->value == oracle->second); ++oracle;
    }
    assert(oracle == expected.end());
    verify(old, expected);
    expected["later"] = "change";
    while (engine.pending()) engine.advance(4096);
    verify(engine.snapshot(), expected);
  }

  struct append_sort {
    using encoding = bit_encoding<>;
    using key_codec = unsigned_key<16>;
    using value_codec = string_value<>;
    using state_type = std::string;
    inline static bool reject = false;
    static state_type initial(std::uint64_t) { return {}; }
    static state_type apply(std::uint64_t, state_type before, state_type const & next) {
      if (reject) throw std::runtime_error("scan callback");
      return before + next;
    }
    static state_type compose(std::uint64_t key, state_type before, state_type const & next) {
      return apply(key, std::move(before), next);
    }
    static bool present(std::uint64_t, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::uint64_t key) { return key + 17; }
    static std::uint64_t hash_value(std::uint64_t, state_type const & value) { return u64_table_hash{}.key(value); }
  };
  using mixed = storage_policy<bin<tip<strings>, bin<tip<append_sort>, sort_undefined>>>;
  void arrows() {
    typed_engine<mixed> engine("scan/mixed/1");
    std::map<std::string, std::string> text;
    std::map<std::uint64_t, std::string> expected;
    for (unsigned i = 0; i != 129; ++i) {
      auto key = std::uint64_t((i * 19) % 41);
      auto change = "[" + std::to_string(i) + "]";
      auto batch = engine.batch();
      batch.change<append_sort>(key, change).put<strings>(std::to_string(key), change);
      engine.contribute(std::move(batch).finish());
      expected[key] += change; text[std::to_string(key)] = change;
    }
    auto rows = scan<append_sort>(engine.snapshot());
    auto oracle = expected.begin();
    while (auto row = rows.next()) {
      assert(oracle != expected.end() && row->key == oracle->first && row->value == oracle->second);
      assert(row->value == engine.snapshot().template get<append_sort>(row->key)); ++oracle;
    }
    assert(oracle == expected.end());
    auto texts = scan<strings>(engine.snapshot());
    auto t = text.begin();
    while (auto row = texts.next()) { assert(t != text.end() && row->key == t->first && row->value == t->second); ++t; }
    assert(t == text.end());
    auto bad = scan<append_sort>(engine.snapshot());
    append_sort::reject = true;
    rejects([&] { bad.next(); });
    append_sort::reject = false;
    assert(bad.failed());
    rejects([&] { bad.step(1); });
    assert(engine.snapshot().template get<append_sort>(0) == expected[0]);
  }
}
int main() {
  replacement<everett::string_policy>();
  replacement<everett::storage_policy<strings, 7, everett::exponential_golomb<0>, 4>>();
  arrows();
  std::cout << "typed scan: replacements, chronological arrows, budgets and captured snapshots passed\n";
}
