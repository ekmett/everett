/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks typed updates, chronological sort composition and snapshot fingerprints.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/typed_world.h>

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
  using namespace everett;
  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class F> void rejects(F && f) {
    try { f(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected rejection");
  }
  template <class E> void drain(E & engine) {
    while (engine.pending()) engine.advance(4096);
  }
  using default_sort = unsorted<std::optional<std::string>>;

  void strings() {
    typed_engine<> engine;
    auto empty = engine.snapshot();
    check(!empty.get("a") && empty.live_count() == 0, "initial string table");
    rejects([&] { empty.erase("absent"); });
    auto first = engine.contribute(empty.put("a", std::string("before")));
    auto second = engine.contribute(first.put("b", std::string("")));
    check(second.get("a") == "before" && second.get("b") == "" && second.live_count() == 2,
      "strings or present empty string lost");
    auto third = engine.contribute(second.put("a", std::string("after")));
    auto fourth = engine.contribute(third.erase("b"));
    check(first.get("a") == "before" && !first.get("b"), "retained snapshot changed");
    check(fourth.get("a") == "after" && !fourth.get("b") && fourth.live_count() == 1, "replacement or deletion");
    auto expected = sort_semantics<default_sort>::hash_key("a") *
      sort_semantics<default_sort>::hash_value("a", std::optional<std::string>("after"));
    check(fourth.signature() == expected, "incremental string fingerprint");
    drain(engine);
    check(engine.snapshot().metadata() == fourth.metadata() && engine.snapshot().get("a") == "after", "maintenance changed semantics");
    auto restarted = typed_engine<>::from_snapshot(engine.snapshot());
    auto next = restarted.contribute(restarted.snapshot().put(std::string("\0z", 2), std::string("\0v", 2)));
    check(next.get(std::string("\0z", 2)) == std::string("\0v", 2), "embedded zero string");
    auto metadata = typed_world_metadata<>::decode(next.metadata().encode());
    check(metadata == next.metadata(), "compact metadata roundtrip");
    auto restored = typed_world<>::restore(next.runtime(), metadata, metadata.schema_id);
    check(restored.get("a") == "after" && restored.signature() == next.signature(), "trusted metadata restore");
    rejects([&] { (void)typed_world<>::restore(next.runtime(), metadata, "other"); });
    ++metadata.live_count; metadata.live_count += next.runtime().admissions();
    rejects([&] { (void)typed_world<>::restore(next.runtime(), metadata, metadata.schema_id); });
    rejects([&] { (void)typed_world_metadata<>::decode({}); });
  }

  void batches_and_session() {
    typed_engine<> engine;
    auto base = engine.snapshot();
    auto a = base.put("a", std::string("1")), b = base.put("b", std::string("2"));
    auto left = typed_engine<>::from_snapshot(base), right = typed_engine<>::from_snapshot(base);
    left.contribute(a); auto ab = left.contribute(b);
    right.contribute(b); auto ba = right.contribute(a);
    check(ab.signature() == ba.signature() && ab.live_count() == ba.live_count() && ab.get("a") == ba.get("a"),
      "disjoint contributions did not commute");
    rejects([&] { left.contribute(base.put("a", std::string("wrong-base"))); });
    check(left.snapshot().get("a") == "1" && !left.failed(), "stale preflight changed or poisoned snapshot");
    rejects([&] { left.contribute(typed_engine<>::erase("absent")); });
    check(!left.failed() && left.snapshot().get("a") == "1", "invalid erase changed or poisoned snapshot");
    auto updates = left.snapshot().batch();
    updates.put("z", std::string("last")).put("c", std::string("middle")).erase("a");
    auto changed = left.contribute(std::move(updates).finish());
    check(!changed.get("a") && changed.get("c") == "middle" && changed.get("z") == "last" && changed.live_count() == 3,
      "batch changes");
    auto duplicate = changed.batch(); duplicate.put("same", std::string("1")).put("same", std::string("2"));
    rejects([&] { std::move(duplicate).finish(); });
    auto empty_batch = changed.batch();
    check(left.contribute(std::move(empty_batch).finish()).metadata() == changed.metadata(), "empty batch metadata");

    session<typed_engine<>> active(typed_engine<>{}, {1'000'000, 1'000'000, 32, 4096});
    auto initial = active.snapshot();
    auto one = active.submit(initial->world.put("x", std::string("one")));
    auto two = active.submit(initial->world.put("y", std::string("two")));
    check(one.get()->world.get("x") == "one" && two.get()->world.live_count() == 2, "typed session queued publication");
    auto three = active.apply(active.snapshot()->world.put("x", std::string("three")));
    check(three->world.get("x") == "three" && initial->world.live_count() == 0, "typed session apply and snapshot");
    auto overwrite_one = active.submit(typed_engine<>::put("same", std::string("one")));
    auto overwrite_two = active.submit(typed_engine<>::put("same", std::string("two")));
    check(overwrite_one.get()->world.get("same") == "one" && overwrite_two.get()->world.get("same") == "two",
      "unconditional same-key commands were treated as stale snapshots");
    check(!active.apply(typed_engine<>::erase("same"))->world.get("same"), "unconditional erase");
    active.shutdown();
    check(!active.failure(), "typed session worker failed");
  }

  template <class P> void replacement_oracle() {
    typed_engine<P> engine("oracle/string/1");
    std::map<std::string, std::string> expected;
    std::vector<std::pair<typed_world<P>, std::map<std::string, std::string>>> saved;
    auto verify = [](auto const & snapshot, auto const & entries) {
      std::uint64_t hash = 0;
      for (auto const & [key, value] : entries) {
        check(snapshot.get(key) == value, "oracle lookup");
        hash += sort_semantics<default_sort>::hash_key(key) *
          sort_semantics<default_sort>::hash_value(key, std::optional<std::string>(value));
      }
      check(snapshot.signature() == hash && snapshot.live_count() == entries.size(), "oracle count or hash");
      check(!snapshot.get("missing"), "oracle miss");
    };
    for (unsigned i = 0; i != 128; ++i) {
      auto before = engine.snapshot();
      std::string key = "prefix/"; key.push_back(char((i * 19) % 31));
      if (i % 7 == 0 && expected.contains(key)) {
        engine.contribute(before.erase(key)); expected.erase(key);
        check(!engine.snapshot().get(key), "oracle tombstone lost");
      } else {
        std::string value(i % 11, char(i));
        engine.contribute(before.put(key, value)); expected[key] = std::move(value);
      }
      if (i % 5 == 0) engine.advance(256);
      if (i % 17 == 0) saved.emplace_back(engine.snapshot(), expected);
      verify(engine.snapshot(), expected);
    }
    drain(engine); verify(engine.snapshot(), expected);
    for (auto const & [snapshot, entries] : saved) verify(snapshot, entries);
  }

  struct text_sort : sort_semantics<default_sort> {
    using encoding = byte_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
  };
  struct sum_sort {
    using encoding = byte_encoding<fixed_values<8>>;
    using key_codec = unsigned_key<16>;
    using value_codec = unsigned_value<64>;
    using state_type = std::uint64_t;
    static state_type initial(std::uint64_t) { return 0; }
    static state_type apply(std::uint64_t, state_type old, state_type delta) { return old + delta; }
    static state_type compose(std::uint64_t, state_type a, state_type b) { return a + b; }
    static bool present(std::uint64_t, state_type value) { return value != 0; }
    static std::uint64_t hash_key(std::uint64_t key) { return u64_table_hash::mix(key + 17); }
    static std::uint64_t hash_value(std::uint64_t, state_type value) { return u64_table_hash::mix(value + 19); }
  };
  struct append_sort {
    using encoding = byte_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = string_value<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type old, state_type const & delta) { return old + delta; }
    static state_type compose(std::string const &, state_type old, state_type const & delta) { return old + delta; }
    static bool present(std::string const &, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::string const & key) { return u64_table_hash{}.key(key) + 23; }
    static std::uint64_t hash_value(std::string const &, state_type const & value) { return u64_table_hash{}.key(value) + 29; }
  };
  using registry = bin<tip<text_sort>, bin<tip<sum_sort>, tip<append_sort>>>;
  using policy = storage_policy<registry>;
  using mixed_engine = typed_engine<policy>;

  void mixed_sorts() {
    rejects([] { mixed_engine invalid; });
    mixed_engine engine("typed-test/1");
    auto batch = engine.snapshot().batch();
    batch.put<text_sort>("word", std::string("value")).change<sum_sort>(9, 3).change<append_sort>("word", "A");
    auto current = engine.contribute(std::move(batch).finish());
    check(current.get<text_sort>("word") == "value" && current.get<sum_sort>(9) == 3 &&
      current.get<append_sort>("word") == "A" && current.live_count() == 3, "mixed typed dispatch");
    std::string expected = "A";
    for (unsigned i = 0; i != 19; ++i) {
      std::string next(1, char('a' + i)); expected += next;
      current = engine.contribute(current.change<append_sort>("word", next));
      check(current.get<append_sort>("word") == expected, "arrow query chronology");
      if (i % 3 == 0) { drain(engine); current = engine.snapshot(); }
    }
    current = engine.contribute(current.change<sum_sort>(9, 7));
    drain(engine); current = engine.snapshot();
    check(current.get<sum_sort>(9) == 10 && current.get<append_sort>("word") == expected, "per-sort merge composition");
    auto expected_hash = text_sort::hash_key("word") * text_sort::hash_value("word", std::optional<std::string>("value")) +
      sum_sort::hash_key(9) * sum_sort::hash_value(9, 10) + append_sort::hash_key("word") * append_sort::hash_value("word", expected);
    check(current.signature() == expected_hash, "mixed sort fingerprint or sort prefix hashed");
    using rebalanced = storage_policy<bin<bin<tip<text_sort>, tip<sum_sort>>, tip<append_sort>>>;
    typed_engine<rebalanced> other("typed-test/rebalanced");
    auto other_batch = other.snapshot().batch();
    other_batch.put<text_sort>("word", std::string("value")).change<sum_sort>(9, 10).change<append_sort>("word", expected);
    auto same = other.contribute(std::move(other_batch).finish());
    check(same.signature() == current.signature(), "rebalance changed sort-owned hashes");
  }
}

int main() try {
  strings(); batches_and_session(); mixed_sorts();
  replacement_oracle<string_policy>();
  replacement_oracle<storage_policy<default_sort, 7, exponential_golomb<0>, 16>>();
  std::cout << "typed worlds: strings, batches, registry composition and fingerprints passed\n";
} catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
