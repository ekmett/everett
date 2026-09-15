/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/sqlite_catalog.h>

#include <sqlite3.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  using namespace diet;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid catalog operation accepted");
  }

  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
#if defined(__unix__) || defined(__APPLE__)
      auto pattern = (std::filesystem::temp_directory_path() / "diet-sqlite-adversarial-XXXXXX").string();
      auto result = ::mkdtemp(pattern.data());
      if (!result) throw std::system_error(errno, std::generic_category(), "create catalog fixture");
      path = result;
#else
      for (unsigned i = 0; i < 10000; ++i) {
        auto candidate = std::filesystem::temp_directory_path() / ("diet-sqlite-adversarial-" + std::to_string(i));
        if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
      }
      throw std::runtime_error("cannot reserve catalog fixture directory");
#endif
    }
    ~temporary_directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  object_id id(unsigned n) {
    char value[33];
    std::snprintf(value, sizeof value, "fedcba9876543210fedcba98%08x", n);
    return object_id(value);
  }

  // A separate raw SQLite connection observes committed rows. It does not use
  // the catalog's serializers, statement helpers or replay comparison logic.
  struct observer {
    sqlite3 * db = nullptr;
    explicit observer(std::filesystem::path const & path) {
      if (sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        throw std::runtime_error("open independent catalog observer");
      }
    }
    observer(observer const &) = delete;
    observer & operator=(observer const &) = delete;
    ~observer() { if (sqlite3_close(db) != SQLITE_OK) std::terminate(); }
    void exec(char const * sql) {
      char * error = nullptr;
      auto code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
      std::string message = error ? error : "independent SQL statement failed";
      sqlite3_free(error);
      if (code != SQLITE_OK) throw std::runtime_error(message);
    }
    struct statement {
      sqlite3_stmt * value = nullptr;
      statement(sqlite3 * db, char const * sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
          throw std::runtime_error("prepare independent catalog query");
      }
      ~statement() { sqlite3_finalize(value); }
    };
    std::int64_t integer(char const * sql) {
      statement query(db, sql);
      require(sqlite3_step(query.value) == SQLITE_ROW && sqlite3_column_type(query.value, 0) == SQLITE_INTEGER,
              "missing independent SQL integer");
      auto value = sqlite3_column_int64(query.value, 0);
      require(sqlite3_step(query.value) == SQLITE_DONE, "multiple independent SQL integer rows");
      return value;
    }
    std::vector<std::byte> blob(char const * sql, std::string_view key) {
      statement query(db, sql);
      require(sqlite3_bind_blob64(query.value, 1, key.empty() ? "" : key.data(), key.size(), SQLITE_TRANSIENT) == SQLITE_OK,
              "bind independent catalog identifier");
      require(sqlite3_step(query.value) == SQLITE_ROW && sqlite3_column_type(query.value, 0) == SQLITE_BLOB,
              "missing independent SQL blob");
      auto size = sqlite3_column_bytes(query.value, 0);
      auto first = static_cast<std::byte const *>(sqlite3_column_blob(query.value, 0));
      std::vector<std::byte> result;
      if (size) result.assign(first, first + size);
      require(sqlite3_step(query.value) == SQLITE_DONE, "multiple independent SQL blob rows");
      return result;
    }
  };

  using policy = storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 16>;
  using catalog = sqlite_catalog<policy>;

  std::vector<std::byte> pair_bytes(blob_identity const & pair) {
    std::vector<std::byte> result;
    for (auto const * object : {&pair.native, &pair.index}) {
      for (unsigned i = 0; i < 8; ++i) result.push_back(std::byte(i ? 0 : 32));
      for (unsigned char c : object->hex()) result.push_back(std::byte(c));
    }
    return result;
  }
  object_attempt_id attempt(unsigned n) { return object_attempt_id(id(n).hex()); }

  struct fixture {
    temporary_directory directory;
    blob_identity head{id(101), id(102)};
    object_attempt_id output_attempt = attempt(103);
    std::vector<profile_record> records;
    profile_blob<policy> source;
    catalog db;
    std::vector<object_seal_receipt> receipts;
    std::optional<mapped_query_root<policy>> root;

    explicit fixture(bool malformed = false, bool large = false)
      : records(make_records(large)), source(profile_blob<policy>::build(records)),
        db(catalog::create(directory.path, id(100), {0})) {
      std::array outputs{catalog_object_reservation{head.native, file_kind::native_blob},
                         catalog_object_reservation{head.index, file_kind::fractional_index}};
      db.reserve("fixture/reserve", output_attempt, "fixture/owner", {}, outputs);
      observer sql(database_path());
      require(sql.integer("SELECT count(*) FROM objects WHERE bytes IS NULL") == 2 &&
              sql.integer("SELECT count(*) FROM owner_objects") == 2 &&
              sql.integer("SELECT count(*) FROM operations") == 1,
              "reservation and retention were not externally committed before construction");
      for (auto const & output : outputs)
        require(!std::filesystem::exists(directory.path / object_path(output.object, output.kind)),
                "reservation unexpectedly constructed an object");
      std::array<std::uint64_t, 2> ceilings{0, 0};
      auto native = malformed ? profile_array<policy>::build(records, ceilings) : profile_array<policy>::build(records);
      auto native_encoding = encode_native_sections(native);
      auto index_encoding = encode_index_sections(source, head.native);
      receipts.push_back(native_encoding.seal(directory.path, head.native, output_attempt));
      receipts.push_back(index_encoding.seal(directory.path, head.index, output_attempt));
      db.record_sealed("fixture/native", receipts[0]);
      db.record_sealed("fixture/index", receipts[1]);
      root.emplace(open_mapped_query<policy>(directory.path, head));
      if (!malformed) db.register_chain("fixture/register", *root, catalog_admission::scan);
    }
    std::filesystem::path database_path() const { return directory.path / "catalog.sqlite3"; }
    static std::vector<profile_record> make_records(bool large) {
      auto prefix = large ? std::string(65536, 'a') : std::string("a");
      return {{bit_string::from_bytes(prefix + "a"), {}}, {bit_string::from_bytes(prefix + "b"), {}}};
    }
  };

  struct commit_state {
    enum class action { normal, error_before, error_after, success_without_commit };
    action next = action::normal;
    int calls = 0;
    int observed_autocommit = -1;
  };
  struct injected_commit {
    std::shared_ptr<commit_state> state = std::make_shared<commit_state>();
    int commit(sqlite3 * db) noexcept {
      ++state->calls;
      auto action = std::exchange(state->next, commit_state::action::normal);
      state->observed_autocommit = sqlite3_get_autocommit(db);
      if (action == commit_state::action::error_before) return SQLITE_IOERR_FSYNC;
      if (action == commit_state::action::success_without_commit) return SQLITE_OK;
      auto code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      if (code != SQLITE_OK) return code;
      state->observed_autocommit = sqlite3_get_autocommit(db);
      return action == commit_state::action::error_after ? SQLITE_IOERR_FSYNC : SQLITE_OK;
    }
  };

  void exact_replay_test() {
    fixture value;
    auto & db = value.db;
    auto outputs = std::array{catalog_object_reservation{value.head.native, file_kind::native_blob},
                              catalog_object_reservation{value.head.index, file_kind::fractional_index}};
    auto before = db.lookup_operation("fixture/reserve");
    require(before && before->kind == "reserve" && before->outcome.empty(), "missing reservation outcome");
    db.reserve("fixture/reserve", value.output_attempt, "fixture/owner", {}, outputs);
    auto after = db.lookup_operation("fixture/reserve");
    require(after && after->request == before->request && after->outcome == before->outcome, "exact reserve replay changed outcome");
    rejects([&] { db.reserve("fixture/reserve", attempt(104), "fixture/owner", {}, outputs); });
    rejects([&] { db.reserve("fixture/reserve", value.output_attempt, "fixture/owner!", {}, outputs); });
    auto reversed = outputs; std::swap(reversed[0], reversed[1]);
    rejects([&] { db.reserve("fixture/reserve", value.output_attempt, "fixture/owner", {}, reversed); });
    auto wrong_kind = outputs; wrong_kind[0].kind = file_kind::fractional_index;
    rejects([&] { db.reserve("fixture/reserve", value.output_attempt, "fixture/owner", {}, wrong_kind); });
    std::array inputs{value.head};
    rejects([&] { db.reserve("fixture/reserve", value.output_attempt, "fixture/owner", inputs, outputs); });
    db.record_sealed("fixture/native", value.receipts[0]);
    auto wrong_receipt = value.receipts[0]; ++wrong_receipt.body_crc32c;
    rejects([&] { db.record_sealed("fixture/native", wrong_receipt); });
    wrong_receipt = value.receipts[0]; ++wrong_receipt.bytes;
    rejects([&] { db.record_sealed("fixture/native", wrong_receipt); });
    db.register_chain("fixture/register", *value.root, catalog_admission::scan);
    rejects([&] { db.register_chain("fixture/register", *value.root, catalog_admission::trusted); });
    rejects([&] { db.save("fixture/reserve", "cross-kind", value.head); });
    db.save("save/replay", "checkpoint", value.head);
    db.save("save/replay", "checkpoint", value.head);
    auto outcome = db.lookup_operation("save/replay");
    require(outcome && outcome->kind == "save" && outcome->outcome == pair_bytes(value.head), "save outcome differs from independent encoding");
    rejects([&] { db.save("save/replay", "checkpoint-changed", value.head); });
    auto wrong_head = value.head; wrong_head.index = id(999);
    rejects([&] { db.save("save/replay", "checkpoint", wrong_head); });
    rejects([&] { db.save("save/replace", "checkpoint", wrong_head); });
    require(db.find_save("checkpoint") == value.head && !db.find_save("checkpoint-changed") && !db.poisoned(),
            "rejected replay changed save or poisoned healthy connection");
    auto acquired = db.acquire_save("reader/replay", "checkpoint", "reader-owner");
    auto replay = db.acquire_save("reader/replay", "checkpoint", "reader-owner");
    require(acquired.head == value.head && replay.head == acquired.head && replay.owner == acquired.owner,
            "reader replay changed its pin");
    rejects([&] { (void)db.acquire_save("reader/replay", "checkpoint", "other-reader"); });
    observer sql(value.database_path());
    require(sql.integer("SELECT count(*) FROM objects") == 2 && sql.integer("SELECT count(*) FROM owner_objects") == 2 &&
            sql.integer("SELECT count(*) FROM saves") == 1 && sql.integer("SELECT count(*) FROM owners WHERE kind='reader'") == 1,
            "replay duplicated rows or retention");
    require(sql.blob("SELECT outcome FROM operations WHERE id=?", "save/replay") == pair_bytes(value.head),
            "raw observer disagrees with operation outcome");
  }

  void binary_identifiers_test() {
    fixture value;
    constexpr char raw_op[] = "op\0'; DROP TABLE objects;--\xff";
    std::string op(raw_op, sizeof raw_op - 1);
    std::string name("save\0suffix", 11), owner("reader\0suffix", 13);
    value.db.save(op, name, value.head);
    value.db.save(op, name, value.head);
    require(value.db.find_save(name) == value.head && !value.db.find_save("save"), "save name was truncated at NUL");
    require(value.db.lookup_operation(op) && !value.db.lookup_operation("op"), "operation identity was truncated at NUL");
    auto different = op; different.back() ^= 1;
    rejects([&] { value.db.save(op, "different-name", value.head); });
    value.db.save(different, "other-name", value.head);
    auto pin = value.db.acquire_save(std::string("get\0pin", 7), name, owner);
    require(pin.owner == owner && pin.head == value.head, "reader owner was truncated at NUL");
    observer sql(value.database_path());
    require(sql.integer("SELECT count(*) FROM objects") == 2 && sql.integer("SELECT count(*) FROM saves") == 2,
            "identifier data executed SQL or merged distinct names");
    require(sql.blob("SELECT id FROM owners WHERE kind='reader' AND id=?", owner) ==
            std::vector<std::byte>(reinterpret_cast<std::byte const *>(owner.data()), reinterpret_cast<std::byte const *>(owner.data() + owner.size())),
            "raw reader owner differs from supplied binary bytes");
  }

  void uncertain_commit_test() {
    using fault_catalog = sqlite_catalog<policy, injected_commit>;
    for (bool acquire : {false, true}) for (auto action : {
        commit_state::action::error_before, commit_state::action::error_after,
        commit_state::action::success_without_commit}) {
      fixture value;
      if (acquire) value.db.save("prior/save", "prior-save", value.head);
      auto state = std::make_shared<commit_state>();
      {
        auto failed = fault_catalog::open(value.directory.path, {0}, injected_commit{state});
        state->next = action;
        bool threw = false;
        try {
          if (acquire) (void)failed.acquire_save("uncertain/save", "prior-save", "possibly-pinned");
          else failed.save("uncertain/save", "possibly-saved", value.head);
        }
        catch (catalog_error const & error) {
          threw = true;
          require(error.outcome_unknown && error.operation == "uncertain/save", "ambiguous commit lost its operation identity");
        }
        require(threw && failed.poisoned() && state->calls == 1, "commit error was retried or acknowledged");
        require(state->observed_autocommit == (action == commit_state::action::error_after ? 1 : 0),
                "commit injector did not reach the intended transaction boundary");
        rejects([&] { (void)failed.lookup_operation("uncertain/save"); });
        rejects([&] { (void)failed.find_save("possibly-saved"); });
        rejects([&] { failed.save("after-error", "later", value.head); });
        require(state->calls == 1, "poisoned connection attempted another commit");
      }
      auto recovered = catalog::open(value.directory.path, {0});
      auto committed = action == commit_state::action::error_after;
      require(bool(recovered.lookup_operation("uncertain/save")) == committed &&
              bool(recovered.find_save("possibly-saved")) == (!acquire && committed),
              "reopened catalog misclassified actual commit outcome");
      observer sql(value.database_path());
      require(sql.integer("SELECT count(*) FROM owner_objects") == 2 &&
              sql.integer("SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 2,
              "uncertain save released precommitted output pins");
      require(sql.integer("SELECT count(*) FROM owners WHERE kind='reader'") == (acquire && committed ? 1 : 0),
              "reader pin disagrees with committed acquisition outcome");
      if (acquire) {
        auto pin = recovered.acquire_save("uncertain/save", "prior-save", "possibly-pinned");
        require(pin.head == value.head && pin.owner == "possibly-pinned", "resolved reader acquisition changed outcome");
      } else {
        recovered.save("uncertain/save", "possibly-saved", value.head);
        require(recovered.find_save("possibly-saved") == value.head, "resolved exact operation did not replay");
      }
      require(std::filesystem::exists(value.receipts[0].path) && std::filesystem::exists(value.receipts[1].path),
              "uncertain commit removed sealed files");
      auto cursor = value.root->cursor(value.records.front().key.view());
      require(cursor.step() == 1 && cursor.has_match(), "catalog poisoning invalidated retained mapping");
    }
  }

  void concurrent_connections_test() {
    fixture value;
    auto other = catalog::open(value.directory.path, {0});
    observer sql(value.database_path());
    std::array inputs{value.head};
    std::array outputs{catalog_object_reservation{id(201), file_kind::native_blob},
                       catalog_object_reservation{id(202), file_kind::fractional_index}};
    auto generation = attempt(203);
    sql.exec("BEGIN IMMEDIATE");
    bool busy = false;
    try { other.reserve("contended/reserve", generation, "contended/owner", inputs, outputs); }
    catch (catalog_error const & error) {
      busy = true;
      require((error.code & 255) == SQLITE_BUSY && !error.outcome_unknown, "writer contention was reported as storage uncertainty");
    }
    require(busy && !other.poisoned() && !other.lookup_operation("contended/reserve"), "failed BEGIN left an operation or poisoned handle");
    sql.exec("ROLLBACK");
    other.reserve("contended/reserve", generation, "contended/owner", inputs, outputs);
    value.db.reserve("contended/reserve", generation, "contended/owner", inputs, outputs);
    rejects([&] { value.db.reserve("competing/reserve", attempt(204), "competing/owner", inputs, outputs); });
    require(!value.db.poisoned() && sql.integer("SELECT count(*) FROM objects") == 4 &&
            sql.integer("SELECT count(*) FROM owner_objects") == 4 &&
            sql.integer("SELECT count(*) FROM attempts") == 2 &&
            sql.integer("SELECT count(*) FROM owners WHERE kind='attempt'") == 2 &&
            sql.integer("SELECT count(*) FROM owner_roots WHERE owner_kind='attempt'") == 1,
            "competing reservation leaked rows or lost committed input retention");
    require(!value.db.lookup_operation("competing/reserve"), "failed competing reservation recorded an outcome");
  }

  void schema_policy_test() {
    fixture value;
    rejects([&] { (void)sqlite_catalog<storage_policy<profile_unit::bit, fixed_values<0>, 7, exponential_golomb<0>, 16>>::open(value.directory.path); });
    rejects([&] { (void)sqlite_catalog<storage_policy<profile_unit::byte, variable_values, 7, exponential_golomb<0>, 16>>::open(value.directory.path); });
    rejects([&] { (void)sqlite_catalog<storage_policy<profile_unit::byte, fixed_values<1>, 7, exponential_golomb<0>, 16>>::open(value.directory.path); });
    rejects([&] { (void)sqlite_catalog<storage_policy<profile_unit::byte, fixed_values<0>, 3, exponential_golomb<0>, 16>>::open(value.directory.path); });
    rejects([&] { (void)sqlite_catalog<storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 15>>::open(value.directory.path); });
    {
      temporary_directory bits;
      using bit_policy = storage_policy<profile_unit::bit, fixed_values<0>, 7, exponential_golomb<0>, 16>;
      auto bit_catalog = sqlite_catalog<bit_policy>::create(bits.path, id(400));
      rejects([&] { (void)sqlite_catalog<storage_policy<profile_unit::bit, fixed_values<0>, 7, golomb<3>, 16>>::open(bits.path); });
      rejects([&] { (void)sqlite_catalog<storage_policy<profile_unit::bit, fixed_values<0>, 7, exponential_golomb<1>, 16>>::open(bits.path); });
    }
    rejects([&] { (void)catalog::open(value.directory.path, {-1}); });
    rejects([&] { (void)catalog::create(value.directory.path, id(333)); });
    require(value.db.lookup_operation("fixture/register").has_value(), "rejected open/create damaged existing catalog");
    {
      temporary_directory missing;
      rejects([&] { (void)catalog::open(missing.path); });
      require(!std::filesystem::exists(missing.path / "catalog.sqlite3"), "open created a missing catalog");
      rejects([&] { (void)catalog::create(missing.path, id(401), {-1}); });
      require(!std::filesystem::exists(missing.path / "catalog.sqlite3"), "invalid create options reserved a catalog name");
      auto valid = catalog::create(missing.path, id(401));
      require(!valid.lookup_operation("anything"), "corrected creation failed after invalid options");
    }
    {
      auto original = catalog::open(value.directory.path);
      { auto moved = std::move(original); require(moved.lookup_operation("fixture/register").has_value(), "moved-to catalog lost connection"); }
      rejects([&] { (void)original.lookup_operation("fixture/register"); });
      rejects([&] { (void)original.find_save("anything"); });
    }
    {
      observer sql(value.database_path());
      sql.exec("CREATE VIEW diagnostic_objects AS SELECT id,bytes FROM objects; CREATE INDEX diagnostic_kind ON objects(kind)");
      auto with_diagnostics = catalog::open(value.directory.path);
      require(with_diagnostics.lookup_operation("fixture/register").has_value(), "diagnostic view/index made catalog incompatible");
    }
    {
      observer sql(value.database_path());
      sql.exec("DROP TRIGGER immutable_catalog_info_UPDATE; PRAGMA ignore_check_constraints=ON; UPDATE catalog_info SET version=2");
      rejects([&] { (void)catalog::open(value.directory.path); });
    }
    for (auto corruption : {
        "DROP TRIGGER immutable_catalog_info_UPDATE; UPDATE catalog_info SET policy=x'00'",
        "DROP TABLE operations",
        "ALTER TABLE operations RENAME COLUMN request TO unrecognized_request",
        "DROP TRIGGER immutable_operations_UPDATE",
        "CREATE TRIGGER skip_operation BEFORE INSERT ON operations BEGIN SELECT RAISE(IGNORE); END"}) {
      fixture malformed;
      observer sql(malformed.database_path());
      sql.exec(corruption);
      rejects([&] { (void)catalog::open(malformed.directory.path); });
    }
  }

  void trusted_admission_test() {
    // A nonmaximal FC encoding is structurally valid with a freshly streamed
    // CRC and successful seal barriers. Trusted admission must not turn into
    // a hidden semantic scan; explicit scan must reject this same fixture.
    fixture value(true);
    rejects([&] { value.root->head()->scan(); });
    value.db.register_chain("trusted/admission", *value.root);
    require(value.db.lookup_operation("trusted/admission").has_value(), "trusted admission scanned payload semantics");
    rejects([&] { value.db.register_chain("checked/admission", *value.root, catalog_admission::scan); });
    require(!value.db.lookup_operation("checked/admission") && !value.db.poisoned(), "semantic scan failure changed catalog state");
  }

  void uncertain_reservation_test() {
    for (auto action : {commit_state::action::error_before, commit_state::action::error_after}) {
      temporary_directory directory;
      auto original = catalog::create(directory.path, id(900));
      std::array outputs{catalog_object_reservation{id(901), file_kind::native_blob},
                         catalog_object_reservation{id(902), file_kind::fractional_index}};
      auto state = std::make_shared<commit_state>();
      auto failed = sqlite_catalog<policy, injected_commit>::open(directory.path, {0}, injected_commit{state});
      state->next = action;
      rejects([&] { failed.reserve("uncertain/reserve", attempt(903), "pending-owner", {}, outputs); });
      require(failed.poisoned(), "uncertain reservation did not poison handle");
      auto recovered = catalog::open(directory.path);
      auto committed = action == commit_state::action::error_after;
      require(bool(recovered.lookup_operation("uncertain/reserve")) == committed,
              "reservation replay log did not match the actual commit");
      observer sql(directory.path / "catalog.sqlite3");
      require(sql.integer("SELECT count(*) FROM objects") == (committed ? 2 : 0) &&
              sql.integer("SELECT count(*) FROM owner_objects") == (committed ? 2 : 0),
              "reservation committed without pins or leaked rolled-back rows");
      for (auto const & output : outputs)
        require(!std::filesystem::exists(directory.path / object_path(output.object, output.kind)),
                "failed reservation created external output");
      recovered.reserve("uncertain/reserve", attempt(903), "pending-owner", {}, outputs);
      rejects([&] { sql.exec("UPDATE objects SET bytes=96 WHERE bytes IS NULL"); });
      require(sql.integer("SELECT count(*) FROM objects WHERE bytes IS NULL AND crc IS NULL AND barrier IS NULL") == 2,
              "partial sealed object state passed SQL CHECK");
    }
  }

#if defined(__unix__) || defined(__APPLE__)
  void path_alias_test() {
    fixture value;
    temporary_directory aliases;
    auto alias = aliases.path / "same-root";
    std::filesystem::create_directory_symlink(value.directory.path, alias);
    auto opened = catalog::open(alias);
    require(opened.root() == std::filesystem::canonical(value.directory.path), "catalog root alias was not canonicalized");
    opened.save("alias/save", "alias-checkpoint", value.head);
    require(value.db.find_save("alias-checkpoint") == value.head, "root alias opened a different catalog");
    auto receipt = value.receipts[0];
    receipt.path = alias / object_path(value.head.native, file_kind::native_blob);
    opened.record_sealed("fixture/native", receipt);
    temporary_directory redirected;
    std::filesystem::create_symlink(value.database_path(), redirected.path / "catalog.sqlite3");
    rejects([&] { (void)catalog::open(redirected.path); });
    rejects([&] { (void)catalog::create(redirected.path, id(402)); });
    require(value.db.find_save("alias-checkpoint") == value.head, "rejected catalog symlink altered its target");
  }
#endif

  void embedding_thread_mode_test() {
    temporary_directory directory;
    { auto created = catalog::create(directory.path, id(500)); }
    // sqlite3_threadsafe() reports the compile-time option, not an embedding
    // application's current global configuration. No connections remain here.
    require(sqlite3_shutdown() == SQLITE_OK && sqlite3_config(SQLITE_CONFIG_SINGLETHREAD) == SQLITE_OK,
            "cannot configure independent single-thread fixture");
    require(sqlite3_threadsafe() != 0, "test SQLite lacks compiled mutex support");
    rejects([&] { (void)catalog::open(directory.path); });
    require(sqlite3_shutdown() == SQLITE_OK && sqlite3_config(SQLITE_CONFIG_SERIALIZED) == SQLITE_OK,
            "cannot restore serialized SQLite configuration");
    auto restored = catalog::open(directory.path);
    require(!restored.lookup_operation("anything"), "single-thread rejection damaged catalog");
  }
}

int main(int argc, char ** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--expect-unsupported-runtime") {
      require(sqlite3_libversion_number() < 3051003, "runtime rejection fixture unexpectedly linked supported SQLite");
      temporary_directory unsupported;
      rejects([&] { (void)catalog::open(unsupported.path); });
      require(!std::filesystem::exists(unsupported.path / "catalog.sqlite3"), "old runtime rejection created a catalog");
      rejects([&] { (void)catalog::create(unsupported.path, id(499)); });
      require(!std::filesystem::exists(unsupported.path / "catalog.sqlite3"), "old runtime create reserved a catalog name");
      std::cout << "unsupported SQLite runtime rejected\n";
      return 0;
    }
    require(argc == 1, "unexpected adversarial test argument");
    require(sqlite3_libversion_number() >= 3051003, "adversarial suite requires SQLite 3.51.3 or later");
    exact_replay_test();
    binary_identifiers_test();
    uncertain_commit_test();
    concurrent_connections_test();
    schema_policy_test();
    trusted_admission_test();
    uncertain_reservation_test();
#if defined(__unix__) || defined(__APPLE__)
    path_alias_test();
#endif
    embedding_thread_mode_test();
    std::cout << "adversarial SQLite catalog tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks replay, uncertain commits and external retention independently.
 */
