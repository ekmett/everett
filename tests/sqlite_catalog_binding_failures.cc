/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks binding acknowledgment failures, runtime owner lifetimes and canonical fallback.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/runtime_store.h>

#include <cassert>
#include <iostream>

namespace {
  using namespace diet;
  using P = storage_policy<tip<encoded_sort<bit_encoding<>>>, 3>;
  using family = binary_runtime_family<P>;
  using node = family::node_type;
  using native = family::native_type;
  using snapshot = family::snapshot_type;
  using store = runtime_store<P>;
  using sealer = runtime_store_detail::graph_sealer<P, random_object_ids, sqlite_catalog_ops, family>;

  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-binding-failure-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action) {
    bool caught = false;
    try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  profile_record row(std::string_view key, std::string_view value) {
    return {bit_string::from_bytes(key), bit_string::from_bytes(value)};
  }
  node::pair_type pair(std::string_view key, std::string_view value, node::pair_type main = {}) {
    std::array records{row(key, value)};
    auto data = native::from_owned(profile_array<P>::build(records));
    return node::from_built(node::built_type::adopt_native(std::move(data), std::move(main)));
  }
  snapshot singleton() {
    std::array intervals{cola_runtime_interval{0, 1}};
    return snapshot::restore(pair("key", "value"), intervals);
  }
  std::vector<bit_string> matches(snapshot const & source, std::string_view key) {
    auto encoded = bit_string::from_bytes(key);
    auto cursor = source.cursor(encoded.view());
    std::vector<bit_string> result;
    while (!cursor.done()) {
      cursor.step(1);
      if (cursor.has_match()) result.push_back(cursor.take_match().value);
    }
    return result;
  }
  std::size_t files(std::filesystem::path const & root, std::string_view extension) {
    std::size_t count = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == extension) ++count;
    return count;
  }

  struct failure_state {
    unsigned target, commits = 0;
    bool after;
  };
  struct failure_ops {
    std::shared_ptr<failure_state> state;
    int commit(sqlite3 * db) noexcept {
      bool fail = ++state->commits == state->target;
      if (fail && !state->after) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return result == SQLITE_OK && fail ? SQLITE_IOERR_FSYNC : result;
    }
  };
  void acknowledgment_failures() {
    // A terminal pair has five transactions: reserve/seal native,
    // reserve index, seal/register pair, publish the first named root.
    // A successful COMMIT reported as a failure must not install a binding.
    for (unsigned stage = 1; stage <= 5; ++stage) for (bool after : {false, true}) {
      temporary dir;
      { auto initialized = store::create(dir.root); }
      auto source = singleton();
      auto state = std::make_shared<failure_state>(failure_state{stage, 0, after});
      using faulty_store = runtime_store<P, random_object_ids, failure_ops>;
      auto faulty = faulty_store::open(dir.root, {}, {}, {state});
      try { (void)faulty.create_tap("uncertain", source); assert(false); }
      catch (catalog_error const & error) {
        assert(error.outcome_unknown && error.operation == faulty.last_operation());
      }
      assert(faulty.failed() && state->commits == stage);
      rejects([&] { (void)faulty.create_tap("forbidden-retry", source); });

      auto catalog = sqlite_catalog<P>::open(dir.root);
      random_object_ids ids;
      sealer graph(catalog, ids);
      auto original = source.query_root().head();
      std::optional<object_id> acknowledged_native;
      std::optional<blob_identity> acknowledged_pair;
      if (stage >= 3) acknowledged_native = graph.native_id(original->native_owner());
      else rejects([&] { (void)graph.native_id(original->native_owner()); });
      if (stage == 5) acknowledged_pair = graph.pair_id(original);
      else rejects([&] { (void)graph.pair_id(original); });

      auto before_natives = files(dir.root, ".kv"), before_indexes = files(dir.root, ".index");
      auto healthy = store::open(dir.root);
      auto result = healthy.create_tap("healthy-retry", source); // The exact same original owners.
      assert(!healthy.failed() && matches(result.snapshot, "key") == std::vector{bit_string::from_bytes("value")});
      auto actual = result.head.timeline.head;
      if (acknowledged_native) assert(actual.native == *acknowledged_native);
      if (acknowledged_pair) assert(actual == *acknowledged_pair);
      assert(files(dir.root, ".kv") == before_natives + (acknowledged_native ? 0 : 1));
      assert(files(dir.root, ".index") == before_indexes + (acknowledged_pair ? 0 : 1));
      auto uncertain = healthy.find("uncertain");
      assert(bool(uncertain) == (stage == 5 && after));
      if (uncertain) assert(uncertain->head.timeline.head == actual);
      auto reopened = store::open(dir.root).find("healthy-retry");
      assert(reopened && reopened->head == result.head);
    }
  }

  void lifetimes() {
    temporary dir;
    auto adapter = store::create(dir.root);
    std::vector<std::weak_ptr<node const>> original_pairs, mapped_pairs;
    std::vector<std::weak_ptr<native const>> original_natives, mapped_natives;
    std::vector<std::weak_ptr<mapped_cola_blob<P> const>> physical_pairs;
    std::vector<std::weak_ptr<mapped_native<P> const>> physical_natives;
    std::optional<stored_runtime<P>> retained;
    {
      std::array intervals{cola_runtime_interval{0, 1}, cola_runtime_interval{1, 2}};
      auto source = snapshot::restore(pair("key", "new", pair("key", "old")), intervals);
      for (auto p = source.query_root().head(); p; p = p->main_target()) {
        original_pairs.push_back(p); original_natives.push_back(p->native_owner());
      }
      retained = adapter.create_tap("lifetime", source);
      for (auto p = retained->snapshot.query_root().head(); p; p = p->main_target()) {
        mapped_pairs.push_back(p); mapped_natives.push_back(p->native_owner());
        physical_pairs.push_back(p->mapped()); physical_natives.push_back(p->native_owner()->mapped());
      }
      // Reusing already canonical owners must not memoize a pointer to self.
      auto again = adapter.publish(retained->head, retained->snapshot);
      assert(again.snapshot.query_root().head() == retained->snapshot.query_root().head());
      retained = std::move(again);
    }
    for (auto const & weak : original_pairs) assert(weak.expired());
    for (auto const & weak : original_natives) assert(weak.expired());
    for (auto const & weak : mapped_pairs) assert(!weak.expired());
    assert(matches(retained->snapshot, "key") ==
      (std::vector{bit_string::from_bytes("new"), bit_string::from_bytes("old")}));
    retained.reset();
    // The adapter is still alive. It must not keep any owner or mapping cache.
    for (auto const & weak : mapped_pairs) assert(weak.expired());
    for (auto const & weak : mapped_natives) assert(weak.expired());
    for (auto const & weak : physical_pairs) assert(weak.expired());
    for (auto const & weak : physical_natives) assert(weak.expired());
    assert(adapter.find("lifetime")); // Durable pins survive the last in-memory owner.
  }

  void distinct_facades() {
    temporary dir;
    auto adapter = store::create(dir.root);
    { (void)adapter.create_tap("original", singleton()); }
    auto left = store::open(dir.root).find("original"), right = store::open(dir.root).find("original");
    assert(left && right && left->head.timeline.head == right->head.timeline.head);
    auto own = left->snapshot.query_root().head()->native_owner();
    auto main = right->snapshot.query_root().head();
    assert(own != main->native_owner() && own->mapped() != main->native_owner()->mapped());
    auto parent = node::from_built(node::built_type::adopt_native(own, main));
    std::array intervals{cola_runtime_interval{0, 1}, cola_runtime_interval{1, 2}};
    auto source = snapshot::restore(parent, intervals);
    auto natives = files(dir.root, ".kv"), indexes = files(dir.root, ".index");
    auto combined = adapter.create_tap("combined", source);
    assert(files(dir.root, ".kv") == natives && files(dir.root, ".index") == indexes + 1);
    auto canonical = combined.snapshot.query_root().head();
    assert(canonical->main_target() && canonical->native_owner() == canonical->main_target()->native_owner());
    assert(canonical->mapped()->native_object() == canonical->main_target()->mapped()->native_object());
    assert(canonical->main_target() != main && canonical->native_owner() != own);
    assert(combined.snapshot.runs().size() == 2 && combined.snapshot.admissions() == 2);
    auto expected = std::vector{bit_string::from_bytes("value"), bit_string::from_bytes("value")};
    assert(matches(combined.snapshot, "key") == expected);
    auto reopened = store::open(dir.root).find("combined");
    assert(reopened && reopened->head == combined.head && matches(reopened->snapshot, "key") == expected);
    auto reloaded = reopened->snapshot.query_root().head();
    assert(reloaded->native_owner() == reloaded->main_target()->native_owner());
    adapter.save("twice", combined.head);
    auto forked = adapter.fork("branch", combined.head);
    assert(forked.head.auxiliary == combined.head.auxiliary && matches(forked.snapshot, "key") == expected);
    assert(files(dir.root, ".kv") == natives && files(dir.root, ".index") == indexes + 1);
  }
}
int main() {
  try { acknowledgment_failures(); lifetimes(); distinct_facades(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
