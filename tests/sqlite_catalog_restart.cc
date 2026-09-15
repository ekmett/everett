/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks saved roots and exact operations after process interruption, without simulating power loss.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/sqlite_catalog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using namespace diet;
  using policy = storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 4>;
  using catalog = sqlite_catalog<policy>;
  using clock_type = std::chrono::steady_clock;
  void require(bool okay, char const * message) { if (!okay) throw std::runtime_error(message); }
  object_id id(unsigned value) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", value); return object_id(text);
  }
  object_attempt_id attempt(unsigned value) { return object_attempt_id(id(value).hex()); }
  blob_identity baseline_head() { return {id(2), id(3)}; }
  constexpr std::array<char const *, 8> operations{
    "next-reserve", "next-native-1", "next-index-1", "next-native-0", "next-index-0",
    "next-register", "next-save", "next-reader"};

  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-restart-XXXXXX").string();
      auto made = ::mkdtemp(name.data());
      if (!made) throw std::runtime_error("mkdtemp restart fixture");
      root = made;
    }
    ~temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
  };
  struct pipe_pair {
    int read = -1, write = -1;
    pipe_pair() {
      int descriptors[2];
      if (::pipe(descriptors)) throw std::runtime_error("restart pipe");
      read = descriptors[0]; write = descriptors[1];
    }
    pipe_pair(pipe_pair const &) = delete;
    ~pipe_pair() { close_read(); close_write(); }
    void close_read() noexcept { if (read >= 0) ::close(std::exchange(read, -1)); }
    void close_write() noexcept { if (write >= 0) ::close(std::exchange(write, -1)); }
  };
  struct child_guard {
    pid_t pid;
    explicit child_guard(pid_t value) : pid(value) {}
    child_guard(child_guard const &) = delete;
    bool stop() noexcept {
      if (pid <= 0) return true;
      if (::kill(pid, SIGKILL) && errno != ESRCH) return false;
      auto end = clock_type::now() + std::chrono::seconds(5);
      int status = 0;
      do {
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
      if (pid > 0) {
        (void)stop();
        if (pid > 0) {
          // Do not continue to remove a fixture while its writer might be live.
          std::fputs("failed to kill/reap restart child\n", stderr); std::abort();
        }
      }
    }
  };

  enum class phase : unsigned char { before_commit = 1, after_commit = 2, after_seal = 3 };
  struct checkpoint {
    unsigned operation = 0;
    phase when = phase::before_commit;
  };
  struct control {
    checkpoint wanted;
    unsigned current = 0;
    int ready = -1, resume = -1;
    bool enabled = true;
    void reach(phase when) const noexcept {
      if (!enabled || current != wanted.operation || when != wanted.when) return;
      unsigned char message[2]{static_cast<unsigned char>(current), static_cast<unsigned char>(when)};
      ssize_t written;
      do { written = ::write(ready, message, sizeof message); } while (written < 0 && errno == EINTR);
      if (written != sizeof message) ::_exit(91);
      // This is test control, not an application acknowledgment. The parent
      // kills us here; a COMMIT hook has not returned to the catalog method.
      unsigned char byte;
      for (;;) {
        auto result = ::read(resume, &byte, 1);
        if (result < 0 && errno == EINTR) continue;
        ::_exit(92); // Unexpected resume or closed pipe must never publish more.
      }
    }
  };
  struct stopping_ops {
    std::shared_ptr<control> state;
    int commit(sqlite3 * db) noexcept {
      state->reach(phase::before_commit);
      int result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      if (result == SQLITE_OK) state->reach(phase::after_commit);
      return result;
    }
  };

  struct sql_reader {
    sqlite3 * db = nullptr;
    explicit sql_reader(std::filesystem::path const & root) {
      int code = sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
      if (code != SQLITE_OK) { if (db) sqlite3_close(db); throw std::runtime_error("open restart oracle"); }
    }
    ~sql_reader() { sqlite3_close(db); }
    sql_reader(sql_reader const &) = delete;
    std::int64_t count(std::string const & sql) const {
      sqlite3_stmt * statement = nullptr;
      require(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "prepare restart count");
      int result = sqlite3_step(statement);
      auto answer = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
      sqlite3_finalize(statement);
      require(result == SQLITE_ROW, "read restart count"); return answer;
    }
    void integrity() const {
      sqlite3_stmt * statement = nullptr;
      require(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, nullptr) == SQLITE_OK,
              "prepare restart integrity");
      int result = sqlite3_step(statement);
      auto text = result == SQLITE_ROW ? sqlite3_column_text(statement, 0) : nullptr;
      bool okay = text && std::strcmp(reinterpret_cast<char const *>(text), "ok") == 0;
      if (okay) okay = sqlite3_step(statement) == SQLITE_DONE;
      sqlite3_finalize(statement); require(okay, "restart catalog integrity");
      require(count("SELECT count(*) FROM pragma_foreign_key_check") == 0, "restart foreign key integrity");
    }
  };
  void baseline(std::filesystem::path const & root) {
    auto metadata = catalog::create(root, id(1));
    auto head = baseline_head();
    std::array outputs{catalog_object_reservation{head.native, file_kind::native_blob},
                       catalog_object_reservation{head.index, file_kind::fractional_index}};
    metadata.reserve("base-reserve", attempt(4), "base-build", {}, outputs);
    std::array records{profile_record{bit_string::from_bytes("base"), bit_string::from_bytes("old")}};
    auto pair = profile_blob<policy>::build(records);
    auto native = encode_native_sections(pair.native());
    auto index = encode_index_sections(pair, head.native);
    metadata.record_sealed("base-native", native.seal(root, head.native, attempt(4)));
    metadata.record_sealed("base-index", index.seal(root, head.index, attempt(4)));
    auto query = open_mapped_query<policy>(root, head);
    metadata.register_chain("base-register", query, catalog_admission::scan);
    metadata.save("base-save", "baseline", head);
    metadata.acquire_save("base-reader", "baseline", "base-reader");
  } // All SQLite connections close before fork; none are inherited for reuse.

  struct plan {
    std::vector<profile_record> records;
    std::vector<std::shared_ptr<profile_blob<policy> const>> chain;
    std::vector<blob_identity> identities;
    std::vector<catalog_object_reservation> outputs;
    plan() {
      for (auto key : {"base", "key1", "key2", "key3", "key4"})
        records.push_back({bit_string::from_bytes(key), bit_string::from_bytes(std::string("new/") + key)});
      auto pair = std::make_shared<profile_blob<policy> const>(profile_blob<policy>::build(records));
      auto prepared = query_root<policy>::build(pair);
      for (auto current = prepared.head(); current; current = current->target()) {
        auto i = static_cast<unsigned>(chain.size());
        chain.push_back(current); identities.push_back({id(10 + 2 * i), id(11 + 2 * i)});
        outputs.push_back({identities.back().native, file_kind::native_blob});
        outputs.push_back({identities.back().index, file_kind::fractional_index});
      }
      require(chain.size() == 2, "restart fixture needs a routing prefix and native target");
    }
    template<class Catalog> void reserve(Catalog & metadata) const {
      std::array inputs{baseline_head()};
      metadata.reserve(operations[0], attempt(20), "next-build", inputs, outputs);
    }
    template<class Catalog> void apply(Catalog & metadata, control & state) const {
      state.current = 0; reserve(metadata);
      unsigned operation = 1;
      for (std::size_t i = chain.size(); i-- != 0;) {
        auto native = encode_native_sections(chain[i]->native());
        auto index = encode_index_sections(*chain[i], identities[i].native,
          i + 1 < chain.size() ? std::optional{identities[i + 1]} : std::nullopt);
        state.current = operation;
        auto receipt = native.seal(metadata.root(), identities[i].native, attempt(20));
        state.reach(phase::after_seal);
        metadata.record_sealed(operations[operation++], receipt);
        state.current = operation;
        receipt = index.seal(metadata.root(), identities[i].index, attempt(20));
        state.reach(phase::after_seal);
        metadata.record_sealed(operations[operation++], receipt);
      }
      require(operation == 5, "restart operation accounting");
      auto mapped = open_mapped_query<policy>(metadata.root(), identities.front());
      state.current = 5; metadata.register_chain(operations[5], mapped, catalog_admission::scan);
      state.current = 6; metadata.save(operations[6], "candidate", identities.front());
      state.current = 7; metadata.acquire_save(operations[7], "candidate", "next-reader");
    }
  };
  void query_values(std::filesystem::path const & root, blob_identity const & head,
                    std::span<profile_record const> records) {
    auto query = open_mapped_query<policy>(root, head);
    query.head()->scan();
    for (auto const & record : records) {
      auto cursor = query.cursor(record.key.view());
      unsigned matches = 0, steps = 0;
      while (!cursor.done()) {
        require(++steps <= 16, "restart query failed to make bounded progress");
        cursor.step(1);
        if (cursor.has_match()) {
          auto found = cursor.take_match(); ++matches;
          require(compare_bits(found.value.view(), record.value.view()) == 0, "restart changed saved value");
        }
      }
      require(matches == 1, "restart lost or duplicated saved key");
    }
    auto absent = bit_string::from_bytes("missing");
    auto cursor = query.cursor(absent.view());
    unsigned steps = 0;
    while (!cursor.done()) {
      require(++steps <= 16, "restart missing-key query failed to make progress");
      cursor.step(1); require(!cursor.has_match(), "restart invented missing key");
    }
  }
  using transcript = std::array<catalog_operation, operations.size()>;
  transcript reference(plan const & work) {
    temporary directory;
    baseline(directory.root);
    auto metadata = catalog::open(directory.root);
    control disabled; disabled.enabled = false;
    work.apply(metadata, disabled);
    transcript result;
    for (std::size_t i = 0; i != result.size(); ++i) {
      auto entry = metadata.lookup_operation(operations[i]);
      require(bool(entry), "missing reference operation"); result[i] = std::move(*entry);
    }
    return result;
  }
  void verify(std::filesystem::path const & root, plan const & work, transcript const & expected,
              unsigned committed, unsigned sealed_files) {
    auto metadata = catalog::open(root);
    require(metadata.find_save("baseline") == baseline_head(), "restart lost acknowledged baseline save");
    bool reserved = committed >= 1, registered = committed >= 6, saved = committed >= 7, reader = committed >= 8;
    unsigned seals = committed ? std::min(committed - 1, 4u) : 0;
    for (std::size_t i = 0; i != operations.size(); ++i) {
      auto actual = metadata.lookup_operation(operations[i]);
      require(bool(actual) == (i < committed), "restart exposed wrong operation prefix");
      if (actual) require(actual->kind == expected[i].kind && actual->request == expected[i].request &&
                          actual->outcome == expected[i].outcome, "restart changed exact operation request/outcome");
    }
    require(metadata.find_save("candidate") == (saved ? std::optional{work.identities.front()} : std::nullopt),
            "restart exposed an incomplete save");
    sql_reader sql(root); sql.integrity();
    require(sql.count("SELECT count(*) FROM operations") == 6 + committed, "restart partial operation row");
    require(sql.count("SELECT count(*) FROM attempts") == 1 + reserved, "restart partial reservation");
    require(sql.count("SELECT count(*) FROM objects") == 2 + 4 * reserved, "restart lost reserved identities");
    require(sql.count("SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 2 + seals, "restart partial seal metadata");
    require(sql.count("SELECT count(*) FROM pairs") == 1 + 2 * registered, "restart partial exact graph");
    require(sql.count("SELECT count(*) FROM owner_objects") == 2 + 4 * reserved, "restart lost output pins");
    require(sql.count("SELECT count(*) FROM owner_roots") == 2 + reserved + saved + reader, "restart partial root pins");
    require(sql.count("SELECT count(*) FROM owners") == 3 + reserved + saved + reader, "restart partial owners");
    require(sql.count("SELECT count(*) FROM saves") == 1 + saved, "restart partial saved root");
    auto old = baseline_head();
    require(sql.count("SELECT count(*) FROM pairs WHERE native_id='" + old.native.hex() +
      "' AND index_id='" + old.index.hex() +
      "' AND target_native IS NULL AND target_index IS NULL AND native_count=1 AND borrowed_count=0 AND virtual_count=1") == 1,
      "restart changed exact baseline pair metadata");
    for (auto const & object : {old.native, old.index}) {
      require(sql.count("SELECT count(*) FROM objects WHERE id='" + object.hex() + "' AND bytes IS NOT NULL") == 1,
              "restart lost baseline seal metadata");
      require(sql.count("SELECT count(*) FROM owner_objects WHERE owner_kind='attempt' AND owner_id=X'626173652d6275696c64' AND object_id='" +
        object.hex() + "'") == 1, "restart lost baseline output retention");
    }
    if (registered) for (std::size_t i = 0; i != work.identities.size(); ++i) {
      auto const & identity = work.identities[i];
      std::string target = i + 1 == work.identities.size() ? "target_native IS NULL AND target_index IS NULL" :
        "target_native='" + work.identities[i + 1].native.hex() + "' AND target_index='" + work.identities[i + 1].index.hex() + "'";
      auto const & source = work.chain[i];
      require(sql.count("SELECT count(*) FROM pairs WHERE native_id='" + identity.native.hex() +
        "' AND index_id='" + identity.index.hex() + "' AND " + target +
        " AND native_count=" + std::to_string(source->native().size()) +
        " AND borrowed_count=" + std::to_string(source->borrowed().size()) +
        " AND virtual_count=" + std::to_string(source->virtual_size())) == 1,
        "restart changed exact registered target graph");
    }
    auto exact_root = [&](char const * kind, char const * owner_hex, blob_identity const & head) {
      std::string query = "SELECT count(*) FROM owner_roots WHERE owner_kind='" + std::string(kind) +
        "' AND owner_id=X'" + owner_hex + "' AND native_id='" + head.native.hex() + "' AND index_id='" + head.index.hex() + "'";
      return sql.count(query);
    };
    require(exact_root("save", "626173656c696e65", old) == 1, "restart lost baseline save pin");
    require(exact_root("reader", "626173652d726561646572", old) == 1, "restart lost baseline reader pin");
    require(exact_root("attempt", "6e6578742d6275696c64", old) == reserved, "restart lost old input retention");
    require(exact_root("save", "63616e646964617465", work.identities.front()) == saved, "restart wrong new save pin");
    require(exact_root("reader", "6e6578742d726561646572", work.identities.front()) == reader, "restart wrong new reader pin");
    for (auto const & output : work.outputs) {
      auto query = "SELECT count(*) FROM owner_objects WHERE owner_kind='attempt' AND owner_id=X'6e6578742d6275696c64' AND object_id='" +
        output.object.hex() + "'";
      require(sql.count(query) == reserved, "restart lost an exact output pin");
    }
    std::array old_records{profile_record{bit_string::from_bytes("base"), bit_string::from_bytes("old")}};
    query_values(root, old, old_records);
    if (saved) query_values(root, work.identities.front(), work.records);
    unsigned physical = 0;
    for (auto const & output : work.outputs) physical += std::filesystem::exists(root / object_path(output.object, output.kind));
    require(physical == sealed_files, "restart changed successfully sealed output files");
    // Exact replay is safe after process death: these operations have concrete
    // parameters and no fresh fsync failure is being reinterpreted as success.
    if (reserved) work.reserve(metadata);
    if (saved) metadata.save(operations[6], "candidate", work.identities.front());
    if (reader) require(metadata.acquire_save(operations[7], "candidate", "next-reader").head == work.identities.front(),
                        "restart replay changed reader head");
    require(sql.count("SELECT count(*) FROM operations") == 6 + committed, "restart replay duplicated an operation");
    require(sql.count("SELECT count(*) FROM owner_roots") == 2 + reserved + saved + reader, "restart replay duplicated a root pin");
  }

  void interrupted(plan const & work, transcript const & expected, checkpoint stop) {
    temporary directory; baseline(directory.root);
    // Only immutable C++ data remains open at fork. Each process opens its own
    // SQLite connection afterward; the test never reuses an inherited handle.
    pipe_pair ready, resume;
    auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork restart writer");
    if (!child) {
      ready.close_read(); resume.close_write();
      try {
        auto state = std::make_shared<control>();
        state->wanted = stop; state->ready = ready.write; state->resume = resume.read;
        auto metadata = sqlite_catalog<policy, stopping_ops>::open(directory.root, {}, {state});
        work.apply(metadata, *state);
        ::_exit(93); // Every requested cut must have paused before reaching EOF.
      } catch (...) { ::_exit(94); }
    }
    child_guard cleanup(child);
    ready.close_write(); resume.close_read();
    auto deadline = clock_type::now() + std::chrono::seconds(15);
    unsigned char message[2]{}; std::size_t received = 0;
    while (received != sizeof message) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now()).count();
      require(remaining > 0, "restart child checkpoint timeout");
      pollfd descriptor{ready.read, POLLIN, 0};
      int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
      if (result < 0 && errno == EINTR) continue;
      require(result > 0, "restart child checkpoint wait failed");
      auto count = ::read(ready.read, message + received, sizeof message - received);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "restart child failed before its checkpoint");
      received += static_cast<std::size_t>(count);
    }
    require(message[0] == stop.operation && message[1] == static_cast<unsigned char>(stop.when), "wrong restart checkpoint");
    require(cleanup.stop(), "restart child was not killed and reaped");
    auto committed = stop.operation + (stop.when == phase::after_commit);
    auto sealed = std::min(stop.operation, 4u);
    verify(directory.root, work, expected, committed, sealed);
    verify(directory.root, work, expected, committed, sealed); // Close/reopen once more after reconciliation.
  }
}

int main() {
  try {
    plan work;
    auto expected = reference(work);
    unsigned cases = 0;
    for (unsigned operation = 0; operation != operations.size(); ++operation)
      for (auto when : {phase::before_commit, phase::after_commit}) {
        interrupted(work, expected, {operation, when}); ++cases;
      }
    for (unsigned operation = 1; operation != 5; ++operation) {
      interrupted(work, expected, {operation, phase::after_seal}); ++cases;
    }
    std::cout << "SQLite process restart: " << cases << " bounded SIGKILL cuts; SQLite " << catalog::runtime_version() << '\n';
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
#else
int main() { std::cout << "SQLite process-restart tests require POSIX fork and signals\n"; }
#endif
