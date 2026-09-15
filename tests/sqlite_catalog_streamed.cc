/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks streamed merge publication and retained roots across bounded process interruptions.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/sqlite_catalog.h>
#include <diet/native_file_merge.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using namespace diet;
  using clock_type = std::chrono::steady_clock;
  void require(bool okay, char const * message) { if (!okay) throw std::runtime_error(message); }
  object_id id(unsigned value) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", value); return object_id(text);
  }
  object_attempt_id attempt(unsigned value) { return object_attempt_id(id(value).hex()); }
  blob_identity older_head() { return {id(10), id(11)}; }
  blob_identity newer_head() { return {id(20), id(21)}; }
  blob_identity candidate_head() { return {id(30), id(31)}; }
  constexpr std::array<char const *, 5> operations{
    "next-reserve", "next-native", "next-index", "next-register", "next-publish"};

  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-streamed-XXXXXX").string();
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

  enum class phase : unsigned char {
    before_commit = 1, after_commit = 2, partial_stream = 3, after_native_seal = 4, after_index_seal = 5
  };
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

  // Integer IDs determine ordering and replacement independently of encoded
  // keys and the merge implementation. Values force several real buffer writes.
  template<class P> struct plan {
    std::vector<unsigned> older_ids{0, 2, 4, 6, 8, 10, 12};
    std::vector<unsigned> newer_ids{1, 2, 3, 6, 9, 12, 13};
    std::vector<unsigned> merged_ids;
    std::vector<profile_record> older, newer, merged;
    std::array<catalog_object_reservation, 2> outputs{{
      {candidate_head().native, file_kind::native_blob},
      {candidate_head().index, file_kind::fractional_index}}};
    static bit_string key(unsigned n) {
      char text[16]; std::snprintf(text, sizeof text, "k%02u", n);
      auto result = bit_string::from_bytes(text);
      if constexpr (P::unit == profile_unit::bit) {
        result.bytes.push_back(std::byte{0xa0}); result.bit_size += 3;
      }
      return result;
    }
    static profile_record record(unsigned n, unsigned generation) {
      bit_string value;
      value.bytes.resize(32768 + n);
      for (std::size_t i = 0; i != value.bytes.size(); ++i)
        value.bytes[i] = static_cast<std::byte>((i * 29 + n * 17 + generation * 83) & 255);
      value.bit_size = value.bytes.size() * 8;
      if constexpr (P::unit == profile_unit::bit) {
        value.bytes.push_back(std::byte{0x60}); value.bit_size += 3;
      }
      return {key(n), std::move(value)};
    }
    plan() {
      for (auto n : older_ids) older.push_back(record(n, 0));
      for (auto n : newer_ids) newer.push_back(record(n, 1));
      for (unsigned n = 0; n != 15; ++n) {
        bool in_newer = std::find(newer_ids.begin(), newer_ids.end(), n) != newer_ids.end();
        bool in_older = std::find(older_ids.begin(), older_ids.end(), n) != older_ids.end();
        if (in_newer || in_older) {
          merged_ids.push_back(n); merged.push_back(record(n, in_newer ? 1 : 0));
        }
      }
      require(merged.size() <= P::group_size, "streamed fixture needs a prepared terminal head");
    }
    template<class Catalog> void reserve(Catalog & metadata) const {
      std::array inputs{older_head(), newer_head()};
      metadata.reserve(operations[0], attempt(40), "next-build", inputs, outputs);
    }
    template<class Catalog> void apply(Catalog & metadata, control & state) const {
      auto expected = metadata.find_timeline("timeline");
      require(bool(expected) && expected->generation == 0, "missing initial timeline generation");
      state.current = 0; reserve(metadata);
      auto old_input = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(
        metadata.root() / object_path(older_head().native, file_kind::native_blob)));
      auto new_input = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(
        metadata.root() / object_path(newer_head().native, file_kind::native_blob)));
      state.current = 1;
      native_file_merge<P, mapped_native<P>> merge(metadata.root(), candidate_head().native,
        attempt(40), old_input, new_input);
      old_input.reset(); new_input.reset(); // The merger alone now pins its mmap sources.
      merge.step(5);
      require(merge.progress().keys == 5 && !merge.done(), "wrong partial-stream boundary");
      require(std::filesystem::exists(merge.paths().private_output) &&
        std::filesystem::file_size(merge.paths().private_output) > 96 + 128,
        "partial-stream boundary did not write a private payload");
      require(!std::filesystem::exists(merge.paths().final), "partial stream published its native name");
      state.reach(phase::partial_stream);
      while (!merge.done()) merge.step(2);
      require(merge.progress().keys == merged.size() &&
        merge.progress().input_records == older.size() + newer.size(), "streamed merge counters");
      auto native_receipt = merge.finish();
      state.reach(phase::after_native_seal);
      metadata.record_sealed(operations[1], native_receipt);
      state.current = 2;
      auto index = profile_index<P>::native_only(merge.progress().keys);
      auto encoded_index = encode_index_sections(index, candidate_head().native);
      auto index_receipt = encoded_index.seal(metadata.root(), candidate_head().index, attempt(40));
      state.reach(phase::after_index_seal);
      metadata.record_sealed(operations[2], index_receipt);
      auto query = open_mapped_query<P>(metadata.root(), candidate_head());
      state.current = 3; metadata.register_chain(operations[3], query, catalog_admission::scan);
      state.current = 4;
      auto published = metadata.publish_timeline(operations[4], *expected, candidate_head());
      require(published.published && published.head.generation == 1 &&
        published.head.head == candidate_head(), "streamed publication did not advance the expected head");
    }
  };

  template<class P> catalog_timeline_head baseline(std::filesystem::path const & root, plan<P> const & work) {
    auto metadata = sqlite_catalog<P>::create(root, id(1));
    std::array outputs{
      catalog_object_reservation{older_head().native, file_kind::native_blob},
      catalog_object_reservation{older_head().index, file_kind::fractional_index},
      catalog_object_reservation{newer_head().native, file_kind::native_blob},
      catalog_object_reservation{newer_head().index, file_kind::fractional_index}};
    metadata.reserve("base-reserve", attempt(4), "base-build", {}, outputs);
    auto persist = [&](blob_identity const & head, std::span<profile_record const> records, char const * tag) {
      auto pair = profile_blob<P>::build(records);
      auto native = encode_native_sections(pair.native());
      auto index = encode_index_sections(pair, head.native);
      metadata.record_sealed(std::string("base-native-") + tag, native.seal(root, head.native, attempt(4)));
      metadata.record_sealed(std::string("base-index-") + tag, index.seal(root, head.index, attempt(4)));
      auto query = open_mapped_query<P>(root, head);
      metadata.register_chain(std::string("base-register-") + tag, query, catalog_admission::scan);
    };
    persist(older_head(), work.older, "old"); persist(newer_head(), work.newer, "new");
    metadata.save("base-save", "old-save", older_head());
    metadata.save("source-save", "source-save", newer_head());
    metadata.acquire_save("base-reader", "old-save", "old-reader");
    return metadata.create_timeline("base-timeline", "timeline", older_head());
  } // Connections and source mappings are gone before fork.

  template<class P> void query_values(std::filesystem::path const & root, blob_identity const & head,
      std::span<unsigned const> ids, std::span<profile_record const> records) {
    auto query = open_mapped_query<P>(root, head);
    query.head()->scan();
    for (unsigned n = 0; n != 15; ++n) {
      auto position = std::find(ids.begin(), ids.end(), n);
      auto key = plan<P>::key(n);
      auto cursor = query.cursor(key.view());
      unsigned matches = 0, steps = 0;
      while (!cursor.done()) {
        require(++steps <= 3, "streamed terminal query failed to make progress");
        cursor.step(1);
        if (cursor.has_match()) {
          auto found = cursor.take_match(); ++matches;
          require(position != ids.end(), "streamed query invented an absent key");
          auto ordinal = static_cast<std::size_t>(position - ids.begin());
          require(found.source->identity() == head && found.ordinal == ordinal,
            "streamed query returned the wrong exact source or ordinal");
          require(found.value.bit_size == records[ordinal].value.bit_size &&
            found.value.bytes == records[ordinal].value.bytes, "streamed query changed the original value bits");
        }
      }
      require(matches == (position != ids.end() ? 1u : 0u), "streamed query lost or duplicated a binding");
    }
  }
  using transcript = std::array<catalog_operation, operations.size()>;
  struct reference_result { transcript operations; catalog_timeline_head published; };
  template<class P> reference_result reference(plan<P> const & work) {
    temporary directory;
    (void)baseline(directory.root, work);
    auto metadata = sqlite_catalog<P>::open(directory.root);
    control disabled; disabled.enabled = false;
    work.apply(metadata, disabled);
    transcript result;
    for (std::size_t i = 0; i != result.size(); ++i) {
      auto entry = metadata.lookup_operation(operations[i]);
      require(bool(entry), "missing streamed reference operation"); result[i] = std::move(*entry);
    }
    query_values<P>(directory.root, candidate_head(), work.merged_ids, work.merged);
    return {std::move(result), *metadata.find_timeline("timeline")};
  }
  std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
      auto value = static_cast<unsigned char>(byte);
      result.push_back(digits[value >> 4]); result.push_back(digits[value & 15]);
    }
    return result;
  }
  template<class P> void verify(std::filesystem::path const & root, plan<P> const & work,
      reference_result const & expected, catalog_timeline_head const & initial, checkpoint cut) {
    auto committed = cut.operation + (cut.when == phase::after_commit);
    bool reserved = committed >= 1, registered = committed >= 4, published = committed >= 5;
    unsigned receipts = committed ? std::min(committed - 1, 2u) : 0;
    auto metadata = sqlite_catalog<P>::open(root);
    require(metadata.find_save("old-save") == older_head(), "streamed restart lost the acknowledged old save");
    require(metadata.find_save("source-save") == newer_head(), "streamed restart lost the newer source save");
    auto current = metadata.find_timeline("timeline");
    require(current == (published ? expected.published : initial), "streamed restart exposed an incomplete timeline generation");
    for (std::size_t i = 0; i != operations.size(); ++i) {
      auto actual = metadata.lookup_operation(operations[i]);
      require(bool(actual) == (i < committed), "streamed restart exposed the wrong committed operation prefix");
      if (actual) require(actual->kind == expected.operations[i].kind && actual->request == expected.operations[i].request &&
        actual->outcome == expected.operations[i].outcome, "streamed restart changed exact request/outcome bytes");
    }
    sql_reader sql(root); sql.integrity();
    require(sql.count("SELECT count(*) FROM operations") == 11 + committed, "streamed partial operation row");
    require(sql.count("SELECT count(*) FROM attempts") == 1 + reserved, "streamed partial reservation");
    require(sql.count("SELECT count(*) FROM objects") == 4 + 2 * reserved, "streamed lost reserved object identities");
    require(sql.count("SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 4 + receipts, "streamed partial receipt metadata");
    require(sql.count("SELECT count(*) FROM pairs") == 2 + registered, "streamed partial pair registration");
    require(sql.count("SELECT count(*) FROM owners") == 5 + reserved + published, "streamed partial owners");
    require(sql.count("SELECT count(*) FROM owner_objects") == 4 + 2 * reserved, "streamed lost output pins");
    require(sql.count("SELECT count(*) FROM owner_roots") == 4 + 2 * reserved + published, "streamed partial root retention");
    require(sql.count("SELECT count(*) FROM saves") == 2, "streamed changed immutable saves");
    require(sql.count("SELECT count(*) FROM timelines") == 1, "streamed changed timeline identity");
    require(sql.count("SELECT count(*) FROM timeline_generations") == 1 + published, "streamed partial timeline history");
    auto exact_root = [&](char const * kind, std::string_view owner, blob_identity const & head) {
      return sql.count("SELECT count(*) FROM owner_roots WHERE owner_kind='" + std::string(kind) +
        "' AND owner_id=X'" + hex(owner) + "' AND native_id='" + head.native.hex() + "' AND index_id='" + head.index.hex() + "'");
    };
    require(exact_root("save", "old-save", older_head()) == 1, "streamed lost exact old save pin");
    require(exact_root("save", "source-save", newer_head()) == 1, "streamed lost exact newer source pin");
    require(exact_root("reader", "old-reader", older_head()) == 1, "streamed lost acknowledged reader pin");
    require(exact_root("timeline", initial.owner, older_head()) == 1, "streamed lost historical timeline root");
    require(exact_root("timeline", expected.published.owner, candidate_head()) == published, "streamed incomplete published root pin");
    for (auto const & head : {older_head(), newer_head()}) {
      require(exact_root("attempt", "next-build", head) == reserved, "streamed lost exact merge input pin");
      require(sql.count("SELECT count(*) FROM pairs WHERE native_id='" + head.native.hex() + "' AND index_id='" +
        head.index.hex() + "' AND target_native IS NULL AND target_index IS NULL AND native_count=7 AND borrowed_count=0 AND virtual_count=7") == 1,
        "streamed changed original pair metadata");
      for (auto const & object : {head.native, head.index}) {
        require(sql.count("SELECT count(*) FROM objects WHERE id='" + object.hex() + "' AND bytes IS NOT NULL") == 1,
          "streamed lost original seal metadata");
        require(sql.count("SELECT count(*) FROM owner_objects WHERE owner_kind='attempt' AND owner_id=X'" +
          hex("base-build") + "' AND object_id='" + object.hex() + "'") == 1, "streamed lost original output retention");
      }
    }
    auto candidate = candidate_head();
    require(sql.count("SELECT count(*) FROM pairs WHERE native_id='" + candidate.native.hex() + "' AND index_id='" +
      candidate.index.hex() + "' AND target_native IS NULL AND target_index IS NULL AND native_count=" +
      std::to_string(work.merged.size()) + " AND borrowed_count=0 AND virtual_count=" + std::to_string(work.merged.size())) == registered,
      "streamed changed registered output pair metadata");
    for (auto const & output : work.outputs)
      require(sql.count("SELECT count(*) FROM owner_objects WHERE owner_kind='attempt' AND owner_id=X'" +
        hex("next-build") + "' AND object_id='" + output.object.hex() + "'") == reserved, "streamed lost reserved output pin");
    auto paths = object_output_paths(root, candidate.native, attempt(40), file_kind::native_blob);
    bool partial = cut.when == phase::partial_stream;
    require(std::filesystem::exists(paths.private_output) == partial, "streamed unexpected private-output lifetime");
    if (partial) require(std::filesystem::file_size(paths.private_output) > 96 + 128,
      "streamed interrupted attempt has no physical payload");
    require(std::filesystem::exists(paths.final) == (cut.operation >= 1 && !partial), "streamed native publication boundary");
    require(std::filesystem::exists(root / object_path(candidate.index, file_kind::fractional_index)) == (cut.operation >= 2),
      "streamed index publication boundary");
    query_values<P>(root, older_head(), work.older_ids, work.older);
    query_values<P>(root, newer_head(), work.newer_ids, work.newer);
    if (registered) query_values<P>(root, candidate, work.merged_ids, work.merged);
    // Reconcile by stable IDs only. No scan is used to rehabilitate a failed
    // sync, and no private stream is resumed or adopted as a complete object.
    if (reserved) work.reserve(metadata);
    if (published) require(metadata.publish_timeline(operations[4], initial, candidate) ==
      catalog_timeline_publication{true, expected.published}, "streamed replay changed acknowledged publication");
    require(sql.count("SELECT count(*) FROM operations") == 11 + committed, "streamed replay duplicated an operation");
    require(sql.count("SELECT count(*) FROM owner_roots") == 4 + 2 * reserved + published, "streamed replay duplicated retention");
  }

  template<class P> void interrupted(plan<P> const & work, reference_result const & expected, checkpoint cut) {
    temporary directory; auto initial = baseline(directory.root, work);
    // No live SQLite connection crosses fork. Each process opens its own one;
    // SIGKILL tests process interruption, not physical power loss or fsync failure.
    pipe_pair ready, resume;
    auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork streamed writer");
    if (!child) {
      ready.close_read(); resume.close_write();
      try {
        auto state = std::make_shared<control>();
        state->wanted = cut; state->ready = ready.write; state->resume = resume.read;
        auto metadata = sqlite_catalog<P, stopping_ops>::open(directory.root, {}, {state});
        work.apply(metadata, *state);
        ::_exit(93);
      } catch (std::exception const & error) {
        std::fprintf(stderr, "streamed child: %s\n", error.what()); ::_exit(94);
      } catch (...) { ::_exit(95); }
    }
    child_guard cleanup(child);
    ready.close_write(); resume.close_read();
    auto deadline = clock_type::now() + std::chrono::seconds(15);
    unsigned char message[2]{}; std::size_t received = 0;
    while (received != sizeof message) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now()).count();
      require(remaining > 0, "streamed child checkpoint timeout");
      pollfd descriptor{ready.read, POLLIN, 0};
      int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
      if (result < 0 && errno == EINTR) continue;
      require(result > 0, "streamed child checkpoint wait failed");
      auto count = ::read(ready.read, message + received, sizeof message - received);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "streamed child failed before its checkpoint");
      received += static_cast<std::size_t>(count);
    }
    require(message[0] == cut.operation && message[1] == static_cast<unsigned char>(cut.when), "wrong streamed checkpoint");
    require(cleanup.stop(), "streamed child was not killed and reaped");
    verify(directory.root, work, expected, initial, cut);
    verify(directory.root, work, expected, initial, cut);
  }
  template<class P> unsigned exercise() {
    plan<P> work;
    auto expected = reference(work);
    unsigned cases = 0;
    auto run = [&](checkpoint cut) {
      try { interrupted(work, expected, cut); ++cases; }
      catch (std::exception const & error) {
        throw std::runtime_error(std::string(P::unit == profile_unit::byte ? "byte" : "bit") +
          " operation " + std::to_string(cut.operation) + " phase " +
          std::to_string(static_cast<unsigned>(cut.when)) + ": " + error.what());
      }
    };
    for (unsigned operation = 0; operation != operations.size(); ++operation)
      for (auto when : {phase::before_commit, phase::after_commit}) run({operation, when});
    run({1, phase::partial_stream});
    run({1, phase::after_native_seal});
    run({2, phase::after_index_seal});
    return cases;
  }
}

int main() {
  try {
    using byte_policy = storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 15, exponential_golomb<0>, 4>;
    using bit_policy = storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 15, golomb<3>, 4>;
    auto cases = exercise<byte_policy>() + exercise<bit_policy>();
    std::cout << "SQLite streamed merge: " << cases << " bounded SIGKILL cuts; SQLite "
      << sqlite_catalog<byte_policy>::runtime_version() << '\n';
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
#else
int main() { std::cout << "SQLite streamed restart tests require POSIX fork and signals\n"; }
#endif
