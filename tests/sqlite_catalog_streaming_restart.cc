/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks named sort-owned streamed frontiers across sealing and publication process crashes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/connection.h>
#include <diet/sort_runtime_context.h>
#include <diet/typed_scan.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using namespace diet;
  using clock_type = std::chrono::steady_clock;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  enum class stage { admission, logical_publication, service, equivalent_publication };
  enum class event : unsigned {
    admission_native, admission_index_pair, logical_publication,
    service_native, service_index_pair, equivalent_publication,
    native_barrier, index_barrier, count
  };
  constexpr std::array<char const *, unsigned(event::count)> names{
    "admission native seal", "admission index seal/registration", "logical publication",
    "service native seal", "service index seal/registration", "equivalent publication",
    "native final barrier", "index final barrier"};
  struct notice {
    std::uint64_t generation = 0;
    unsigned selected = 0, after = 0;
    std::array<char, 33> operation{};
  };
  struct control {
    event selected;
    bool after;
    int ready = -1, resume = -1;
    bool enabled = false;
    stage current = stage::admission;
    std::uint64_t generation = 0;
    std::array<unsigned, unsigned(event::count)> encounters{};

    void reach(event point, bool completed, std::string_view operation = {}) noexcept {
      if (!enabled) return;
      auto & number = encounters[unsigned(point)];
      if (!completed) ++number;
      if (point != selected || completed != after || number != 1) return;
      notice message;
      message.generation = generation;
      message.selected = unsigned(point);
      message.after = unsigned(completed);
      if (operation.size() > 32) ::_exit(90);
      if (!operation.empty()) std::memcpy(message.operation.data(), operation.data(), operation.size());
      ssize_t written;
      do { written = ::write(ready, &message, sizeof message); } while (written < 0 && errno == EINTR);
      if (written != static_cast<ssize_t>(sizeof message)) ::_exit(91);
      // The parent kills this process. This signal is test control, never an
      // application acknowledgment; the selected hook has not returned.
      char byte;
      do { written = ::read(resume, &byte, 1); } while (written < 0 && errno == EINTR);
      ::_exit(92);
    }
  };
  std::shared_ptr<control> stopping;

  struct transaction_ops {
    std::shared_ptr<control> state = stopping;
    int commit(sqlite3 * db) noexcept {
      if (!state || !state->enabled) return sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      sqlite3_stmt * row = nullptr;
      // The operation row is inserted immediately before this real COMMIT.
      // Inspecting it avoids coupling cuts to incidental reserve transaction counts.
      char const * sql = "SELECT id,kind,CASE WHEN kind='seal' THEN "
        "(SELECT kind FROM objects WHERE id=CAST(substr(operations.request,9,32) AS TEXT)) "
        "ELSE -1 END FROM operations ORDER BY rowid DESC LIMIT 1";
      if (sqlite3_prepare_v2(db, sql, -1, &row, nullptr) != SQLITE_OK || sqlite3_step(row) != SQLITE_ROW)
        ::_exit(93);
      auto id = static_cast<char const *>(sqlite3_column_blob(row, 0));
      int length = sqlite3_column_bytes(row, 0);
      auto kind = reinterpret_cast<char const *>(sqlite3_column_text(row, 1));
      int native = sqlite3_column_int(row, 2);
      if (!id || length != 32 || !kind) ::_exit(94);
      std::array<char, 32> operation;
      std::memcpy(operation.data(), id, operation.size());
      std::optional<event> point, joint;
      auto phase = state->current;
      bool admission = phase == stage::admission || phase == stage::logical_publication;
      if (std::strcmp(kind, "seal") == 0) {
        if (native != 0) ::_exit(95);
        point = admission ? event::admission_native : event::service_native;
      } else if (std::strcmp(kind, "seal_cola_pair") == 0) {
        point = admission ? event::admission_index_pair : event::service_index_pair;
      } else if (std::strcmp(kind, "seal_native_cola_pair") == 0) {
        // Both files become catalog-visible at the same COMMIT. Keep a cut
        // for each obligation, with the same operation as its recovery oracle.
        point = admission ? event::admission_native : event::service_native;
        joint = admission ? event::admission_index_pair : event::service_index_pair;
      } else if (std::strcmp(kind, "publish_tap") == 0) {
        if (phase == stage::logical_publication) point = event::logical_publication;
        if (phase == stage::equivalent_publication) point = event::equivalent_publication;
      }
      sqlite3_finalize(row);
      std::string_view op(operation.data(), operation.size());
      if (point) state->reach(*point, false, op);
      if (joint) state->reach(*joint, false, op);
      int result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      if (result == SQLITE_OK && point) state->reach(*point, true, op);
      if (result == SQLITE_OK && joint) state->reach(*joint, true, op);
      return result;
    }
  };
  struct file_ops : posix_object_ops {
    struct descriptor { int fd = -1; unsigned syncs = 0; bool index = false; };
    std::array<descriptor, 16> files{};
    std::shared_ptr<control> state = stopping;
    int create_private(int parent, char const * name) noexcept {
      int fd = posix_object_ops::create_private(parent, name);
      if (fd >= 0) {
        for (auto & file : files) if (file.fd < 0) {
          file = {fd, 0, std::string_view(name).find(".index.") != std::string_view::npos};
          return fd;
        }
        ::_exit(96);
      }
      return fd;
    }
    int sync_file(int fd) noexcept {
      auto found = std::find_if(files.begin(), files.end(), [&](auto const & file) { return file.fd == fd; });
      bool final = found != files.end() && ++found->syncs == 2 && state && state->current == stage::service;
      auto point = found != files.end() && found->index ? event::index_barrier : event::native_barrier;
      if (final) state->reach(point, false);
      int result = posix_object_ops::sync_file(fd);
      if (!result && final) state->reach(point, true);
      return result;
    }
    int close(int fd) noexcept {
      for (auto & file : files) if (file.fd == fd) file.fd = -1;
      return posix_object_ops::close(fd);
    }
  };

  // These are the ordinary bit string registry and redundant streamed storage,
  // with test-only syscall hooks. Fresh parent connections have no active hook.
  using P = string_policy;
  using strings = unsorted<std::optional<std::string>>;
  using family = streaming_sort_runtime_family<P, registry_selector<string_registry>,
    random_object_ids, transaction_ops, file_ops>;
  using core = typed_engine<P, wrapping_fingerprint_algebra, 256, family>;
  using runtime = core::runtime_type;
  using engine = persistent_engine<>; // Recovery exercises the ordinary default bit engine.
  using store = engine::store_type;
  using stopped_store = runtime_store<P, random_object_ids, transaction_ops, family>;
  using oracle = std::map<std::string, std::string>;
  std::string const key("shared\0key", 10);
  std::string const old_value(513, 'o'), new_value(1025, 'n');
  oracle contents(std::string const & value) {
    oracle result{{key, value}};
    for (unsigned i = 0; i != 63; ++i)
      result.emplace("stable/" + std::to_string(i), "value/" + std::to_string(i));
    return result;
  }

  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-stream-restart-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  };
  struct pipe_pair {
    int read = -1, write = -1;
    pipe_pair() {
      int fds[2];
      if (::pipe(fds)) throw std::runtime_error("pipe");
      read = fds[0]; write = fds[1];
    }
    ~pipe_pair() { close_read(); close_write(); }
    void close_read() { if (read >= 0) ::close(std::exchange(read, -1)); }
    void close_write() { if (write >= 0) ::close(std::exchange(write, -1)); }
  };
  struct child_guard {
    pid_t pid;
    bool stop() noexcept {
      if (pid <= 0) return true;
      if (::kill(pid, SIGKILL) && errno != ESRCH) return false;
      auto end = clock_type::now() + std::chrono::seconds(5);
      do {
        int status = 0;
        auto result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
          pid = -1;
          return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        }
        if (result < 0 && errno != EINTR) return false;
        ::poll(nullptr, 0, 10);
      } while (clock_type::now() < end);
      return false;
    }
    ~child_guard() {
      if (pid > 0) (void)stop();
      if (pid > 0) std::abort(); // Never remove a directory with a live writer.
    }
  };
  notice await(int fd) {
    notice message;
    auto bytes = std::as_writable_bytes(std::span(&message, 1));
    auto end = clock_type::now() + std::chrono::seconds(25);
    while (!bytes.empty() && clock_type::now() < end) {
      pollfd descriptor{fd, POLLIN, 0};
      auto polled = ::poll(&descriptor, 1, 100);
      if (polled < 0 && errno == EINTR) continue;
      check(polled >= 0, "poll child");
      if (!polled) continue;
      auto count = ::read(fd, bytes.data(), bytes.size());
      if (count < 0 && errno == EINTR) continue;
      check(count > 0, "child exited before reaching requested cut");
      bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    check(bytes.empty(), "child checkpoint timeout");
    return message;
  }
  template <class E> void drain(E & active) {
    unsigned steps = 0;
    while (active.pending()) {
      active.advance(100'000);
      check(++steps < 1000, "restored work stalled");
    }
    check(active.admission_ready(), "restored engine not ready");
  }
  std::uint64_t signature(oracle const & expected) {
    std::uint64_t result = 0;
    for (auto const & [k, v] : expected)
      result += sort_semantics<strings>::hash_key(k) * sort_semantics<strings>::hash_value(k, v);
    return result;
  }
  template <class Cola> void verify(Cola const & state, oracle const & expected) {
    check(state.live_count() == expected.size() && state.signature() == signature(expected), "logical count/hash");
    for (auto const & [k, v] : expected) check(state.get(k) == v, "logical query");
    check(!state.get("absent"), "unexpected key");
    auto cursor = diet::scan(state);
    auto entry = expected.begin();
    while (auto row = cursor.next()) {
      check(entry != expected.end() && row->key == entry->first && row->value == entry->second, "resolved scan");
      ++entry;
    }
    check(entry == expected.end(), "scan missing key");
    state.runtime().query_root().head()->mapped()->scan();
    for (auto const & object : runtime_storage_codec<typename Cola::runtime_family>::objects(state.runtime().frontier())) {
      check(object->native->mapped() && !object->native->owned(), "restored frontier retained owned native");
      object->native->mapped()->scan();
    }
  }
  void child_run(std::filesystem::path const & root, std::shared_ptr<control> state) {
    stopping = state;
    auto saved = stopped_store::open(root);
    auto initial = saved.find("latest");
    check(bool(initial), "missing baseline");
    auto meta = engine::metadata_type::decode(initial->semantic);
    // Ordinary small outputs stay private until publication. The four syscall
    // cuts force streaming so they still exercise the context's FileOps hooks;
    // graph publication has its own independently tested file writer.
    bool eager = state->selected == event::native_barrier || state->selected == event::index_barrier;
    auto storage = family::storage_type::open(root, {}, {}, {}, {},
      eager ? runtime_output_options{0, 0} : runtime_output_options{});
    auto context = storage.context();
    auto active = runtime::from_snapshot(initial->snapshot, std::move(storage));
    while (active.pending()) active.advance(100'000);
    state->generation = initial->head.timeline.generation;
    state->enabled = true;
    state->current = stage::admission;
    auto contribution = core::put(key, new_value);
    check(bool(active.try_contribute(contribution.records().front(), 0)), "pending fixture admission failed");
    check(active.pending(), "zero-service contribution did not leave real work");
    meta.signature = signature(contents(new_value));
    ++meta.mutations;
    auto frontier = active.checkpoint();
    meta.validate(frontier.admissions());
    if (!eager) check(!context->sealed_outputs() && !context->sealed_indexes() &&
      context->retained_output_bytes(), "small admission did not retain its private encoding");
    state->current = stage::logical_publication;
    auto head = saved.publish(initial->head, frontier, meta.encode());
    state->generation = head.head.timeline.generation;
    unsigned steps = 0;
    while (active.pending()) {
      state->current = stage::service;
      auto price = active.next_service_cost();
      auto credit = active.credit();
      active.advance(price > credit ? price - credit : 1);
      frontier = active.checkpoint();
      state->current = stage::equivalent_publication;
      head = saved.publish(head.head, frontier, meta.encode());
      state->generation = head.head.timeline.generation;
      check(++steps < 1000, "child service stalled");
    }
    state->enabled = false;
    ::_exit(97); // Every requested cut must have stopped this run before here.
  }
  void integrity_and_operation(std::filesystem::path const & root, notice const & message) {
    sqlite3 * db = nullptr;
    check(sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
      "open SQL oracle");
    struct closer { sqlite3 * value; ~closer() { sqlite3_close(value); } } close{db};
    auto read = [&](char const * sql) {
      sqlite3_stmt * row = nullptr;
      check(sqlite3_prepare_v2(db, sql, -1, &row, nullptr) == SQLITE_OK, "prepare SQL oracle");
      return row;
    };
    auto row = read("PRAGMA integrity_check");
    check(sqlite3_step(row) == SQLITE_ROW &&
      std::strcmp(reinterpret_cast<char const *>(sqlite3_column_text(row, 0)), "ok") == 0, "SQLite integrity");
    check(sqlite3_step(row) == SQLITE_DONE, "extra integrity rows");
    sqlite3_finalize(row);
    row = read("PRAGMA foreign_key_check");
    check(sqlite3_step(row) == SQLITE_DONE, "SQLite foreign keys");
    sqlite3_finalize(row);
    if (message.operation[0]) {
      row = read("SELECT count(*) FROM operations WHERE id=?");
      sqlite3_bind_blob(row, 1, message.operation.data(), 32, SQLITE_TRANSIENT);
      check(sqlite3_step(row) == SQLITE_ROW && sqlite3_column_int(row, 0) == int(message.after),
        "operation commit boundary changed after crash");
      sqlite3_finalize(row);
    }
  }
  void run_case(std::filesystem::path const & source, event point, bool after) {
    temporary dir;
    std::filesystem::copy(source, dir.root, std::filesystem::copy_options::recursive |
      std::filesystem::copy_options::overwrite_existing);
    pipe_pair ready, resume;
    auto pid = ::fork();
    check(pid >= 0, "fork");
    if (!pid) {
      ready.close_read(); resume.close_write();
      auto state = std::make_shared<control>();
      state->selected = point; state->after = after;
      state->ready = ready.write; state->resume = resume.read;
      try { child_run(dir.root, std::move(state)); }
      catch (std::exception const & error) {
        std::fprintf(stderr, "restart child: %s\n", error.what());
        ::_exit(98);
      }
    }
    child_guard child{pid};
    ready.close_write(); resume.close_read();
    auto message = await(ready.read);
    check(message.selected == unsigned(point) && bool(message.after) == after, "wrong child checkpoint");
    check(child.stop(), "child was not killed at checkpoint");
    ready.close_read(); resume.close_write();
    integrity_and_operation(dir.root, message);

    auto active = engine::connect(dir.root, "latest", {.create_if_missing = false});
    auto state = active.snapshot();
    bool newer = point >= event::service_native || (point == event::logical_publication && after);
    auto expected = contents(newer ? new_value : old_value);
    verify(state, expected);
    auto expected_generation = message.generation +
      unsigned(after && (point == event::logical_publication || point == event::equivalent_publication));
    check(state.head().timeline.generation == expected_generation, "wrong named generation after COMMIT cut");
    check(state.runtime().admissions() == (newer ? 66 : 65), "restart admission mass");
    drain(active);
    verify(active.snapshot(), expected);
    active.contribute(engine::core_type::put("resumed", "yes"));
    expected["resumed"] = "yes";
    drain(active);
    verify(active.snapshot(), expected);
    active.contribute(engine::core_type::erase(key));
    expected.erase(key);
    drain(active);
    verify(active.snapshot(), expected);
    auto saved = store::open(dir.root).find_save("old");
    check(bool(saved), "old save vanished");
    auto old_meta = engine::metadata_type::decode(saved->semantic);
    auto old = engine::typed_cola_type::restore(saved->snapshot, old_meta, old_meta.schema_id);
    verify(old, contents(old_value));
    // The snapshot opened before recovery and later writes remains immutable.
    verify(state, contents(newer ? new_value : old_value));
  }
}

int main() {
  try {
    temporary baseline;
    {
      auto active = engine::connect(baseline.root, "latest");
      auto batch = engine::core_type::batch();
      for (auto const & [k, v] : contents(old_value)) batch.put(k, v);
      active.contribute(std::move(batch).finish());
      drain(active);
      // Keep a valid large replacement generation below its rebuild trigger:
      // b=64, u=1. The child's next overwrite leaves a real two-input carry
      // while b=64, u=2 remains valid independently of its private progress.
      active.contribute(engine::core_type::put(key, old_value));
      drain(active);
      auto state = active.snapshot();
      auto saved = store::open(baseline.root);
      saved.save("old", state.head());
      verify(state, contents(old_value));
    } // No live SQLite connection is inherited by a child.
    for (unsigned n = 0; n != unsigned(event::count); ++n)
      for (bool after : {false, true}) {
        std::cerr << names[n] << (after ? " after\n" : " before\n");
        run_case(baseline.root, event(n), after);
      }
    std::cout << "12 adaptive publication and 4 streamed barrier interruption cuts passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
