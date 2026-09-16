/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks operation identities from a streamed executor's independent catalog connection.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/connection.h>
#include <diet/sort_runtime_context.h>

#include <array>
#include <cstring>
#include <iostream>

namespace {
  using namespace diet;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  struct fault {
    bool armed = false, after = false, reached = false;
    std::array<char, 32> operation{};
  };
  std::shared_ptr<fault> injection;
  struct catalog_ops {
    std::shared_ptr<fault> state = injection;
    int commit(sqlite3 * db) noexcept {
      if (!state || !state->armed) return sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      sqlite3_stmt * row = nullptr;
      if (sqlite3_prepare_v2(db, "SELECT id,kind FROM operations ORDER BY rowid DESC LIMIT 1",
          -1, &row, nullptr) != SQLITE_OK || sqlite3_step(row) != SQLITE_ROW)
        std::abort();
      auto kind = reinterpret_cast<char const *>(sqlite3_column_text(row, 1));
      bool selected = kind && std::strcmp(kind, "seal") == 0;
      if (selected) {
        auto id = sqlite3_column_blob(row, 0);
        if (!id || sqlite3_column_bytes(row, 0) != int(state->operation.size())) std::abort();
        std::memcpy(state->operation.data(), id, state->operation.size());
        state->armed = false;
        state->reached = true;
      }
      sqlite3_finalize(row);
      if (selected && !state->after) return SQLITE_IOERR_FSYNC;
      int result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return selected && result == SQLITE_OK ? SQLITE_IOERR_FSYNC : result;
    }
  };
  using P = string_policy;
  using strings = unsorted<std::optional<std::string>>;
  // These diagnostics deliberately fail the executor's separate connection.
  // Small adaptive outputs seal through the publication adapter instead, so
  // force streaming here to retain that distinct operation-identity boundary.
  struct eager_storage : sort_file_runtime_storage<P, registry_selector<string_registry>, random_object_ids, catalog_ops> {
    using base = sort_file_runtime_storage<P, registry_selector<string_registry>, random_object_ids, catalog_ops>;
    using base::base;
    static eager_storage open(std::filesystem::path const & root) {
      return eager_storage(context_type::open(root, {}, {}, {}, {}, {0, 0}));
    }
  };
  using family = sort_runtime_family<P, registry_selector<string_registry>, eager_storage>;
  using core = typed_engine<P, wrapping_fingerprint_algebra, 256, family>;
  using engine = persistent_engine<core>;
  using store = engine::store_type;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-stream-diagnostic-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  };
  std::uint64_t hash(std::string const & key, std::string const & value) {
    return sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, value);
  }
  template <class E> void drain(E & active) {
    unsigned steps = 0;
    while (active.pending()) {
      active.advance(1'000'000);
      check(++steps < 1000, "service stalled");
    }
  }
  void prepare_pending(std::filesystem::path const & root) {
    auto saved = store::create(root);
    core seed;
    auto initial = seed.snapshot();
    auto metadata = initial.metadata();
    auto runtime = core::runtime_type::from_snapshot(initial.runtime(), family::open_storage(root));
    for (auto const * key : {"first", "second"}) {
      while (!runtime.admission_ready()) runtime.advance(1'000'000);
      auto input = core::put(key, "retained");
      check(bool(runtime.try_contribute(input.records().front(), 0)), "ready admission failed");
      ++metadata.live_count;
      metadata.signature += hash(key, "retained");
    }
    check(runtime.pending(), "fixture has no deferred streamed work");
    saved.create_tap("latest", runtime.checkpoint(), metadata.encode());
  }
  void run_case(bool service, bool after) {
    temporary dir;
    injection = std::make_shared<fault>();
    if (service) prepare_pending(dir.root);
    auto active = engine::connect(dir.root, "latest");
    if (!service) {
      active.contribute(core::put("first", "retained"));
      drain(active);
      check(!active.last_operation().empty(), "successful publication omitted its operation");
    }
    auto old = active.snapshot();
    auto previous_operation = active.last_operation();
    injection->after = after;
    injection->armed = true;
    std::string reported;
    bool caught = false;
    try {
      if (service) drain(active);
      else active.contribute(core::put("second", "unpublished"));
    } catch (catalog_error const & error) {
      caught = true;
      reported = error.operation;
      check(error.code == SQLITE_IOERR_FSYNC && error.outcome_unknown, "catalog error evidence changed");
      check(active.last_operation() == error.operation, "engine reported another catalog's operation");
    }
    check(caught && injection->reached && !injection->armed, "streamed seal cut was not reached");
    check(reported == std::string(injection->operation.data(), injection->operation.size()) &&
      reported != previous_operation, "streamed operation was not distinct from publication");
    auto failed = active.snapshot();
    check(active.failed() && failed.head() == old.head(), "failed executor published a partial state");
    // The caller's exception is gone. The diagnostic must survive both that
    // lifetime and moving the poisoned engine, without changing the exception.
    auto moved = std::move(active);
    check(moved.last_operation() == reported && moved.failed(), "diagnostic lifetime or move");
    bool refused = false;
    try { moved.advance(1); }
    catch (std::logic_error const &) { refused = true; }
    check(refused && moved.last_operation() == reported, "poisoned retry erased diagnostic");

    auto catalog = sqlite_catalog<P>::open(dir.root);
    auto operation = catalog.lookup_operation(reported);
    check(bool(operation) == after, "reported operation does not reconcile with actual COMMIT");
    if (operation) check(operation->kind == "seal", "reported operation is not the failing seal");
    injection.reset();
    auto healthy = engine::connect(dir.root, "latest", {.create_if_missing = false});
    auto reopened = healthy.snapshot();
    check(reopened.head() == old.head() && reopened.signature() == old.signature() &&
      reopened.live_count() == old.live_count(), "uncertain seal changed the durable root");
    check(reopened.get("first") == "retained" &&
      reopened.get("second") == (service ? std::optional<std::string>("retained") : std::nullopt),
      "reopened contents disagree with acknowledged state");
    drain(healthy);
    healthy.contribute(core::put("resumed", "healthy"));
    check(healthy.snapshot().get("resumed") == "healthy", "fresh connection could not resume");
  }
}

int main() {
  try {
    for (bool service : {false, true})
      for (bool after : {false, true}) run_case(service, after);
    std::cout << "streamed catalog failure diagnostics passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
