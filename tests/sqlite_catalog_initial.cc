/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks durable initial batches, retained snapshots and failed preparation or publication.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/connection.h>
#include <diet/typed_scan.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>

namespace {
  using namespace diet;
  using P = string_policy;
  using strings = unsorted<std::optional<std::string>>;
  using core = active_engine<>;
  using engine = persistent_engine<>;
  using oracle = std::map<std::string, std::string>;

  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-initial-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class E> void drain(E & current) {
    unsigned steps = 0;
    while (current.pending()) { current.advance(1'000'000); assert(++steps < 10'000); }
    assert(current.admission_ready());
  }
  oracle contents(unsigned count, bool large = false) {
    oracle result;
    for (unsigned n = 0; n != count; ++n) {
      auto key = std::string("prefix\0", 7) + std::to_string(n);
      auto value = large ? std::string(96 * 1024 + n, char('a' + n % 20)) : "value-" + std::to_string(n);
      value[value.size() / 2] = '\0';
      result.emplace(std::move(key), std::move(value));
    }
    return result;
  }
  template <class E> auto batch(oracle const & rows) {
    auto result = E::batch();
    // Exercise the public batch's sorting, rather than relying on this map's order.
    for (auto i = rows.rbegin(); i != rows.rend(); ++i) result.put(i->first, i->second);
    return std::move(result).finish();
  }
  template <class Cola> void verify(Cola const & value, oracle const & expected) {
    std::uint64_t signature = 0;
    for (auto const & [key, data] : expected) {
      assert(value.template get<strings>(key) == data);
      signature += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, data);
    }
    assert(value.live_count() == expected.size() && value.signature() == signature);
    typed_scan<strings, Cola> rows(value);
    auto next = expected.begin();
    while (auto row = rows.next()) {
      assert(next != expected.end() && row->key == next->first && row->value == next->second);
      ++next;
    }
    assert(next == expected.end());
  }
  void initialized_snapshots() {
    for (unsigned count : {3u, 16u, 65u, 128u}) {
      temporary dir;
      auto current = engine::connect(dir.root, "initial");
      auto empty = current.snapshot();
      auto expected = contents(count);
      auto full = current.contribute(batch<core>(expected));
      verify(empty, {}); verify(full, expected);
      if (std::has_single_bit(count)) assert(!current.pending() && current.admission_ready());
      else assert(current.admission_ready() || current.pending());
      assert(full.head().timeline.generation == empty.head().timeline.generation + 1);
      assert(full.metadata().clean_base == count && !full.metadata().mutations &&
        !full.metadata().rebuilding && full.runtime().admissions() == count);
      auto catalog = engine::store_type::open(dir.root);
      catalog.save("initial-save", full.head());
      auto branch = catalog.fork("branch", full.head());
      assert(branch.head.timeline.head == full.head().timeline.head && branch.head.auxiliary == full.head().auxiliary);
      auto reopened = engine::connect(dir.root, "initial", {.create_if_missing = false});
      auto reopened_full = reopened.snapshot();
      assert(reopened_full.head() == full.head());
      verify(reopened_full, expected);
      auto key = expected.begin()->first;
      expected[key] = "overwritten";
      reopened.contribute(core::put(key, expected.at(key)));
      drain(reopened);
      auto removed = std::next(expected.begin())->first;
      reopened.contribute(core::erase(removed)); expected.erase(removed);
      drain(reopened);
      verify(reopened.snapshot(), expected);
      assert(!reopened.snapshot().get(removed));
      auto history = engine::connect(dir.root, "branch", {.create_if_missing = false});
      verify(history.snapshot(), contents(count));
      auto saved = engine::store_type::open(dir.root).find_save("initial-save");
      assert(saved && saved->head == full.head());
      auto restored = core::restore_checkpoint(saved->snapshot, saved->semantic, full.metadata().schema_id);
      verify(restored, contents(count)); verify(full, contents(count)); verify(empty, {});
      for (auto const & file : std::filesystem::recursive_directory_iterator(dir.root))
        if (file.path().extension() == ".kv" || file.path().extension() == ".index")
          diet::file<P>::open(file.path()).scan();
    }
  }

  void queued_initial_batch() {
    temporary dir;
    connection<> db(dir.root, "queued");
    auto empty = db.snapshot();
    auto expected = contents(17);
    auto changes = decltype(db)::core_type::batch();
    for (auto const & [key, value] : expected) changes.put(key, value);
    auto first = db.submit(std::move(changes).finish());
    auto second = db.put_async("following", "next");
    auto initial = first.get()->cola;
    auto later = second.get()->cola;
    verify(empty, {}); verify(initial, expected);
    expected["following"] = "next"; verify(later, expected);
    db.shutdown();
    auto restored = engine::connect(dir.root, "queued", {.create_if_missing = false});
    verify(restored.snapshot(), expected);
    verify(initial, contents(17));
  }

  struct fault {
    bool publish = false, after = false, armed = false, reached = false;
    std::array<char, 32> operation{};
  };
  std::shared_ptr<fault> injection;
  struct fault_ops {
    std::shared_ptr<fault> state = injection;
    int commit(sqlite3 * db) noexcept {
      bool selected = false;
      if (state && state->armed) {
        sqlite3_stmt * row = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id,kind FROM operations ORDER BY rowid DESC LIMIT 1",
            -1, &row, nullptr) != SQLITE_OK || sqlite3_step(row) != SQLITE_ROW) std::abort();
        auto kind = reinterpret_cast<char const *>(sqlite3_column_text(row, 1));
        selected = kind && std::strcmp(kind, state->publish ? "publish_tap" : "seal") == 0;
        if (selected) {
          auto id = sqlite3_column_blob(row, 0);
          if (!id || sqlite3_column_bytes(row, 0) != int(state->operation.size())) std::abort();
          std::memcpy(state->operation.data(), id, state->operation.size());
          state->armed = false; state->reached = true;
        }
        sqlite3_finalize(row);
      }
      if (selected && !state->after) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return selected && result == SQLITE_OK ? SQLITE_IOERR_FSYNC : result;
    }
  };
  using family = streaming_sort_runtime_family<P, registry_selector<string_registry>, random_object_ids, fault_ops>;
  using faulty_core = replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, family>;
  using faulty_store = runtime_store<P, random_object_ids, fault_ops, family>;

  void acknowledgment_failures() {
    for (bool publish : {false, true}) for (bool after : {false, true}) {
      temporary dir;
      injection = std::make_shared<fault>(fault{publish, after, false, false, {}});
      auto adapter = faulty_store::create(dir.root);
      faulty_core seed;
      auto empty = seed.snapshot();
      auto before = adapter.create_tap("initial", empty.runtime(), empty.metadata().encode());
      auto current = faulty_core::from_snapshot(empty,
        family::open_storage(dir.root, empty.metadata().schema_id));
      auto expected = contents(9, true);
      injection->armed = true;
      std::string operation;
      try {
        auto built = current.contribute(batch<faulty_core>(expected));
        (void)adapter.publish(before.head, built.runtime(), built.metadata().encode());
        assert(false);
      } catch (catalog_error const & error) {
        assert(error.outcome_unknown && error.code == SQLITE_IOERR_FSYNC);
        operation = error.operation;
      }
      assert(injection->reached && !injection->armed &&
        operation == std::string(injection->operation.data(), injection->operation.size()));
      assert(publish ? adapter.failed() : current.failed());
      verify(empty, {});
      if (!publish) verify(current.snapshot(), {});
      auto catalog = sqlite_catalog<P>::open(dir.root);
      auto recorded = catalog.lookup_operation(operation);
      assert(bool(recorded) == after);
      if (recorded) assert(recorded->kind == (publish ? "publish_tap" : "seal"));
      auto healthy = engine::connect(dir.root, "initial", {.create_if_missing = false});
      auto recovered = healthy.snapshot();
      bool committed = publish && after;
      verify(recovered, committed ? expected : oracle{});
      if (committed) assert(recovered.head().timeline.generation == before.head.timeline.generation + 1);
      else assert(recovered.head() == before.head);
      drain(healthy);
      if (!committed) healthy.contribute(batch<core>(expected));
      drain(healthy); verify(healthy.snapshot(), expected);
      healthy.contribute(core::put("resumed", "healthy")); expected["resumed"] = "healthy";
      drain(healthy); verify(healthy.snapshot(), expected);
      injection.reset();
    }
  }
}
int main() {
  try { initialized_snapshots(); queued_initial_batch(); acknowledgment_failures(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
