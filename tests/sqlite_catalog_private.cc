/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks private graph sealing, scoped pin release and abandoned-writer recovery.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/active_engine.h>
#include <everett/private_construction.h>

#include <iostream>
#include <sys/wait.h>

namespace {
  using namespace everett;
  using P = string_policy;
  using core = active_engine<P>;
  using family = core::runtime_family;
  using store = runtime_store<P, random_object_ids, sqlite_catalog_ops, family>;
  void check(bool good, char const * message) { if (!good) throw std::runtime_error(message); }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-private-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
      (void)store::create(root);
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  std::uint64_t scalar(std::filesystem::path const & root, char const * sql) {
    sqlite3 * db = nullptr;
    check(sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "SQL open");
    struct connection { sqlite3 * db; ~connection() { sqlite3_close(db); } } connection{db};
    sqlite3_stmt * q = nullptr;
    check(sqlite3_prepare_v2(db, sql, -1, &q, nullptr) == SQLITE_OK, "SQL prepare");
    struct statement { sqlite3_stmt * q; ~statement() { sqlite3_finalize(q); } } statement{q};
    check(sqlite3_step(q) == SQLITE_ROW, "SQL row");
    return std::uint64_t(sqlite3_column_int64(q, 0));
  }
  constexpr char private_pins[] = "SELECT count(*) FROM live_owner_objects p JOIN attempts a ON a.owner=p.owner_id JOIN private_attempts s ON s.attempt=a.id WHERE p.owner_kind='attempt'";
  void private_publication() {
    temporary d;
    auto published = store::open(d.root);
    core initial;
    auto base = initial.snapshot();
    auto head = published.create_session("visible", base.runtime(), base.metadata().encode());
    auto scope = private_construction<P>::create(d.root);
    auto options = scope->options();
    auto writer = store::open(d.root, {}, options);
    auto engine = core::from_snapshot(base, family::open_storage(d.root, base.metadata().schema_id, options));
    auto changed = engine.contribute(core::put("private", std::optional<std::string>("value")));
    writer.prepare(changed.runtime());
    check(scalar(d.root, "SELECT count(*) FROM timeline_generations") == 1, "prepare published a generation");
    check(published.find("visible")->head == head.head, "private preparation replaced published root");
    check(scalar(d.root, private_pins) > 0, "private output has no construction owner");
    check(private_construction<P>::recover(d.root) == 0, "recovery stole a live lease");
    auto pin = scope;
    scope.reset();
    check(private_construction<P>::recover(d.root) == 0, "copied lease failed to retain ownership");
    auto next = published.publish(head.head, changed.runtime(), changed.metadata().encode());
    auto permanent = scalar(d.root, "SELECT count(*) FROM live_owner_roots WHERE owner_kind='timeline'");
    pin.reset();
    check(scalar(d.root, private_pins) == 0, "released construction still pins output");
    check(scalar(d.root, "SELECT count(*) FROM live_owner_roots WHERE owner_kind='timeline'") == permanent,
      "scope release removed a published owner");
    check(changed.get("private") == std::optional<std::string>("value"), "release damaged retained mapping");
    check(published.find("visible")->head == next.head, "release damaged named publication");
    check(scalar(d.root, "SELECT count(*) FROM objects") > 0, "release erased seal evidence");
    check(private_construction<P>::recover(d.root) == 0, "released scope recovered twice");
    bool rejected = false;
    try { (void)store::open(d.root, {}, options); } catch (std::invalid_argument const &) { rejected = true; }
    check(rejected, "closed scope accepted a new writer");
  }
  void abandoned_writer() {
    temporary d;
    auto child = ::fork();
    check(child >= 0, "fork");
    if (!child) {
      try {
        auto scope = private_construction<P>::create(d.root);
        auto options = scope->options();
        auto writer = store::open(d.root, {}, options);
        core seed;
        auto base = seed.snapshot();
        auto engine = core::from_snapshot(base, family::open_storage(d.root, base.metadata().schema_id, options));
        auto value = engine.contribute(core::put("aborted", std::optional<std::string>("value")));
        writer.prepare(value.runtime());
        ::_exit(0); // No C++ destructors: simulate lost process ownership.
      } catch (...) { ::_exit(2); }
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "child construction failed");
    check(scalar(d.root, private_pins) > 0, "crashed writer did not retain recoverable pins");
    check(scalar(d.root, "SELECT count(*) FROM timeline_generations") == 0, "crashed private writer published");
    check(private_construction<P>::recover(d.root) == 1, "abandoned scope not recovered");
    check(scalar(d.root, private_pins) == 0, "recovery left private pins active");
    check(private_construction<P>::recover(d.root) == 0, "recovery was not idempotent");
  }
}
int main() {
  try { private_publication(); abandoned_writer(); }
  catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
}
