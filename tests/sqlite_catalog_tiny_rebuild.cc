/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks bounded clean construction without intermediate durable candidate files.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/replacement_rebuild.h>
#include <everett/sort_runtime_context.h>
#include <everett/sort_runtime_store.h>

#include <cassert>
#include <iostream>
#include <map>
#include <unordered_set>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "everett-tiny-rebuild-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class Engine> void drain(Engine & engine) {
    unsigned steps = 0;
    while (engine.pending()) { engine.advance(1'000'000); assert(++steps < 1000); }
    assert(engine.admission_ready());
  }
  template <class World> void verify(World const & state, std::map<std::string, std::string> const & expected) {
    assert(state.live_count() == expected.size());
    std::uint64_t sum = 0;
    for (auto const & [key, value] : expected) {
      assert(state.get(key) == value);
      sum += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, value);
    }
    assert(state.signature() == sum);
    auto cursor = scan(state); auto next = expected.begin();
    while (!cursor.done()) {
      cursor.step(1);
      if (cursor.has_row()) {
        auto row = cursor.take_row();
        assert(next != expected.end() && row.key == next->first && row.value == next->second); ++next;
      }
    }
    assert(next == expected.end());
  }
  std::size_t files(std::filesystem::path const & root, std::string_view extension) {
    std::size_t result = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == extension) ++result;
    return result;
  }
  struct failure_state { unsigned commits = 0; bool after; };
  struct failure_ops {
    std::shared_ptr<failure_state> state;
    int commit(sqlite3 * db) noexcept {
      auto fail = ++state->commits == 2; // A new final native's seal acknowledgment.
      if (fail && !state->after) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return result == SQLITE_OK && fail ? SQLITE_IOERR_FSYNC : result;
    }
  };

  template <class P> void publication(std::optional<bool> fail = {}) {
    using owned_family = sort_runtime_family<P>;
    using family = streaming_sort_runtime_family<P>;
    using typed = typed_engine<P, wrapping_fingerprint_algebra, 256, owned_family>;
    using core = replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, family>;
    using storage = typename family::storage_type;
    using owned_store = runtime_store<P, random_object_ids, sqlite_catalog_ops, owned_family>;
    using store = runtime_store<P, random_object_ids, sqlite_catalog_ops, family>;
    temporary dir;
    auto original = owned_store::create(dir.root);
    typed history;
    std::map<std::string, std::string> expected;
    for (unsigned n = 0; n != 64; ++n) {
      auto key = "key-" + std::to_string(n); expected[key] = "original";
      history.contribute(typed::put(key, "original"));
    }
    history.contribute(typed::erase("key-0")); expected.erase("key-0");
    for (unsigned n = 0; n != 15; ++n) {
      auto value = "revision-" + std::to_string(n); expected["key-1"] = value;
      history.contribute(typed::put("key-1", value));
    }
    drain(history);
    auto historical = history.snapshot();
    typename core::metadata_type metadata(historical.metadata(), 64, 16, true);
    auto dirty = original.create_session("small", historical.runtime(), metadata.encode());
    auto adapter = store::open(dir.root);
    auto found = adapter.find("small"); assert(found);
    auto mapped = core::world_type::restore(found->snapshot, metadata, metadata.schema_id);
    auto context = storage::open(dir.root);
    auto active = core::from_snapshot(mapped, context);
    assert(active.pending() && !active.admission_ready());
    auto natives = files(dir.root, ".kv"), indexes = files(dir.root, ".index");
    auto native_seals = context.context()->sealed_outputs(), index_seals = context.context()->sealed_indexes();
    drain(active);
    auto clean = active.snapshot(); verify(clean, expected);
    assert(clean.runtime().admissions() == 63 && clean.metadata().clean_base == 63 && !clean.metadata().mutations);
    assert(active.work().tiny_generations == 1 && active.work().tiny_indexes && active.work().tiny_conversion_charged);
    assert(active.work().scan_records && active.work().clean_rows == 63);
    assert(active.work().candidate_charged <= active.work().committed);
    assert(active.storage().context() == context.context());
    assert(context.context()->sealed_outputs() == native_seals && context.context()->sealed_indexes() == index_seals);
    assert(files(dir.root, ".kv") == natives && files(dir.root, ".index") == indexes);

    // Only the final converted frontier is about to acquire physical files.
    std::unordered_set<typename family::node_type const *> pairs;
    std::unordered_set<typename family::native_type const *> data;
    auto visit = [&](auto && self, auto const & pair) -> void {
      if (!pair || !pairs.insert(pair.get()).second) return;
      assert(pair->built()); data.insert(pair->native_owner().get());
      if (pair->secondary_target()) data.insert(pair->secondary_target().get());
      self(self, pair->main_target());
    };
    runtime_storage_codec<family>::collect(clean.runtime(), [&](auto const & pair) { visit(visit, pair); },
      [&](auto const & native) { if (native) data.insert(native.get()); });
    if (fail) {
      using faulty_store = runtime_store<P, random_object_ids, failure_ops, family>;
      auto state = std::make_shared<failure_state>(failure_state{0, *fail});
      auto faulty = faulty_store::open(dir.root, {}, {}, {state});
      try { (void)faulty.publish(dirty.head, clean.runtime(), clean.metadata().encode()); assert(false); }
      catch (catalog_error const & error) { assert(error.outcome_unknown); }
      assert(faulty.failed() && state->commits == 2);
      auto unchanged = store::open(dir.root).find("small"); assert(unchanged && unchanged->head == dirty.head);
      verify(mapped, expected); verify(clean, expected);
      assert(!active.failed()); // This independent adapter cannot poison the unrelated executor.
      natives = files(dir.root, ".kv"); indexes = files(dir.root, ".index");
    }
    auto saved = adapter.publish(dirty.head, clean.runtime(), clean.metadata().encode());
    assert(files(dir.root, ".kv") == natives + data.size());
    assert(files(dir.root, ".index") == indexes + pairs.size());
    auto restored = core::world_type::restore(saved.snapshot, clean.metadata(), metadata.schema_id);
    active.rebase(restored);
    assert(active.storage().context() == context.context()); verify(active.snapshot(), expected);
    auto reopened = store::open(dir.root).find("small"); assert(reopened && reopened->head == saved.head);
    verify(core::world_type::restore(reopened->snapshot, clean.metadata(), metadata.schema_id), expected);

    if (!fail) {
      auto quote = core::reservation(core::put("key-0", "returned")); auto before = active.work();
      active.contribute(core::put("key-0", "returned")); expected["key-0"] = "returned";
      auto sixty_four = active.snapshot();
      assert(active.work().tiny_generations == 1 && sixty_four.runtime().admissions() == 64);
      assert(active.work().granted - before.granted + active.work().foreground_charged - before.foreground_charged <= quote.work);
      active.contribute(core::put("key-64", "large")); expected["key-64"] = "large";
      assert(active.work().tiny_generations == 1 && !active.snapshot().metadata().mutations &&
        active.snapshot().metadata().clean_base == 65);
      drain(active); verify(active.snapshot(), expected);
      // The retained dirty and clean snapshots still own their original contents.
      expected.erase("key-64"); expected.erase("key-0"); verify(mapped, expected); verify(clean, expected);
    }
  }
  void owning_oracle() {
    using core = replacement_rebuild_engine<>;
    core active; std::map<std::string, std::string> expected;
    for (unsigned n = 0; n != 65; ++n) {
      auto key = std::to_string(n); expected[key] = "v"; active.contribute(core::put(key, "v"));
    }
    active.contribute(core::put("0", "changed")); expected["0"] = "changed";
    drain(active); verify(active.snapshot(), expected);
    for (unsigned n = 0; n != 65; ++n) active.contribute(core::erase(std::to_string(n)));
    drain(active); verify(active.snapshot(), {});
    auto empty = active.snapshot();
    assert(empty.runtime().admissions() == 0 && !active.work().tiny_generations);
  }
  template <class P> struct owning_clean_storage : sort_runtime_storage<P> {
    using clean_storage_type = sort_runtime_storage<P>;
    std::shared_ptr<unsigned> identity = std::make_shared<unsigned>(0);
  };
  void every_small_frontier() {
    // Exercise every cardinality and its final hidden dependency closure
    // without making 130 independent durable cleanups part of this test.
    using P = storage_policy<string_registry, 3, golomb<3>, 5>;
    using family = sort_runtime_family<P, registry_selector<string_registry>, owning_clean_storage<P>>;
    using core = replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, family>;
    core active; auto context = active.storage().identity;
    std::map<std::string, std::string> expected;
    for (unsigned n = 0; n != 65; ++n) {
      auto key = std::to_string(n); expected[key] = "initial";
      auto command = core::put(key, "initial"); auto quote = core::reservation(command); auto before = active.work();
      active.contribute(std::move(command));
      auto after = active.work();
      assert(after.granted - before.granted + after.foreground_charged - before.foreground_charged <= quote.work);
      assert(active.storage().identity == context); verify(active.snapshot(), expected);
      if (n < 64) { auto state = active.snapshot(); assert(state.runtime().admissions() == n + 1); }
    }
    for (unsigned n = 0; n != 65; ++n) {
      auto key = std::to_string(n);
      if (!(n % 3)) { active.contribute(core::put(key, "changed")); expected[key] = "changed"; }
      active.contribute(core::erase(key)); expected.erase(key);
      verify(active.snapshot(), expected); assert(active.storage().identity == context);
    }
    drain(active);
    auto empty = active.snapshot(); assert(empty.runtime().admissions() == 0 && active.work().tiny_generations >= 64);
  }
  void empty_streamed() {
    using family = streaming_sort_runtime_family<>;
    using core = replacement_rebuild_engine<string_policy, wrapping_fingerprint_algebra, 256, family>;
    temporary dir;
    { auto catalog = sqlite_catalog<string_policy>::create_sessions(dir.root, random_object_ids{}()); }
    auto storage = family::open_storage(dir.root);
    core seed; auto active = core::from_snapshot(seed.snapshot(), storage);
    active.contribute(core::put("only", "value"));
    active.contribute(core::erase("only"));
    verify(active.snapshot(), {});
    auto empty = active.snapshot();
    assert(empty.runtime().admissions() == 0 && active.work().tiny_generations == 1);
    assert(active.storage().context() == storage.context());
  }
}
int main() {
  try {
    owning_oracle(); every_small_frontier(); empty_streamed(); publication<string_policy>();
    using tight = storage_policy<string_registry, 3>;
    publication<tight>(false); publication<tight>(true);
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
