/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests durable timeline generation selection, exact CAS, historical replay and old catalogs.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/sqlite_catalog.h>

#include <array>
#include <barrier>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using namespace everett;
  using policy = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 3, exponential_golomb<0>, 4>;
  using catalog = sqlite_catalog<policy>;
  void require(bool condition, char const * message) { if (!condition) throw std::runtime_error(message); }
  template<class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "expected rejection");
  }
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  object_attempt_id attempt(unsigned n) { return object_attempt_id(id(n).hex()); }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto text = (std::filesystem::temp_directory_path() / "everett-timeline-XXXXXX").string();
      auto result = ::mkdtemp(text.data());
      if (!result) throw std::runtime_error("mkdtemp");
      root = result;
    }
    ~temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
  };
  struct sql_connection {
    sqlite3 * db = nullptr;
    explicit sql_connection(std::filesystem::path const & root) {
      auto result = sqlite3_open((root / "catalog.sqlite3").c_str(), &db);
      if (result != SQLITE_OK) throw std::runtime_error("open test SQL connection");
    }
    ~sql_connection() { sqlite3_close(db); }
    void exec(char const * sql) { require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "test SQL failed"); }
    std::int64_t scalar(char const * sql) {
      sqlite3_stmt * statement = nullptr;
      require(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK, "prepare test query");
      auto code = sqlite3_step(statement);
      auto result = sqlite3_column_int64(statement, 0);
      sqlite3_finalize(statement); require(code == SQLITE_ROW, "missing test scalar"); return result;
    }
  };
  std::int64_t count(std::filesystem::path const & root, char const * sql) { return sql_connection(root).scalar(sql); }
  template<class P> struct graph {
    blob_identity head, tail;
    std::vector<profile_record> records;
  };
  template<class P> graph<P> persist(sqlite_catalog<P> & db, unsigned seed) {
    std::vector<profile_record> records;
    for (unsigned i = 0; i != 19; ++i) {
      char text[16]; std::snprintf(text, sizeof text, "%04u:%04u", seed, i);
      auto key = P::unit == profile_unit::byte ? bit_string::from_bytes(text) : bit_string::from_bits(std::string(i + 1, '1'));
      auto value = P::fixed_width ? bit_string{} : bit_string::from_bytes(text);
      records.push_back({std::move(key), std::move(value)});
    }
    auto pair = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(records));
    auto prepared = query_root<P>::build(pair);
    std::vector<std::shared_ptr<profile_blob<P> const>> chain;
    std::vector<blob_identity> identities;
    std::vector<catalog_object_reservation> outputs;
    for (auto current = prepared.head(); current; current = current->target()) {
      auto n = static_cast<unsigned>(chain.size()); chain.push_back(current);
      identities.push_back({id(seed + n * 2), id(seed + n * 2 + 1)});
      outputs.push_back({identities.back().native, file_kind::native_blob});
      outputs.push_back({identities.back().index, file_kind::fractional_index});
    }
    auto op = std::to_string(seed), owner = "fixture-" + op;
    db.reserve("reserve-" + op, attempt(seed + 99), owner, {}, outputs);
    for (std::size_t i = chain.size(); i-- != 0;) {
      auto native = encode_native_sections(chain[i]->native());
      auto index = encode_index_sections(*chain[i], identities[i].native,
        i + 1 < chain.size() ? std::optional{identities[i + 1]} : std::nullopt);
      db.record_sealed("native-" + op + "-" + std::to_string(i), native.seal(db.root(), identities[i].native, attempt(seed + 99)));
      db.record_sealed("index-" + op + "-" + std::to_string(i), index.seal(db.root(), identities[i].index, attempt(seed + 99)));
    }
    auto mapped = open_mapped_query<P>(db.root(), identities.front());
    db.register_chain("register-" + op, mapped, catalog_admission::scan);
    return {identities.front(), identities.back(), std::move(records)};
  }
  template<class P> void query_graph(std::filesystem::path const & root, blob_identity const & head, graph<P> const & source) {
    auto query = open_mapped_query<P>(root, head);
    for (auto const & record : source.records) {
      auto cursor = query.cursor(record.key.view());
      unsigned matches = 0, steps = 0;
      while (!cursor.done()) {
        require(++steps < 1000, "timeline query did not terminate"); cursor.step(1);
        if (cursor.has_match()) {
          auto match = cursor.take_match(); ++matches;
          require(compare_bits(match.value.view(), record.value.view()) == 0, "published value differs from full-key oracle");
        }
      }
      require(matches == 1, "published graph lost or duplicated a key");
    }
  }
  void retained(std::filesystem::path const & root, std::int64_t generations) {
    sql_connection sql(root);
    require(sql.scalar("SELECT count(*) FROM timeline_generations") == generations, "wrong generation count");
    require(sql.scalar("SELECT count(*) FROM owners WHERE kind='timeline'") == generations, "wrong immutable generation owners");
    require(sql.scalar("SELECT count(*) FROM owner_roots WHERE owner_kind='timeline'") == generations, "wrong generation pins");
    require(sql.scalar("SELECT count(*) FROM timeline_generations g JOIN owner_roots r ON r.owner_kind=g.owner_kind AND r.owner_id=g.owner_id AND r.native_id=g.native_id AND r.index_id=g.index_id") == generations,
      "generation lost its exact root pin");
    require(sql.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0, "catalog foreign-key failure");
  }
  void lifecycle() {
    temporary directory;
    auto db = catalog::create(directory.root, id(1));
    auto a = persist(db, 100), b = persist(db, 300);
    require(db.schema_version() == 2, "new catalog is not version 2");
    db.save("old-save", "saved", a.head);
    require(!db.find_timeline("unknown"), "unknown timeline found");
    using namespace std::string_literals;
    std::string name = "live\0'; DROP TABLE pairs;--\xff"s, op = "create\0\xff"s;
    auto initial = db.create_timeline(op, name, a.head);
    require(initial.name == name && initial.generation == 0 && initial.head == a.head && !initial.owner.empty(), "wrong initial timeline");
    require(db.create_timeline(op, name, a.head) == initial, "create replay differs");
    rejects([&] { db.create_timeline(op, name, b.head); });
    rejects([&] { db.create_timeline("duplicate-name", name, a.head); });
    rejects([&] { db.create_timeline("nonprepared", "bad", a.tail); });
    require(!db.lookup_operation("duplicate-name") && !db.lookup_operation("nonprepared"), "rejected operation committed");
    auto next = db.publish_timeline("publish", initial, b.head);
    require(next.published && next.head.generation == 1 && next.head.head == b.head && next.head.owner != initial.owner, "publication failed");
    auto stale = db.publish_timeline("stale", initial, a.head);
    require(!stale.published && stale.head == next.head, "stale CAS did not return observed head");
    // A same-root publication is still a new generation; root equality cannot create ABA.
    auto third = db.publish_timeline("same-root", next.head, b.head);
    require(third.published && third.head.generation == 2, "same-root publication did not advance generation");
    auto fourth = db.publish_timeline("back-to-first", third.head, a.head);
    require(fourth.published && fourth.head.generation == 3, "ABA fixture failed");
    require(!db.publish_timeline("stale-after-aba", initial, b.head).published, "generation check allowed ABA");
    require(db.publish_timeline("publish", initial, b.head) == next, "successful replay followed current head");
    require(db.publish_timeline("stale", initial, a.head) == stale, "stale replay followed current head");
    require(db.create_timeline(op, name, a.head) == initial, "creation replay followed current head");
    auto fork = db.fork_timeline("fork", std::string("fork\0\xfe", 6), next.head);
    require(fork.head == b.head && fork.generation == 0, "fork did not select historical generation");
    require(db.fork_timeline("fork", fork.name, next.head) == fork, "fork replay differs");
    auto forked_next = db.publish_timeline("fork-publish", fork, a.head);
    require(forked_next.published && db.find_timeline(name) == fourth.head, "fork changed source timeline");
    require(db.fork_timeline("fork", fork.name, next.head) == fork, "fork replay followed fork's current head");
    auto forged = next.head; forged.owner.push_back('x');
    rejects([&] { db.fork_timeline("bad-owner", "forged", forged); });
    forged = fourth.head; forged.owner.push_back('x');
    auto owner_conflict = db.publish_timeline("owner-conflict", forged, a.head);
    require(!owner_conflict.published && owner_conflict.head == fourth.head, "CAS ignored owner");
    forged = fourth.head; forged.head = b.head;
    require(!db.publish_timeline("identity-conflict", forged, b.head).published, "CAS ignored exact object pair");
    rejects([&] { db.fork_timeline("bad-root", "forged", forged); });
    forged = fourth.head; ++forged.generation;
    rejects([&] { db.fork_timeline("bad-generation", "forged", forged); });
    forged = fourth.head; forged.name += "unknown";
    rejects([&] { db.fork_timeline("bad-name", "forged", forged); });
    rejects([&] { db.publish_timeline("missing-name", forged, a.head); });
    forged = fourth.head; forged.generation = std::numeric_limits<std::uint64_t>::max();
    rejects([&] { db.publish_timeline("overflow-generation", forged, a.head); });
    rejects([&] { db.publish_timeline("publish", initial, a.head); });
    rejects([&] { db.fork_timeline("publish", "kind-mismatch", initial); });
    rejects([&] { db.publish_timeline("bad-candidate", fourth.head, a.tail); });
    rejects([&] { db.publish_timeline("missing-candidate", fourth.head, {id(900), id(901)}); });
    require(!db.poisoned() && !db.lookup_operation("bad-candidate"), "ordinary rejection poisoned or committed");
    retained(directory.root, 6);
    {
      auto reopened = catalog::open(directory.root);
      require(reopened.find_timeline(name) == fourth.head, "reopen lost current generation");
      require(reopened.find_timeline(fork.name) == forked_next.head, "reopen lost fork head");
      require(reopened.find_save("saved") == a.head, "timeline publication changed immutable save");
      require(reopened.publish_timeline("stale", initial, a.head) == stale, "reopen stale reconciliation differs");
      query_graph(directory.root, initial.head, a); query_graph(directory.root, next.head.head, b);
      query_graph(directory.root, reopened.find_timeline(name)->head, a);
    }
    auto moved = std::move(db);
    rejects([&] { db.find_timeline(name); });
    require(moved.find_timeline(name) == fourth.head, "moved catalog lost timeline");
    for (auto const & value : {std::string{}, std::string("\0", 1)}) {
      if (value.empty()) rejects([&] { moved.create_timeline("empty", value, a.head); });
      else require(moved.create_timeline("nul-name", value, a.head).name == value, "one-NUL name was truncated");
    }
  }
  void contention() {
    temporary directory;
    auto db = catalog::create(directory.root, id(1));
    auto a = persist(db, 100), b = persist(db, 300);
    auto initial = db.create_timeline("create", "contended", a.head);
    std::array<std::optional<catalog_timeline_publication>, 2> results;
    std::array<std::exception_ptr, 2> errors;
    std::barrier ready(2);
    auto connection_a = catalog::open(directory.root, {5000});
    auto connection_b = catalog::open(directory.root, {5000});
    auto run = [&](unsigned i) {
      try {
        auto & connection = i ? connection_b : connection_a;
        ready.arrive_and_wait();
        results[i] = connection.publish_timeline("race-" + std::to_string(i), initial, i ? a.head : b.head);
      } catch (...) { errors[i] = std::current_exception(); }
    };
    std::thread first(run, 0), second(run, 1); first.join(); second.join();
    for (auto error : errors) if (error) std::rethrow_exception(error);
    require(results[0] && results[1] && results[0]->published != results[1]->published, "two stale writers both succeeded or failed");
    require(results[0]->head == results[1]->head, "losing writer did not observe winner");
    retained(directory.root, 2);
    auto winner = results[0]->head;
    auto latest = db.publish_timeline("later", winner, a.head);
    require(latest.published, "publication after race failed");
    for (unsigned i = 0; i != 2; ++i)
      require(db.publish_timeline("race-" + std::to_string(i), initial, i ? a.head : b.head) == *results[i], "race replay changed its outcome");
  }
  struct fault_ops {
    std::shared_ptr<int> mode;
    int commit(sqlite3 * db) noexcept {
      int chosen = std::exchange(*mode, 0);
      if (chosen == 1) return SQLITE_IOERR_FSYNC;
      int code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return code == SQLITE_OK && chosen == 2 ? SQLITE_IOERR_FSYNC : code;
    }
  };
  void ambiguous_commits() {
    temporary directory;
    auto db = catalog::create(directory.root, id(1));
    auto a = persist(db, 100), b = persist(db, 300);
    for (int mode : {1, 2}) for (unsigned operation = 0; operation != 4; ++operation) {
      auto suffix = std::to_string(mode) + "-" + std::to_string(operation);
      auto before = db.create_timeline("setup-" + suffix, "source-" + suffix, a.head);
      auto expected = before;
      if (operation == 3) before = db.publish_timeline("setup-publish-" + suffix, before, b.head).head;
      auto invoke = [&](auto & connection) -> catalog_timeline_publication {
        auto op = "uncertain-" + suffix;
        if (operation == 0) return {true, connection.create_timeline(op, "new-" + suffix, b.head)};
        if (operation == 1) return {true, connection.fork_timeline(op, "fork-" + suffix, expected)};
        return connection.publish_timeline(op, expected, b.head);
      };
      auto owners_before = count(directory.root, "SELECT count(*) FROM owners WHERE kind='timeline'");
      {
        auto fault = sqlite_catalog<policy, fault_ops>::open(directory.root, {}, {std::make_shared<int>(mode)});
        bool failed = false;
        try { (void)invoke(fault); }
        catch (catalog_error const & error) {
          failed = true; require(error.outcome_unknown && error.operation == "uncertain-" + suffix && error.code == SQLITE_IOERR_FSYNC, "wrong ambiguous commit evidence");
        }
        require(failed && fault.poisoned(), "COMMIT error did not poison handle");
        rejects([&] { fault.find_timeline(before.name); });
        rejects([&] { (void)invoke(fault); });
      }
      auto reopened = catalog::open(directory.root);
      require(bool(reopened.lookup_operation("uncertain-" + suffix)) == (mode == 2), "operation presence disagrees with actual COMMIT");
      auto expected_delta = mode == 2 && operation != 3 ? 1 : 0;
      require(count(directory.root, "SELECT count(*) FROM owners WHERE kind='timeline'") == owners_before + expected_delta, "partial or missing generation owner");
      // Move the source beyond either captured outcome before reconciling.
      auto current = *reopened.find_timeline(before.name);
      auto changed = reopened.publish_timeline("intervening-" + suffix, current, a.head);
      require(changed.published, "intervening publication failed");
      auto result = invoke(reopened);
      auto replay = invoke(reopened);
      require(result == replay, "reconciliation not idempotent");
      if (operation < 2) require(result.published && result.head.generation == 0, "create/fork reconciliation followed current source");
      else if (mode == 1) require(!result.published && result.head == changed.head, "uncommitted operation did not compare fresh state");
      else if (operation == 2) require(result.published && result.head.generation == 1 && result.head.head == b.head, "committed publication was not recovered");
      else require(!result.published && result.head == before, "committed stale outcome was not recovered");
    }
  }
  void metadata_only() {
    temporary directory;
    auto db = catalog::create(directory.root, id(1));
    auto a = persist(db, 100), b = persist(db, 300);
    // No timeline operation should open, scan or copy any immutable payload.
    // Hide the whole object shard after registration so even a header-only
    // reopen would fail. Restore it before reading the published query.
    auto visible = directory.root / "00", hidden = directory.root / "hidden-payloads";
    std::filesystem::rename(visible, hidden);
    auto initial = db.create_timeline("create", "metadata-only", a.head);
    auto next = db.publish_timeline("publish", initial, b.head);
    auto fork = db.fork_timeline("fork", "metadata-fork", initial);
    require(next.published && db.find_timeline(initial.name) == next.head && fork.head == a.head,
      "timeline metadata operation read unavailable payload");
    require(db.publish_timeline("publish", initial, b.head) == next, "metadata-only replay failed");
    std::filesystem::rename(hidden, visible);
    query_graph(directory.root, next.head.head, b);
  }
  struct restart_ops {
    int notify;
    bool after;
    int commit(sqlite3 * db) noexcept {
      if (after) {
        auto code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (code != SQLITE_OK) return code;
      }
      char message = 'C';
      ssize_t sent;
      do { sent = ::write(notify, &message, 1); } while (sent < 0 && errno == EINTR);
      if (sent != 1) return SQLITE_IOERR;
      for (;;) ::pause(); // The parent kills this process at the selected cut.
    }
  };
  struct child_guard {
    pid_t pid;
    ~child_guard() {
      if (pid <= 0) return;
      ::kill(pid, SIGKILL);
      int status;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
  };
  void process_restarts() {
    temporary directory;
    std::optional<graph<policy>> a, b;
    {
      auto db = catalog::create(directory.root, id(1));
      a = persist(db, 100); b = persist(db, 300);
      db.save("old-save", "old-save", a->head);
    }
    for (bool after : {false, true}) for (unsigned operation = 0; operation != 4; ++operation) {
      auto suffix = std::to_string(after) + "-" + std::to_string(operation);
      std::optional<catalog_timeline_head> expected, before;
      {
        auto db = catalog::open(directory.root);
        expected = db.create_timeline("setup-" + suffix, "source-" + suffix, a->head);
        before = expected;
        if (operation == 3) before = db.publish_timeline("setup-publish-" + suffix, *expected, b->head).head;
      }
      auto invoke = [&](auto & db) -> catalog_timeline_publication {
        auto op = "cut-" + suffix;
        if (operation == 0) return {true, db.create_timeline(op, "created-" + suffix, b->head)};
        if (operation == 1) return {true, db.fork_timeline(op, "forked-" + suffix, *expected)};
        return db.publish_timeline(op, *expected, b->head);
      };
      auto pins_before = count(directory.root, "SELECT count(*) FROM timeline_generations");
      int pipe[2]; require(::pipe(pipe) == 0, "restart pipe");
      auto pid = ::fork();
      if (pid < 0) { ::close(pipe[0]); ::close(pipe[1]); throw std::runtime_error("fork"); }
      if (pid == 0) {
        ::close(pipe[0]);
        try {
          auto db = sqlite_catalog<policy, restart_ops>::open(directory.root, {}, {pipe[1], after});
          (void)invoke(db);
        } catch (...) { ::_exit(90); }
        ::_exit(91);
      }
      child_guard child{pid}; ::close(pipe[1]);
      pollfd watched{pipe[0], POLLIN, 0};
      int ready;
      do { ready = ::poll(&watched, 1, 10000); } while (ready < 0 && errno == EINTR);
      char message = 0;
      auto received = ready > 0 ? ::read(pipe[0], &message, 1) : -1;
      ::close(pipe[0]);
      require(received == 1 && message == 'C', "child did not reach bounded COMMIT cut");
      require(::kill(pid, SIGKILL) == 0, "kill restart child");
      int status = 0; pid_t reaped;
      do { reaped = ::waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
      require(reaped == pid, "reap restart child"); child.pid = -1;
      require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "restart child exited unexpectedly");
      {
        auto db = catalog::open(directory.root);
        require(bool(db.lookup_operation("cut-" + suffix)) == after, "process cut lost exact commit boundary");
        retained(directory.root, pins_before + (after && operation != 3 ? 1 : 0));
        require(db.find_save("old-save") == a->head, "process cut lost old immutable save");
        query_graph(directory.root, a->head, *a);
        auto outcome = invoke(db);
        require(invoke(db) == outcome, "restart replay duplicated generation");
        if (operation < 2) require(outcome.published && outcome.head.generation == 0, "wrong restarted create/fork outcome");
        else if (operation == 2) require(outcome.published && outcome.head.generation == 1, "wrong restarted publication outcome");
        else require(!outcome.published && outcome.head == *before, "wrong restarted stale CAS outcome");
        query_graph(directory.root, outcome.head.head, operation == 1 ? *a : *b);
      }
      // A second complete close/reopen must preserve the reconciled operation.
      { auto db = catalog::open(directory.root); require(db.lookup_operation("cut-" + suffix).has_value(), "second reopen lost reconciled operation"); }
    }
  }
  void legacy() {
    temporary directory;
    {
      sql_connection sql(directory.root);
      // The frozen version-1 schema is a supported read/write compatibility fixture.
      // Original catalog creation enabled WAL before installing this schema;
      // opening an existing file deliberately does not change its journal mode.
      sql.exec("PRAGMA journal_mode=WAL");
      sql.exec(catalog_detail::schema);
      catalog_detail::bytes descriptor;
      for (auto value : {std::uint64_t(policy::unit), policy::group_size, policy::codec_block_size,
          std::uint64_t(policy::backspace_code), policy::backspace_parameter,
          std::uint64_t(policy::fixed_width), policy::value_width.value_or(0)}) catalog_detail::number(descriptor, value);
      catalog_detail::statement insert(sql.db, "INSERT INTO catalog_info VALUES(1,1,?,?)");
      insert.text(1, id(1).hex()); insert.blob(2, descriptor); insert.done();
      for (auto table : {"catalog_info", "operations", "owners", "attempts", "pairs", "owner_objects", "owner_roots", "saves"})
        for (auto action : {"UPDATE", "DELETE"}) {
          auto definition = "CREATE TRIGGER immutable_" + std::string(table) + "_" + action + " BEFORE " + action + " ON " + table + " BEGIN SELECT RAISE(ABORT,'immutable catalog row'); END";
          sql.exec(definition.c_str());
        }
      sql.exec("CREATE TRIGGER objects_no_delete BEFORE DELETE ON objects BEGIN SELECT RAISE(ABORT,'retained object'); END");
    }
    auto db = catalog::open(directory.root);
    require(db.schema_version() == 1, "legacy version changed");
    auto a = persist(db, 100);
    db.save("save", "legacy", a.head);
    require(db.acquire_save("acquire", "legacy", "reader").head == a.head, "legacy save API failed");
    auto fake = catalog_timeline_head{"legacy", 0, a.head, "owner"};
    rejects([&] { db.find_timeline("legacy"); });
    rejects([&] { db.create_timeline("new", "new", a.head); });
    rejects([&] { db.fork_timeline("fork", "fork", fake); });
    rejects([&] { db.publish_timeline("publish", fake, a.head); });
    require(!db.poisoned() && !db.lookup_operation("new"), "legacy capability rejection changed catalog");
    auto reopened = catalog::open(directory.root);
    require(reopened.find_save("legacy") == a.head, "legacy save disappeared");
    require(count(directory.root, "SELECT version FROM catalog_info") == 1, "legacy open silently migrated schema");
    query_graph(directory.root, a.head, a);
  }
  void schema_integrity() {
    for (auto sql : {
      "CREATE TRIGGER ignore_generation BEFORE INSERT ON timeline_generations BEGIN SELECT RAISE(IGNORE); END",
      "DROP TRIGGER immutable_timeline_generations_UPDATE",
      "DROP TRIGGER immutable_timelines_DELETE",
      "ALTER TABLE timeline_generations RENAME COLUMN generation TO changed"}) {
      temporary directory;
      { auto db = catalog::create(directory.root, id(1)); }
      { sql_connection db(directory.root); db.exec(sql); }
      rejects([&] { (void)catalog::open(directory.root); });
    }
  }
  void reject_version_view() {
    temporary directory;
    { auto db = catalog::create(directory.root, id(1)); }
    {
      sql_connection sql(directory.root);
      sql.exec("DROP TABLE catalog_info; CREATE VIEW catalog_info AS SELECT 1 AS singleton, never_execute_this() AS version, '' AS identity, x'' AS policy");
    }
    bool schema_rejected = false;
    try { (void)catalog::open(directory.root); }
    catch (std::invalid_argument const &) { schema_rejected = true; }
    require(schema_rejected, "catalog queried a replacement version view before validating its shape");
  }
  void malformed_outcomes() {
    temporary directory;
    std::optional<graph<policy>> source;
    {
      auto db = catalog::create(directory.root, id(1)); source = persist(db, 100);
      for (unsigned i = 0; i != 4; ++i) db.create_timeline("create-" + std::to_string(i), "name-" + std::to_string(i), source->head);
    }
    for (unsigned i = 0; i != 4; ++i) {
      auto op = "create-" + std::to_string(i), name = "name-" + std::to_string(i);
      std::vector<std::byte> bytes;
      { auto db = catalog::open(directory.root); bytes = db.lookup_operation(op)->outcome; }
      if (i == 0) bytes.clear();
      if (i == 1) bytes.resize(7);
      if (i == 2) std::fill(bytes.begin(), bytes.begin() + 8, std::byte{0xff});
      if (i == 3) bytes.push_back(std::byte{0});
      {
        sql_connection sql(directory.root);
        sql.exec("DROP TRIGGER immutable_operations_UPDATE");
        catalog_detail::statement update(sql.db, "UPDATE operations SET outcome=? WHERE id=?");
        update.blob(1, bytes); update.key(2, op); update.done();
        sql.exec("CREATE TRIGGER immutable_operations_UPDATE BEFORE UPDATE ON operations BEGIN SELECT RAISE(ABORT,'immutable catalog row'); END");
      }
      auto db = catalog::open(directory.root);
      bool corrupt = false;
      try { (void)db.create_timeline(op, name, source->head); }
      catch (catalog_error const & error) { corrupt = error.code == SQLITE_CORRUPT; }
      require(corrupt && db.poisoned(), "malformed replay outcome was not bounded and rejected");
      retained(directory.root, 4);
    }
  }
  void bits() {
    temporary directory;
    using P = storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<0>>>>, 7, golomb<3>, 16>;
    auto db = sqlite_catalog<P>::create(directory.root, id(1));
    auto a = persist(db, 100);
    auto initial = db.create_timeline("create", "bits", a.head);
    auto next = db.publish_timeline("publish", initial, a.head);
    require(next.published && next.head.generation == 1, "bit timeline publication failed");
    query_graph(directory.root, next.head.head, a);
  }
}

int main() {
  lifecycle(); contention(); ambiguous_commits(); metadata_only(); process_restarts(); legacy(); schema_integrity(); reject_version_view(); malformed_outcomes(); bits();
  std::cout << "SQLite timeline generations, CAS, replay and retention: " << catalog::runtime_version() << '\n';
}
#else
int main() { std::cout << "SQLite timeline fixtures require POSIX directory barriers\n"; }
#endif
