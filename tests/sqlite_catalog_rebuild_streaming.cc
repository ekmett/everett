/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks shared streaming storage through replacement cleanup and durable recovery.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/replacement_rebuild.h>
#include <everett/connection.h>
#include <everett/sort_runtime_context.h>

#include <iostream>
#include <map>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using P = storage_policy<string_registry, 3>;

  struct fault {
    unsigned commits = 0, fail_commit = 0, syncs = 0;
    bool fail_sync = false, after_commit = false;
  };
  // Only the concrete execution context uses these operations. The durable
  // publication adapter retains its ordinary, independently opened catalog.
  std::shared_ptr<fault> injection;
  struct file_ops : posix_object_ops {
    std::shared_ptr<fault> state = injection;
    int sync_file(int fd) noexcept {
      if (state) {
        ++state->syncs;
        if (state->fail_sync) {
          state->fail_sync = false;
          errno = EIO;
          return -1;
        }
      }
      return posix_object_ops::sync_file(fd);
    }
  };
  struct catalog_ops {
    std::shared_ptr<fault> state = injection;
    int commit(sqlite3 * db) noexcept {
      bool fail = state && ++state->commits == state->fail_commit;
      if (fail && !state->after_commit) return SQLITE_IOERR_FSYNC;
      int result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return result == SQLITE_OK && fail ? SQLITE_IOERR_FSYNC : result;
    }
  };
  using family = streaming_sort_runtime_family<P, registry_selector<string_registry>,
    random_object_ids, catalog_ops, file_ops>;
  using storage = family::storage_type;
  using core = replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, family>;
  using typed_core = core::engine_type;
  using engine = persistent_engine<core>;
  using store = engine::store_type;
  // Faults in the execution context must reach its own catalog/FileOps rather
  // than the publication adapter that seals small adaptive outputs.
  struct eager_storage : storage {
    using storage::storage;
    static eager_storage open(std::filesystem::path const & root) {
      return eager_storage(context_type::open(root, {}, {}, {}, {}, {0, 0}));
    }
  };
  using eager_family = sort_runtime_family<P, registry_selector<string_registry>, eager_storage>;
  using eager_core = replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, eager_family>;
  using eager_engine = persistent_engine<eager_core>;

  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "everett-rebuild-stream-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  };
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool caught = false;
    try { action(); }
    catch (std::exception const &) { caught = true; }
    check(caught, "expected rejection");
  }
  template <class E> void drain(E & active) {
    unsigned rounds = 0;
    while (active.pending()) {
      active.advance(1'000'000);
      check(++rounds < 10'000, "cleanup did not settle");
    }
    check(active.admission_ready(), "settled admission gate");
  }
  template <class World> void verify(World const & state,
      std::map<std::string, std::string> const & expected, bool all_mapped = false) {
    check(state.live_count() == expected.size(), "live count");
    std::uint64_t signature = 0;
    for (auto const & [key, value] : expected) {
      check(state.get(key) == value, "resolved value");
      signature += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, value);
    }
    check(signature == state.signature(), "independent signature");
    auto rows = everett::scan(state);
    auto entry = expected.begin();
    while (auto row = rows.next()) {
      check(entry != expected.end() && row->key == entry->first && row->value == entry->second,
        "resolved scan order or contents");
      ++entry;
    }
    check(entry == expected.end(), "resolved scan omitted row");
    auto const & meta = state.metadata();
    check(meta.clean_base + meta.mutations == state.runtime().admissions(), "generation mass");
    for (auto const & run : state.runtime().runs())
      if (all_mapped) check(run->native->mapped() && !run->native->owned(), "unmapped durable output");
  }
  std::string key(unsigned n) { return std::string("key\0", 4) + std::to_string(n); }

  void context_and_recovery() {
    temporary dir;
    injection = std::make_shared<fault>();
    auto catalog = store::create(dir.root);
    // Only this private candidate/failure phase requires eager output. Named
    // reopen and recovery below use the ordinary adaptive storage defaults.
    auto disk = storage::open(dir.root, {}, {}, {}, {}, {0, 0});
    auto context = disk.context();
    typed_core empty;
    auto initial = typed_core::from_snapshot(empty.snapshot(), disk);
    std::map<std::string, std::string> expected;
    auto batch = typed_core::batch();
    // Stay above the bounded tiny path so restart exercises partial streamed cleanup.
    for (unsigned n = 0; n != 128; ++n) {
      auto value = std::string(257, char('a' + n % 26));
      expected[key(n)] = value;
      batch.put(key(n), value);
    }
    initial.contribute(std::move(batch).finish());
    drain(initial);
    auto active = core::from_clean(initial.snapshot(), disk);
    check(active.storage().context() == context, "restore changed context identity");
    auto old = active.snapshot();
    verify(old, expected);
    auto charges = active.work().foreground_charged;
    active.rebase(old);
    check(active.storage().context() == context && active.work().foreground_charged > charges,
      "rebase dropped context or initialization charge");
    rejects([&] { active.rebase(core{}.snapshot()); });
    check(!active.failed() && !context->failed(), "healthy rebase rejection poisoned context");

    unsigned mutations = 0;
    while (!active.snapshot().metadata().rebuilding) {
      while (!active.admission_ready()) active.advance(100'000);
      active.contribute(core::put(key(0), "at-freeze"));
      expected[key(0)] = "at-freeze";
      check(++mutations <= 32, "large trigger missed");
    }
    active.contribute(core::put(key(1), "queued"));
    expected[key(1)] = "queued";
    auto saved = active.snapshot();
    verify(saved, expected);
    check(saved.metadata().rebuilding, "active marker lost before restart");
    drain(active);
    check(active.storage().context() == context && !active.snapshot().metadata().rebuilding,
      "successful handoff changed context");
    active.rebase(active.snapshot());
    check(active.storage().context() == context, "post-handoff rebase changed context");
    verify(active.snapshot(), expected);
    auto before_seals = context->sealed_outputs();
    auto restored = core::from_snapshot(saved, disk);
    check(restored.storage().context() == context && !restored.admission_ready(), "recovery context/gate");
    check(restored.work().scan_records == 0, "restore scanned payload eagerly");
    rejects([&] { restored.contribute(core::put("blocked", "blocked")); });
    check(!restored.failed(), "recovery refusal poisoned context");
    unsigned steps = 0;
    while (context->sealed_outputs() == before_seals) {
      restored.advance(100'000);
      check(restored.pending() && ++steps < 1000, "no partial streamed candidate");
    }
    check(restored.snapshot().metadata() == saved.metadata(), "partial cleanup changed generation");
    check(restored.storage().context() == context, "candidate changed context");
    auto acknowledged = restored.snapshot();
    injection->fail_sync = true;
    rejects([&] {
      for (unsigned n = 0; n != 1000 && restored.pending(); ++n) restored.advance(100'000);
    });
    auto failed_snapshot = restored.snapshot();
    check(restored.failed() && context->failed() &&
      failed_snapshot.runtime().same_layout(acknowledged.runtime()) &&
      failed_snapshot.metadata() == acknowledged.metadata(), "candidate failure escaped poison/publication boundary");
    // Persist only the acknowledged foreground. Losing the candidate is an
    // explicit restart, without serialized private cursors or replay queues.
    auto recorded = catalog.create_session("latest", saved.runtime(), saved.metadata().encode());
    catalog.save("active-save", recorded.head);
    catalog.fork("active-fork", recorded.head);
    restored = core{};
    active = core{};
    initial = typed_core{};
    context.reset();
    disk = storage{};
    injection.reset();

    std::optional<engine> durable(engine::connect(dir.root, "latest", {.create_if_missing = false}));
    check(!durable->admission_ready() && durable->snapshot().metadata() == saved.metadata(), "durable recovery gate");
    verify(durable->snapshot(), expected, true);
    durable->advance(100'000);
    check(durable->pending(), "recovery fixture not interrupted");
    durable.reset();
    durable.emplace(engine::connect(dir.root, "latest", {.create_if_missing = false}));
    check(!durable->admission_ready(), "second recovery lost gate");
    drain(*durable);
    auto cleaned = durable->snapshot();
    verify(cleaned, expected, true);
    check(!cleaned.metadata().rebuilding && cleaned.metadata().clean_base == 128 &&
      !cleaned.metadata().mutations, "recovery handoff counters");
    check(cleaned.head().timeline.generation > recorded.head.timeline.generation, "handoff was not durable");
    // A later merge uses the context retained by settled durable rebasing.
    for (unsigned n = 0; n != 4; ++n) {
      while (!durable->admission_ready()) durable->advance(100'000);
      durable->contribute(core::put(key(n), "after-handoff"));
      expected[key(n)] = "after-handoff";
    }
    drain(*durable);
    verify(durable->snapshot(), expected, true);
    durable.reset();
    auto reopened = engine::connect(dir.root, "latest", {.create_if_missing = false});
    verify(reopened.snapshot(), expected, true);
    auto branch = engine::connect(dir.root, "active-fork", {.create_if_missing = false});
    check(!branch.admission_ready() && branch.snapshot().metadata() == saved.metadata(), "fork lost active debt");
    drain(branch);
    check(branch.snapshot().get(key(1)) == "queued", "fork recovery contents");
    check(old.get(key(0)) == std::string(257, 'a') && saved.get(key(1)) == "queued", "old snapshot pins");
  }

  void failures() {
    for (unsigned mode = 0; mode != 3; ++mode) {
      temporary dir;
      injection = std::make_shared<fault>();
      auto durable = eager_engine::connect(dir.root, "stable");
      durable.contribute(eager_core::put("first", "retained"));
      auto old = durable.snapshot();
      // The batch's foreground or cleanup merge must fail before its new
      // logical state is acknowledged. The earlier test isolates the candidate.
      if (mode == 0) injection->fail_sync = true;
      else {
        injection->fail_commit = injection->commits + 2; // reserve, then sealed receipt
        injection->after_commit = mode == 2;
      }
      rejects([&] { durable.contribute(eager_core::put("second", "unpublished")); });
      auto failed_snapshot = durable.snapshot();
      check(durable.failed() && failed_snapshot.head() == old.head(), "failed cleanup published partial batch");
      check(injection->syncs, "candidate never reached file sealing");
      rejects([&] { durable.contribute(eager_core::put("retry", "forbidden")); });
      injection.reset();
      auto healthy = engine::connect(dir.root, "stable", {.create_if_missing = false});
      auto reopened = healthy.snapshot();
      check(reopened.head() == old.head() && reopened.get("first") == "retained" &&
        !reopened.get("second"), "uncertain seal changed durable state");
      healthy.contribute(core::put("second", "healthy"));
      check(healthy.snapshot().get("second") == "healthy", "fresh context retry failed");
    }
    temporary dir;
    auto catalog = store::create(dir.root);
    auto disk = storage::open(dir.root);
    core empty;
    auto active = core::from_snapshot(empty.snapshot(), disk);
    active.contribute(core::put("kept", "value"));
    auto before = active.snapshot();
    active.poison();
    check(active.failed() && disk.context()->failed() && !active.admission_ready(), "public poison did not reach storage");
    check(before.get("kept") == "value", "poison invalidated snapshot");
    rejects([&] { active.advance(1); });
  }
}

int main() {
  try {
    context_and_recovery();
    failures();
    std::cout << "streaming replacement recovery passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
