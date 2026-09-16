/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks streamed runtime ownership, rebase and I/O failure isolation.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sort_runtime_context.h>
#include <diet/typed_scan.h>

#include <cassert>
#include <iostream>
#include <map>

namespace {
  using namespace diet;
  using strings = unsorted<std::optional<std::string>>;
  using P = storage_policy<string_registry, 3>;
  using family = streaming_sort_runtime_family<P>;
  using storage = family::storage_type;
  using core = typed_engine<P, wrapping_fingerprint_algebra, 256, family>;
  using owned_core = typed_engine<P, wrapping_fingerprint_algebra, 256, sort_runtime_family<P>>;
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-stream-context-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action) {
    bool caught = false; try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  template <class E> void drain(E & engine) { while (engine.pending()) engine.advance(4096); }
  template <class C> void verify(C const & state, std::map<std::string, std::string> const & expected) {
    auto rows = diet::scan(state); auto oracle = expected.begin();
    while (auto row = rows.next()) {
      assert(oracle != expected.end() && row->key == oracle->first && row->value == oracle->second);
      assert(state.get(row->key) == row->value); ++oracle;
    }
    assert(oracle == expected.end());
  }
  void owned_parity() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    auto disk = storage::open(dir.root); auto context = disk.context();
    core seed; owned_core reference;
    auto active = core::from_snapshot(seed.snapshot(), disk);
    std::map<std::string, std::string> expected;
    std::vector<std::pair<core::cola_type, decltype(expected)>> saved;
    for (unsigned n = 0; n != 129; ++n) {
      auto key = std::string("a\0", 2) + std::to_string(n % 29);
      auto value = std::string(n % 31, char(n));
      active.contribute(core::put(key, value)); reference.contribute(owned_core::put(key, value)); expected[key] = value;
      if (n % 17 == 0) saved.emplace_back(active.snapshot(), expected);
      if (n % 23 == 0) {
        drain(active); auto before = context->sealed_outputs();
        active.rebase(active.snapshot()); // An equivalent publication retains the concrete storage owner.
        auto moved = std::move(active); active = std::move(moved);
        assert(context->sealed_outputs() == before && !context->failed());
      }
    }
    drain(active); drain(reference); verify(active.snapshot(), expected);
    assert(active.snapshot().metadata() == reference.snapshot().metadata());
    assert(context->sealed_outputs() > 30 && context->sealed_indexes() > 30 && !context->failed());
    unsigned mapped = 0;
    auto snapshot = active.snapshot();
    assert(snapshot.runtime().query_root().head()->mapped());
    snapshot.runtime().query_root().head()->mapped()->scan();
    for (auto const & level : snapshot.runtime().frontier().levels)
      for (auto const & slot : level.slots) if (slot.object && slot.object->pair) {
        assert(slot.object->pair->mapped() && !slot.object->pair->built());
        assert(slot.object->pair->native_owner() == slot.object->native);
      }
    for (auto const & run : snapshot.runtime().runs()) {
      auto native = run->native;
      if (native->owned()) { assert(!native->sealed()); continue; }
      ++mapped; assert(native->mapped() && native->sealed());
      auto seal = native->sealed(); assert(seal->catalog == catalog.identity());
      catalog.verify_sealed(seal->receipt, file_kind::native_blob);
      native->mapped()->scan();
      auto ordinary = family::native_type::from_mapped(native->mapped());
      assert(!ordinary->sealed()); // A caller-provided mapping cannot manufacture admission authority.
    }
    assert(mapped);
    for (auto const & [state, values] : saved) verify(state, values);
    auto prior = active.snapshot(); auto old_count = context->sealed_outputs();
    rejects([&] { active.rebase(seed.snapshot()); });
    assert(!active.failed() && context->sealed_outputs() == old_count && prior.metadata() == active.snapshot().metadata());
    // Mapping owners do not borrow the execution context or its SQLite handle.
    disk = storage{}; context.reset(); active = core{};
    verify(snapshot, expected);
    auto new_disk = storage::open(dir.root); auto new_context = new_disk.context();
    auto poisoned = core::from_snapshot(snapshot, new_disk);
    poisoned.poison();
    assert(poisoned.failed() && new_context->failed());
    verify(snapshot, expected);
  }
  void hidden_stages() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    auto disk = storage::open(dir.root);
    using Runtime = family::runtime_type<replace_native_value>;
    Runtime active(disk);
    auto first = core::put("a", "one"), second = core::put("b", "two");
    active.try_contribute(first.records()[0]); active.try_contribute(second.records()[0]);
    unsigned seen = 0, iterations = 0;
    while (active.pending()) {
      assert(++iterations < 10000);
      auto checkpoint = active.checkpoint(); unsigned phase = 0;
      for (auto const & level : checkpoint.frontier().levels) if (level.job) phase |= 1u << unsigned(level.job->stage);
      if (phase & ~seen) {
        auto recovered = Runtime::from_snapshot(checkpoint, disk);
        while (recovered.pending()) recovered.advance(8192);
        auto resumed = recovered.snapshot();
        resumed.query_root().head()->mapped()->scan();
        for (auto const & key : {"a", "b"}) {
          auto encoded = family::key_transport::encode<strings>(key);
          auto cursor = resumed.cursor(encoded.view());
          while (!cursor.done() && !cursor.has_match()) cursor.step(1);
          assert(cursor.has_match());
        }
        seen |= phase;
      }
      if (active.pending()) {
        auto cost = active.next_service_cost(), credit = active.credit(); active.advance(cost > credit ? cost - credit : 1);
      }
    }
    assert(seen == 15 && !disk.context()->failed());
  }
  struct failure_state { int file = 0, commit = 0, commits = 0; bool after = false; };
  struct file_ops : posix_object_ops {
    std::shared_ptr<failure_state> state;
    explicit file_ops(std::shared_ptr<failure_state> value = {}) : state(std::move(value)) {}
    std::ptrdiff_t write(int fd, std::span<std::byte const> bytes) noexcept {
      if (state && state->file == 1) { state->file = 0; errno = EIO; return -1; }
      return posix_object_ops::write(fd, bytes);
    }
    int sync_file(int fd) noexcept {
      if (state && state->file == 2) { state->file = 0; errno = EIO; return -1; }
      return posix_object_ops::sync_file(fd);
    }
  };
  struct catalog_ops {
    std::shared_ptr<failure_state> state;
    explicit catalog_ops(std::shared_ptr<failure_state> value = {}) : state(std::move(value)) {}
    int commit(sqlite3 * db) noexcept {
      auto fail = state && ++state->commits == state->commit;
      if (fail && !state->after) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return result == SQLITE_OK && fail ? SQLITE_IOERR_FSYNC : result;
    }
  };
  using faulty_family = streaming_sort_runtime_family<P, registry_selector<string_registry>, random_object_ids, catalog_ops, file_ops>;
  using faulty_core = typed_engine<P, wrapping_fingerprint_algebra, 256, faulty_family>;
  void failures() {
    for (unsigned mode = 0; mode != 16; ++mode) {
      temporary dir; auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
      auto state = std::make_shared<failure_state>();
      auto disk = faulty_family::storage_type::open(dir.root, {}, {}, catalog_ops(state), file_ops(state));
      auto context = disk.context(); faulty_core seed;
      auto active = faulty_core::from_snapshot(seed.snapshot(), disk);
      active.contribute(faulty_core::put("first", "retained"));
      auto old = active.snapshot(); auto old_indexes = context->sealed_indexes();
      if (mode < 2) state->file = int(mode) + 1;
      else { state->commit = state->commits + int((mode - 2) / 2 + 1); state->after = mode & 1; }
      rejects([&] {
        for (unsigned n = 0; n != 8; ++n) active.contribute(faulty_core::put("next", std::to_string(n)));
        drain(active);
      });
      assert(active.failed() && context->failed() && !context->sealed_outputs() && context->sealed_indexes() == old_indexes);
      assert(old.get("first") == "retained" && !old.get("next"));
      rejects([&] { active.contribute(faulty_core::put("retry", "forbidden")); });
      rejects([&] { context->make_merge(faulty_family::native_type::from_owned(sort_profile_writer<P>{}.finish()),
        faulty_family::native_type::from_owned(sort_profile_writer<P>{}.finish()), replace_native_value{}); });
      // Reopen uses a new healthy catalog handle; uncertain attempts remain for recovery.
      auto reopened = sqlite_catalog<P>::open(dir.root); assert(reopened.identity() == id(1));
    }
  }
}
int main() {
  try { owned_parity(); hidden_stages(); failures(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
