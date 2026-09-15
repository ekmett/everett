/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/mapped_blob.h>
#include <everett/object_writer.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

static_assert(SQLITE_VERSION_NUMBER >= 3051003, "Everett requires SQLite 3.51.3 or later");

namespace everett {
  enum class catalog_admission { trusted, scan };
  struct catalog_options { int busy_timeout_ms = 250; };
  struct catalog_object_reservation { object_id object; file_kind kind; };
  struct catalog_saved_root { blob_identity head; std::string owner; };
  // Every generation has its own permanent root owner. Names and owners are
  // arbitrary nonempty byte strings; generations never wrap or get reused.
  struct catalog_timeline_head {
    std::string name;
    std::uint64_t generation;
    blob_identity head;
    std::string owner;
    bool operator==(catalog_timeline_head const &) const = default;
  };
  struct catalog_timeline_publication {
    bool published;
    catalog_timeline_head head; // New generation, or the observed head on conflict.
    bool operator==(catalog_timeline_publication const &) const = default;
  };
  struct catalog_operation {
    std::string kind;
    std::vector<std::byte> request;
    std::vector<std::byte> outcome;
  };
  struct catalog_error : std::runtime_error {
    catalog_error(std::string message, int code, std::string operation = {}, bool uncertain = false)
      : std::runtime_error(std::move(message)), code(code), operation(std::move(operation)),
        outcome_unknown(uncertain) {}
    int code; // SQLite extended result code; custom COMMIT hooks preserve their result.
    std::string operation;
    bool outcome_unknown;
  };

  // This seam models an unacknowledged COMMIT, not storage or power failure.
  // A custom hook must be noexcept and either execute COMMIT or return an error.
  struct sqlite_catalog_ops {
    int commit(sqlite3 * db) noexcept { return sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr); }
  };

  namespace catalog_detail {
    using bytes = std::vector<std::byte>;
    inline void number(bytes & out, std::uint64_t value) {
      for (unsigned i = 0; i != 8; ++i) out.push_back(std::byte(value >> (8 * i)));
    }
    inline void field(bytes & out, std::string_view value) {
      number(out, value.size());
      auto data = std::as_bytes(std::span(value.data(), value.size()));
      out.insert(out.end(), data.begin(), data.end());
    }
    inline void identity(bytes & out, object_id const & value) {
      if (value.hex().size() != 32) throw std::invalid_argument("invalid Everett catalog identity");
      field(out, value.hex());
    }
    inline void pair(bytes & out, blob_identity const & value) {
      identity(out, value.native); identity(out, value.index);
    }
    inline void timeline(bytes & out, catalog_timeline_head const & value) {
      field(out, value.name); number(out, value.generation); pair(out, value.head); field(out, value.owner);
    }
    // Outcomes are decoded, rather than looking up today's mutable head during
    // replay. These bounds also reject a malformed stored operation outcome.
    struct outcome_reader {
      std::span<std::byte const> data;
      [[noreturn]] static void invalid() { throw catalog_error("invalid Everett timeline outcome", SQLITE_CORRUPT); }
      std::uint64_t number() {
        if (data.size() < 8) invalid();
        std::uint64_t value = 0;
        for (unsigned i = 0; i != 8; ++i) value |= std::uint64_t(std::to_integer<unsigned>(data[i])) << (8 * i);
        data = data.subspan(8); return value;
      }
      std::string field() {
        auto size = number();
        if (size > data.size()) invalid();
        std::string value(reinterpret_cast<char const *>(data.data()), static_cast<std::size_t>(size));
        data = data.subspan(static_cast<std::size_t>(size)); return value;
      }
      catalog_timeline_head timeline() {
        auto name = field(); auto generation = number();
        auto native = field(); auto index = field(); auto owner = field();
        if (name.empty() || owner.empty() || generation > std::uint64_t(std::numeric_limits<std::int64_t>::max())) invalid();
        try { return {std::move(name), generation, {object_id(native), object_id(index)}, std::move(owner)}; }
        catch (std::invalid_argument const &) { invalid(); }
      }
      void end() const { if (!data.empty()) invalid(); }
    };
    inline void name(std::string_view value) {
      if (value.empty()) throw std::invalid_argument("empty Everett catalog name");
    }
    inline std::int64_t integer(std::uint64_t value) {
      if (value > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        throw std::length_error("Everett catalog count exceeds SQLite integer range");
      return static_cast<std::int64_t>(value);
    }
    inline bool storage_error(int code) noexcept {
      switch (code & 255) {
        case SQLITE_IOERR: case SQLITE_FULL: case SQLITE_CORRUPT: case SQLITE_NOTADB:
        case SQLITE_NOMEM: case SQLITE_CANTOPEN: case SQLITE_PROTOCOL: return true;
        default: return false;
      }
    }
    [[noreturn]] inline void fail(sqlite3 * db, int code) {
      throw catalog_error(db && sqlite3_errcode(db) != SQLITE_OK ? sqlite3_errmsg(db) : sqlite3_errstr(code), code);
    }
    inline void exec(sqlite3 * db, char const * sql) {
      auto code = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
      if (code != SQLITE_OK) fail(db, code);
    }
    struct statement {
      sqlite3 * db;
      sqlite3_stmt * value = nullptr;
      statement(sqlite3 * db, char const * sql) : db(db) {
        auto code = sqlite3_prepare_v2(db, sql, -1, &value, nullptr);
        if (code != SQLITE_OK) {
          if (value) sqlite3_finalize(value);
          fail(db, code);
        }
      }
      ~statement() { if (value) sqlite3_finalize(value); }
      statement(statement const &) = delete;
      statement & operator=(statement const &) = delete;
      void text(int index, std::string_view data) {
        auto code = sqlite3_bind_text64(value, index, data.data(), data.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        if (code != SQLITE_OK) fail(db, code);
      }
      void blob(int index, std::span<std::byte const> data) {
        // A null pointer denotes SQL NULL, including when the length is zero.
        static constexpr std::byte empty{};
        auto code = sqlite3_bind_blob64(value, index, data.empty() ? &empty : data.data(), data.size(), SQLITE_TRANSIENT);
        if (code != SQLITE_OK) fail(db, code);
      }
      void key(int index, std::string_view data) {
        blob(index, std::as_bytes(std::span(data.data(), data.size())));
      }
      void integer(int index, std::int64_t number) {
        auto code = sqlite3_bind_int64(value, index, number);
        if (code != SQLITE_OK) fail(db, code);
      }
      void null(int index) {
        auto code = sqlite3_bind_null(value, index);
        if (code != SQLITE_OK) fail(db, code);
      }
      bool row() {
        auto code = sqlite3_step(value);
        if (code == SQLITE_ROW) return true;
        if (code == SQLITE_DONE) return false;
        fail(db, code);
      }
      void done() { if (row()) throw std::logic_error("unexpected SQLite result row"); }
      std::string text(int column) const {
        auto data = sqlite3_column_text(value, column);
        auto size = sqlite3_column_bytes(value, column);
        if (!data && (size || sqlite3_errcode(db) == SQLITE_NOMEM)) fail(db, SQLITE_NOMEM);
        return size ? std::string(reinterpret_cast<char const *>(data), std::size_t(size)) : std::string{};
      }
      std::string key(int column) const {
        auto data = static_cast<char const *>(sqlite3_column_blob(value, column));
        auto size = sqlite3_column_bytes(value, column);
        if (!data && (size || sqlite3_errcode(db) == SQLITE_NOMEM)) fail(db, SQLITE_NOMEM);
        return size ? std::string(data, std::size_t(size)) : std::string{};
      }
      bytes blob(int column) const {
        auto data = static_cast<std::byte const *>(sqlite3_column_blob(value, column));
        auto size = sqlite3_column_bytes(value, column);
        if (!data && (size || sqlite3_errcode(db) == SQLITE_NOMEM)) fail(db, SQLITE_NOMEM);
        return size ? bytes(data, data + size) : bytes{};
      }
      std::int64_t integer(int column) const { return sqlite3_column_int64(value, column); }
      bool is_null(int column) const { return sqlite3_column_type(value, column) == SQLITE_NULL; }
    };
    inline constexpr char schema[] = R"sql(
CREATE TABLE catalog_info(singleton INTEGER PRIMARY KEY CHECK(singleton=1), version INTEGER NOT NULL CHECK(version=1), identity TEXT NOT NULL CHECK(length(identity)=32), policy BLOB NOT NULL) STRICT;
CREATE TABLE operations(id BLOB PRIMARY KEY, kind TEXT NOT NULL, request BLOB NOT NULL, outcome BLOB NOT NULL) STRICT;
CREATE TABLE owners(kind TEXT NOT NULL CHECK(kind IN('attempt','save','reader')), id BLOB NOT NULL, PRIMARY KEY(kind,id)) STRICT;
CREATE TABLE attempts(id TEXT PRIMARY KEY CHECK(length(id)=32), owner BLOB NOT NULL UNIQUE) STRICT;
CREATE TABLE objects(id TEXT PRIMARY KEY CHECK(length(id)=32), kind INTEGER NOT NULL CHECK(kind IN(0,1)), attempt TEXT NOT NULL REFERENCES attempts(id), bytes INTEGER, crc INTEGER, barrier INTEGER,
 CHECK((bytes IS NULL AND crc IS NULL AND barrier IS NULL) OR (bytes IS NOT NULL AND crc IS NOT NULL AND barrier IS NOT NULL AND bytes>=96 AND crc>=0 AND crc<=4294967295 AND barrier IN(0,1)))) STRICT;
CREATE TABLE pairs(index_id TEXT PRIMARY KEY REFERENCES objects(id), native_id TEXT NOT NULL REFERENCES objects(id), target_native TEXT, target_index TEXT, native_count INTEGER NOT NULL CHECK(native_count>=0), borrowed_count INTEGER NOT NULL CHECK(borrowed_count>=0), virtual_count INTEGER NOT NULL CHECK(virtual_count>=0),
 UNIQUE(native_id,index_id), FOREIGN KEY(target_native,target_index) REFERENCES pairs(native_id,index_id), CHECK((target_native IS NULL)=(target_index IS NULL)), CHECK(native_count<=virtual_count AND borrowed_count=virtual_count-native_count)) STRICT;
CREATE TABLE owner_objects(owner_kind TEXT NOT NULL, owner_id BLOB NOT NULL, object_id TEXT NOT NULL REFERENCES objects(id), PRIMARY KEY(owner_kind,owner_id,object_id), FOREIGN KEY(owner_kind,owner_id) REFERENCES owners(kind,id)) STRICT;
CREATE TABLE owner_roots(owner_kind TEXT NOT NULL, owner_id BLOB NOT NULL, native_id TEXT NOT NULL, index_id TEXT NOT NULL, PRIMARY KEY(owner_kind,owner_id,native_id,index_id), FOREIGN KEY(owner_kind,owner_id) REFERENCES owners(kind,id), FOREIGN KEY(native_id,index_id) REFERENCES pairs(native_id,index_id)) STRICT;
CREATE TABLE saves(name BLOB PRIMARY KEY, native_id TEXT NOT NULL, index_id TEXT NOT NULL, FOREIGN KEY(native_id,index_id) REFERENCES pairs(native_id,index_id)) STRICT;
CREATE TRIGGER sealed_immutable BEFORE UPDATE ON objects WHEN OLD.bytes IS NOT NULL OR NEW.id<>OLD.id OR NEW.kind<>OLD.kind OR NEW.attempt<>OLD.attempt BEGIN SELECT RAISE(ABORT,'immutable object'); END;
)sql";
    // Version 1 is retained verbatim for old-catalog validation. No open path
    // rewrites it. Only newly created catalogs use the version 2 extension.
    inline std::string schema_for(unsigned version) {
      std::string result = schema;
      if (version == 1) return result;
      if (version != 2) throw std::invalid_argument("unsupported Everett catalog version");
      auto replace = [&](std::string_view before, std::string_view after) {
        result.replace(result.find(before), before.size(), after);
      };
      replace("CHECK(version=1)", "CHECK(version=2)");
      replace("IN('attempt','save','reader')", "IN('attempt','save','reader','timeline')");
      result.insert(result.find("CREATE TRIGGER sealed_immutable"), R"sql(CREATE TABLE timelines(name BLOB PRIMARY KEY, source_name BLOB, source_generation INTEGER,
 CHECK((source_name IS NULL)=(source_generation IS NULL)), FOREIGN KEY(source_name,source_generation) REFERENCES timeline_generations(name,generation)) STRICT;
CREATE TABLE timeline_generations(name BLOB NOT NULL REFERENCES timelines(name), generation INTEGER NOT NULL CHECK(generation>=0), native_id TEXT NOT NULL, index_id TEXT NOT NULL, owner_kind TEXT NOT NULL CHECK(owner_kind='timeline'), owner_id BLOB NOT NULL UNIQUE,
 PRIMARY KEY(name,generation), FOREIGN KEY(owner_kind,owner_id,native_id,index_id) REFERENCES owner_roots(owner_kind,owner_id,native_id,index_id)) STRICT;
)sql");
      return result;
    }
  }

  // Insert-only durable metadata over an existing trusted local directory.
  // Separate connections may contend; a single handle must not be used by
  // concurrent callers. No method deletes an object, owner, operation or save.
  // An uncertain storage/COMMIT outcome permanently disables this handle.
  template <class P, class Ops = sqlite_catalog_ops> struct sqlite_catalog {
    static_assert(std::is_nothrow_move_constructible_v<Ops>);
    static_assert(noexcept(std::declval<Ops &>().commit(std::declval<sqlite3 *>())));
    using policy_type = P;
    using operation_type = catalog_operation;
    sqlite_catalog(sqlite_catalog const &) = delete;
    sqlite_catalog & operator=(sqlite_catalog const &) = delete;
    sqlite_catalog(sqlite_catalog && other) noexcept
      : db_(std::exchange(other.db_, nullptr)), root_(std::move(other.root_)),
        ops_(std::move(other.ops_)), poisoned_(other.poisoned_), schema_version_(other.schema_version_) {}
    sqlite_catalog & operator=(sqlite_catalog &&) = delete;
    ~sqlite_catalog() { if (db_) sqlite3_close_v2(db_); }

    static sqlite_catalog create(std::filesystem::path const & root, object_id const & identity,
        catalog_options options = {}, Ops ops = {}) {
      validate_options(options);
      catalog_detail::bytes descriptor; catalog_detail::identity(descriptor, identity);
      auto location = std::filesystem::canonical(root);
#if defined(__APPLE__) || defined(__linux__)
      // Reserve this catalog name without following a pre-existing symlink.
      auto path = location / "catalog.sqlite3";
      int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd < 0) throw std::system_error(errno, std::generic_category(), "create Everett catalog");
      if (::close(fd)) throw std::system_error(errno, std::generic_category(), "close new Everett catalog");
      auto result = connect(location, options, std::move(ops));
      result.transaction("", "initialize", {}, [&] {
        catalog_detail::exec(result.db_, catalog_detail::schema_for(2).c_str());
        catalog_detail::statement insert(result.db_, "INSERT INTO catalog_info VALUES(1,2,?,?)");
        insert.text(1, identity.hex()); insert.blob(2, policy()); insert.done();
        // Immutable tables remain readable through ordinary SQL tooling.
        for (auto table : immutable_tables) {
          for (auto action : {"UPDATE", "DELETE"}) {
            std::string sql = "CREATE TRIGGER immutable_" + std::string(table) + "_" + action +
              " BEFORE " + action + " ON " + table + " BEGIN SELECT RAISE(ABORT,'immutable catalog row'); END";
            catalog_detail::exec(result.db_, sql.c_str());
          }
        }
        catalog_detail::exec(result.db_, "CREATE TRIGGER objects_no_delete BEFORE DELETE ON objects BEGIN SELECT RAISE(ABORT,'retained object'); END");
        return catalog_detail::bytes{};
      }, false);
      // SQLite's transaction is not a substitute for retaining this new name.
      posix_object_ops barriers;
      int directory = barriers.open_root(location);
      if (directory < 0) throw std::system_error(errno, std::generic_category(), "open catalog directory");
      if (barriers.sync_directory(directory)) {
        int error = errno; barriers.close(directory);
        throw std::system_error(error, std::generic_category(), "sync catalog directory; initialization outcome unknown");
      }
      if (barriers.close(directory))
        throw std::system_error(errno, std::generic_category(), "close catalog directory; initialization outcome unknown");
      return result;
#else
      (void)location; (void)options; (void)ops;
      throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "Everett catalog creation requires POSIX directory barriers");
#endif
    }
    static sqlite_catalog open(std::filesystem::path const & root, catalog_options options = {}, Ops ops = {}) {
      auto result = connect(std::filesystem::canonical(root), options, std::move(ops));
      result.schema_version_ = result.detect_schema_version();
      result.validate_schema();
      catalog_detail::statement info(result.db_, "SELECT version,identity,policy FROM catalog_info WHERE singleton=1");
      if (!info.row() || info.integer(0) != result.schema_version_ || info.blob(2) != policy())
        throw std::invalid_argument("Everett catalog schema or policy mismatch");
      (void)object_id(info.text(1));
      if (info.row()) throw std::invalid_argument("multiple Everett catalog identities");
      return result;
    }
    bool poisoned() const noexcept { return poisoned_; }
    unsigned schema_version() const noexcept { return schema_version_; }
    static char const * runtime_version() noexcept { return sqlite3_libversion(); }
    static char const * source_id() noexcept { return sqlite3_sourceid(); }
    std::filesystem::path const & root() const & noexcept { return root_; }
    std::filesystem::path const & root() const && = delete;

    std::optional<catalog_operation> lookup_operation(std::string_view op) const {
      require_active();
      return read([&]() -> std::optional<catalog_operation> {
        catalog_detail::statement query(db_, "SELECT kind,request,outcome FROM operations WHERE id=?");
        query.key(1, op);
        if (!query.row()) return std::nullopt;
        return catalog_operation{query.text(0), query.blob(1), query.blob(2)};
      });
    }
    std::optional<blob_identity> find_save(std::string_view name) const {
      require_active();
      return read([&]() -> std::optional<blob_identity> {
        catalog_detail::statement query(db_, "SELECT native_id,index_id FROM saves WHERE name=?");
        query.key(1, name);
        if (!query.row()) return std::nullopt;
        return blob_identity{object_id(query.text(0)), object_id(query.text(1))};
      });
    }

    void reserve(std::string_view op, object_attempt_id const & attempt, std::string_view owner,
        std::span<blob_identity const> inputs, std::span<catalog_object_reservation const> outputs) {
      require_active(); catalog_detail::name(op); catalog_detail::name(owner);
      if (outputs.empty()) throw std::invalid_argument("reservation has no outputs");
      catalog_detail::bytes request;
      catalog_detail::identity(request, object_id(attempt.hex())); catalog_detail::field(request, owner);
      catalog_detail::number(request, inputs.size());
      for (auto const & input : inputs) catalog_detail::pair(request, input);
      catalog_detail::number(request, outputs.size());
      for (auto const & output : outputs) {
        catalog_detail::identity(request, output.object); (void)file_extension(output.kind);
        catalog_detail::number(request, kind(output.kind));
      }
      transaction(op, "reserve", request, [&] {
        add_owner("attempt", owner);
        catalog_detail::statement job(db_, "INSERT INTO attempts VALUES(?,?)");
        job.text(1, attempt.hex()); job.key(2, owner); job.done();
        for (auto const & input : inputs) add_root("attempt", owner, input);
        for (auto const & output : outputs) {
          catalog_detail::statement insert(db_, "INSERT INTO objects(id,kind,attempt) VALUES(?,?,?)");
          insert.text(1, output.object.hex()); insert.integer(2, kind(output.kind)); insert.text(3, attempt.hex()); insert.done();
          catalog_detail::statement pin(db_, "INSERT INTO owner_objects VALUES('attempt',?,?)");
          pin.key(1, owner); pin.text(2, output.object.hex()); pin.done();
        }
        return catalog_detail::bytes{};
      });
    }

    void record_sealed(std::string_view op, object_seal_receipt const & receipt) {
      require_active(); catalog_detail::name(op);
      catalog_detail::bytes request;
      catalog_detail::identity(request, receipt.object);
      catalog_detail::identity(request, object_id(receipt.attempt.hex()));
      catalog_detail::number(request, receipt.bytes); catalog_detail::number(request, receipt.body_crc32c);
      auto barrier = static_cast<unsigned>(receipt.barrier);
      if (barrier > 1) throw std::invalid_argument("unsupported seal barrier");
      catalog_detail::number(request, barrier);
      auto path = std::filesystem::canonical(receipt.path);
      // Store a relative path in replay descriptors so relocating the complete
      // backing directory does not turn the same receipt into a new operation.
      catalog_detail::field(request, path.lexically_relative(root_).generic_string());
      transaction(op, "seal", request, [&] {
        catalog_detail::statement reserved(db_, "SELECT kind,attempt,bytes FROM objects WHERE id=?");
        reserved.text(1, receipt.object.hex());
        if (!reserved.row() || reserved.text(1) != receipt.attempt.hex() || !reserved.is_null(2))
          throw std::invalid_argument("object is not this attempt's unsealed reservation");
        auto expected = reserved.integer(0) == 0 ? file_kind::native_blob : file_kind::fractional_index;
        if (path != root_ / object_path(receipt.object, expected))
          throw std::invalid_argument("seal receipt names another catalog path");
        // Header-only evidence check. A caller's successful barrier receipt is
        // an attestation; reading bytes cannot establish or repair durability.
        auto mapping = mapped_file::open(path);
        auto slice = mapping.slice(0, mapping.size());
        auto bytes = slice.bytes();
        auto header = decode_file_header<P>(bytes);
        if (header.kind != expected || receipt.bytes != bytes.size() ||
            file_detail::total_bytes<P>(header.extent) != bytes.size() ||
            file_detail::get(bytes, 64, 4) != receipt.body_crc32c)
          throw std::invalid_argument("seal receipt disagrees with object envelope");
        catalog_detail::statement update(db_, "UPDATE objects SET bytes=?,crc=?,barrier=? WHERE id=?");
        update.integer(1, catalog_detail::integer(receipt.bytes)); update.integer(2, receipt.body_crc32c);
        update.integer(3, barrier); update.text(4, receipt.object.hex()); update.done();
        return catalog_detail::bytes{};
      });
    }

    // The supplied head names catalog paths, which are reopened and pinned for
    // this admission. Normal admission checks metadata; scan explicitly checks
    // readable contents and exact target samples before taking the writer lock.
    void register_chain(std::string_view op, mapped_query_root<P> const & source,
        catalog_admission admission = catalog_admission::trusted) {
      require_active(); catalog_detail::name(op);
      if (admission != catalog_admission::trusted && admission != catalog_admission::scan)
        throw std::invalid_argument("unsupported catalog admission mode");
      auto head = source.head();
      if (!head) throw std::invalid_argument("empty catalog query root");
      auto opened = open_mapped_query<P>(root_, head->identity());
      if (admission == catalog_admission::scan) opened.head()->scan();
      std::vector<typename mapped_blob<P>::pair_type> chain;
      catalog_detail::bytes request;
      catalog_detail::number(request, static_cast<unsigned>(admission));
      for (auto current = opened.head(); current; current = current->target()) {
        chain.push_back(current);
        catalog_detail::pair(request, current->identity());
        catalog_detail::number(request, current->native().size());
        catalog_detail::number(request, current->borrowed().size());
        catalog_detail::number(request, current->virtual_size());
      }
      transaction(op, "register_chain", request, [&] {
        for (auto entry = chain.rbegin(); entry != chain.rend(); ++entry) {
          auto const & pair = **entry;
          require_sealed(pair.identity().native, file_kind::native_blob);
          require_sealed(pair.identity().index, file_kind::fractional_index);
          auto target = pair.target();
          catalog_detail::statement old(db_, "SELECT native_id,target_native,target_index,native_count,borrowed_count,virtual_count FROM pairs WHERE index_id=?");
          old.text(1, pair.identity().index.hex());
          if (old.row()) {
            if (old.text(0) != pair.identity().native.hex() || old.is_null(1) != !target ||
                (target && (old.text(1) != target->identity().native.hex() || old.text(2) != target->identity().index.hex())) ||
                old.integer(3) != catalog_detail::integer(pair.native().size()) ||
                old.integer(4) != catalog_detail::integer(pair.borrowed().size()) ||
                old.integer(5) != catalog_detail::integer(pair.virtual_size()))
              throw std::invalid_argument("registered index identity already has different contents");
            continue;
          }
          catalog_detail::statement insert(db_, "INSERT INTO pairs VALUES(?,?,?,?,?,?,?)");
          insert.text(1, pair.identity().index.hex()); insert.text(2, pair.identity().native.hex());
          if (target) { insert.text(3, target->identity().native.hex()); insert.text(4, target->identity().index.hex()); }
          else { insert.null(3); insert.null(4); }
          insert.integer(5, catalog_detail::integer(pair.native().size()));
          insert.integer(6, catalog_detail::integer(pair.borrowed().size()));
          insert.integer(7, catalog_detail::integer(pair.virtual_size())); insert.done();
        }
        catalog_detail::bytes result; catalog_detail::pair(result, opened.head()->identity()); return result;
      });
    }

    void save(std::string_view op, std::string_view name, blob_identity const & head) {
      require_active(); catalog_detail::name(op); catalog_detail::name(name);
      catalog_detail::bytes request; catalog_detail::field(request, name); catalog_detail::pair(request, head);
      transaction(op, "save", request, [&] {
        catalog_detail::statement prepared(db_, "SELECT virtual_count FROM pairs WHERE native_id=? AND index_id=?");
        prepared.text(1, head.native.hex()); prepared.text(2, head.index.hex());
        if (!prepared.row() || std::uint64_t(prepared.integer(0)) > P::group_size)
          throw std::invalid_argument("save requires a registered prepared head");
        add_owner("save", name); add_root("save", name, head);
        catalog_detail::statement insert(db_, "INSERT INTO saves VALUES(?,?,?)");
        insert.key(1, name); insert.text(2, head.native.hex()); insert.text(3, head.index.hex()); insert.done();
        catalog_detail::bytes result; catalog_detail::pair(result, head); return result;
      });
    }
    catalog_saved_root acquire_save(std::string_view op, std::string_view name, std::string_view reader_owner) {
      require_active(); catalog_detail::name(op); catalog_detail::name(name); catalog_detail::name(reader_owner);
      catalog_detail::bytes request; catalog_detail::field(request, name); catalog_detail::field(request, reader_owner);
      transaction(op, "acquire_save", request, [&] {
        auto head = find_save(name);
        if (!head) throw std::invalid_argument("unknown Everett save");
        add_owner("reader", reader_owner); add_root("reader", reader_owner, *head);
        catalog_detail::bytes result; catalog_detail::pair(result, *head); return result;
      });
      // Saves are immutable. Exact replay therefore resolves the same head.
      auto head = find_save(name);
      if (!head) throw std::logic_error("committed save disappeared");
      return {*head, std::string(reader_owner)};
    }

    std::optional<catalog_timeline_head> find_timeline(std::string_view name) const {
      require_timelines(); catalog_detail::name(name);
      return read([&] { return timeline_at(name); });
    }

    catalog_timeline_head create_timeline(std::string_view op, std::string_view name,
        blob_identity const & head) {
      require_timelines(); catalog_detail::name(op); catalog_detail::name(name);
      catalog_detail::bytes request; catalog_detail::field(request, name); catalog_detail::pair(request, head);
      auto outcome = transaction(op, "create_timeline", request, [&] {
        require_prepared(head);
        catalog_detail::statement insert(db_, "INSERT INTO timelines(name) VALUES(?)");
        insert.key(1, name); insert.done();
        auto result = add_generation(name, 0, head);
        catalog_detail::bytes bytes; catalog_detail::timeline(bytes, result); return bytes;
      });
      return decode_timeline(outcome);
    }

    // Fork precisely the supplied historical generation. Advancing its source
    // later cannot change the selected root or the result of operation replay.
    catalog_timeline_head fork_timeline(std::string_view op, std::string_view name,
        catalog_timeline_head const & source) {
      require_timelines(); catalog_detail::name(op); catalog_detail::name(name);
      validate_timeline(source);
      catalog_detail::bytes request; catalog_detail::field(request, name); catalog_detail::timeline(request, source);
      auto outcome = transaction(op, "fork_timeline", request, [&] {
        auto actual = timeline_at(source.name, source.generation);
        if (!actual || *actual != source) throw std::invalid_argument("fork source is not this exact timeline generation");
        require_prepared(source.head);
        catalog_detail::statement insert(db_, "INSERT INTO timelines VALUES(?,?,?)");
        insert.key(1, name); insert.key(2, source.name); insert.integer(3, catalog_detail::integer(source.generation)); insert.done();
        auto result = add_generation(name, 0, source.head);
        catalog_detail::bytes bytes; catalog_detail::timeline(bytes, result); return bytes;
      });
      return decode_timeline(outcome);
    }

    // Compare the complete expected generation under the SQLite writer lock.
    // A stale comparison is itself a committed, replay-stable outcome. Every
    // successful publication appends a generation, even if the root is unchanged.
    catalog_timeline_publication publish_timeline(std::string_view op,
        catalog_timeline_head const & expected, blob_identity const & candidate) {
      require_timelines(); catalog_detail::name(op); validate_timeline(expected);
      catalog_detail::bytes request; catalog_detail::timeline(request, expected); catalog_detail::pair(request, candidate);
      auto outcome = transaction(op, "publish_timeline", request, [&] {
        auto current = timeline_at(expected.name);
        if (!current) throw std::invalid_argument("unknown Everett timeline");
        bool published = *current == expected;
        if (published) {
          require_prepared(candidate);
          if (expected.generation == std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            throw std::length_error("Everett timeline generation exhausted");
          current = add_generation(expected.name, expected.generation + 1, candidate);
        }
        catalog_detail::bytes bytes; catalog_detail::number(bytes, published); catalog_detail::timeline(bytes, *current); return bytes;
      });
      return read([&] {
        catalog_detail::outcome_reader reader{outcome};
        auto published = reader.number();
        if (published > 1) reader.invalid();
        auto head = reader.timeline(); reader.end();
        return catalog_timeline_publication{published != 0, std::move(head)};
      });
    }

  private:
    sqlite3 * db_ = nullptr;
    std::filesystem::path root_;
    Ops ops_;
    mutable bool poisoned_ = false;
    unsigned schema_version_ = 2;
    inline static constexpr char const * immutable_tables[] = {
      "catalog_info", "operations", "owners", "attempts", "pairs", "owner_objects", "owner_roots", "saves",
      "timelines", "timeline_generations"
    };
    sqlite_catalog(sqlite3 * db, std::filesystem::path root, Ops ops)
      : db_(db), root_(std::move(root)), ops_(std::move(ops)) {}
    void require_active() const {
      if (!db_ || poisoned_) throw std::logic_error("Everett catalog handle is inactive or poisoned");
    }
    void require_timelines() const {
      require_active();
      if (schema_version_ != 2) throw std::logic_error("Everett timelines require catalog version 2; no automatic migration");
    }
    static void validate_timeline(catalog_timeline_head const & value) {
      catalog_detail::name(value.name); catalog_detail::name(value.owner);
      (void)catalog_detail::integer(value.generation);
    }
    catalog_timeline_head decode_timeline(catalog_detail::bytes const & bytes) const {
      return read([&] {
        catalog_detail::outcome_reader reader{bytes};
        auto head = reader.timeline(); reader.end(); return head;
      });
    }
    std::optional<catalog_timeline_head> timeline_at(std::string_view name,
        std::optional<std::uint64_t> generation = {}) const {
      catalog_detail::statement query(db_, generation ?
        "SELECT generation,native_id,index_id,owner_id FROM timeline_generations WHERE name=? AND generation=?" :
        "SELECT generation,native_id,index_id,owner_id FROM timeline_generations WHERE name=? ORDER BY generation DESC LIMIT 1");
      query.key(1, name);
      if (generation) query.integer(2, catalog_detail::integer(*generation));
      if (!query.row()) return std::nullopt;
      return catalog_timeline_head{std::string(name), std::uint64_t(query.integer(0)),
        {object_id(query.text(1)), object_id(query.text(2))}, query.key(3)};
    }
    void require_prepared(blob_identity const & head) const {
      catalog_detail::statement query(db_, "SELECT virtual_count FROM pairs WHERE native_id=? AND index_id=?");
      query.text(1, head.native.hex()); query.text(2, head.index.hex());
      if (!query.row() || std::uint64_t(query.integer(0)) > P::group_size)
        throw std::invalid_argument("timeline requires a registered prepared head");
    }
    catalog_timeline_head add_generation(std::string_view name, std::uint64_t generation, blob_identity const & head) {
      catalog_detail::bytes encoded; catalog_detail::field(encoded, name); catalog_detail::number(encoded, generation);
      std::string owner(reinterpret_cast<char const *>(encoded.data()), encoded.size());
      add_owner("timeline", owner); add_root("timeline", owner, head);
      catalog_detail::statement insert(db_, "INSERT INTO timeline_generations VALUES(?,?,?,?, 'timeline',?)");
      insert.key(1, name); insert.integer(2, catalog_detail::integer(generation));
      insert.text(3, head.native.hex()); insert.text(4, head.index.hex()); insert.key(5, owner); insert.done();
      return {std::string(name), generation, head, std::move(owner)};
    }
    template<class F> auto read(F && action) const {
      try { return action(); }
      catch (catalog_error const & error) {
        if (catalog_detail::storage_error(error.code)) poisoned_ = true;
        throw;
      }
    }
    static std::int64_t kind(file_kind value) { return value == file_kind::native_blob ? 0 : 1; }
    static catalog_detail::bytes policy() {
      catalog_detail::bytes result;
      for (auto value : {std::uint64_t(P::unit), P::group_size, P::codec_block_size,
                        std::uint64_t(P::backspace_code), P::backspace_parameter,
                        std::uint64_t(P::fixed_width), P::value_width.value_or(0)})
        catalog_detail::number(result, value);
      return result;
    }
    static void validate_options(catalog_options options) {
      if (sqlite3_libversion_number() < 3051003 || !sqlite3_threadsafe())
        throw std::runtime_error("Everett requires thread-safe SQLite 3.51.3 or later");
      if (options.busy_timeout_ms < 0) throw std::invalid_argument("negative SQLite busy timeout");
    }
    static sqlite_catalog connect(std::filesystem::path const & root, catalog_options options, Ops ops) {
      validate_options(options);
      sqlite3 * db = nullptr;
      auto path = (root / "catalog.sqlite3").string();
      auto code = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOFOLLOW, nullptr);
      sqlite_catalog result(db, root, std::move(ops));
      if (code != SQLITE_OK) catalog_detail::fail(db, code);
      if (!sqlite3_db_mutex(db))
        throw std::runtime_error("Everett requires a serialized SQLite connection");
      sqlite3_extended_result_codes(db, 1);
      code = sqlite3_busy_timeout(db, options.busy_timeout_ms);
      if (code != SQLITE_OK) catalog_detail::fail(db, code);
      int setting = 0;
      code = sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, &setting);
      if (code != SQLITE_OK || setting != 1) catalog_detail::fail(db, code == SQLITE_OK ? SQLITE_ERROR : code);
      catalog_detail::exec(db, "PRAGMA trusted_schema=OFF; PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL; PRAGMA fullfsync=ON; PRAGMA checkpoint_fullfsync=ON;");
      catalog_detail::statement journal(db, "PRAGMA journal_mode=WAL");
      if (!journal.row() || journal.text(0) != "wal") throw std::runtime_error("Everett requires SQLite WAL mode");
      for (auto sql : {"PRAGMA foreign_keys", "PRAGMA synchronous", "PRAGMA fullfsync", "PRAGMA checkpoint_fullfsync", "PRAGMA trusted_schema"}) {
        catalog_detail::statement check(db, sql);
        auto expected = std::string_view(sql) == "PRAGMA synchronous" ? 2 :
                        std::string_view(sql) == "PRAGMA trusted_schema" ? 0 : 1;
        if (!check.row() || check.integer(0) != expected)
          throw std::runtime_error("Everett SQLite persistence setting rejected");
      }
      return result;
    }
    void definition(std::string_view type, std::string_view name, std::string_view expected) const {
      catalog_detail::statement query(db_, "SELECT sql FROM sqlite_schema WHERE type=? AND name=?");
      query.text(1, type); query.text(2, name);
      if (!query.row() || query.text(0) != expected)
        throw std::invalid_argument("incompatible Everett catalog schema definition");
    }
    unsigned detect_schema_version() const {
      // Do not query an unrecognized replacement table/view to discover its
      // version: establish the canonical bounded singleton shape first.
      catalog_detail::statement query(db_, "SELECT sql FROM sqlite_schema WHERE type='table' AND name='catalog_info'");
      if (query.row()) {
        auto actual = query.text(0);
        for (unsigned version : {1, 2}) {
          auto expected = catalog_detail::schema_for(version);
          auto first = expected.find("CREATE TABLE catalog_info");
          auto last = expected.find(';', first);
          if (actual == expected.substr(first, last - first)) return version;
        }
      }
      throw std::invalid_argument("incompatible Everett catalog version table");
    }
    void validate_schema() const {
      // Validate only a bounded schema description, never external payloads or
      // all catalog rows. SQLite preserves these canonical CREATE definitions.
      auto schema = catalog_detail::schema_for(schema_version_);
      std::string_view source = schema;
      while (true) {
        auto begin = source.find("CREATE TABLE ");
        if (begin == std::string_view::npos) break;
        source.remove_prefix(begin);
        auto end = source.find(';');
        auto name_end = source.find('(');
        definition("table", source.substr(13, name_end - 13), source.substr(0, end));
        source.remove_prefix(end + 1);
      }
      auto first = source.find("CREATE TRIGGER sealed_immutable");
      auto last = source.rfind("END;");
      definition("trigger", "sealed_immutable", source.substr(first, last + 3 - first));
      for (auto table : immutable_tables) {
        if (schema_version_ == 1 && (std::string_view(table) == "timelines" || std::string_view(table) == "timeline_generations")) continue;
        for (auto action : {"UPDATE", "DELETE"}) {
          std::string name = "immutable_" + std::string(table) + "_" + action;
          std::string sql = "CREATE TRIGGER " + name + " BEFORE " + action + " ON " + table +
            " BEGIN SELECT RAISE(ABORT,'immutable catalog row'); END";
          definition("trigger", name, sql);
        }
      }
      definition("trigger", "objects_no_delete", "CREATE TRIGGER objects_no_delete BEFORE DELETE ON objects BEGIN SELECT RAISE(ABORT,'retained object'); END");
      catalog_detail::statement triggers(db_, schema_version_ == 1 ?
        "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND tbl_name IN('catalog_info','operations','owners','attempts','objects','pairs','owner_objects','owner_roots','saves')" :
        "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND tbl_name IN('catalog_info','operations','owners','attempts','objects','pairs','owner_objects','owner_roots','saves','timelines','timeline_generations')");
      if (!triggers.row() || triggers.integer(0) != (schema_version_ == 1 ? 18 : 22))
        throw std::invalid_argument("unexpected trigger on Everett catalog tables");
    }
    void add_owner(std::string_view kind, std::string_view name) {
      catalog_detail::statement insert(db_, "INSERT INTO owners VALUES(?,?)");
      insert.text(1, kind); insert.key(2, name); insert.done();
    }
    void add_root(std::string_view kind, std::string_view owner, blob_identity const & pair) {
      catalog_detail::statement insert(db_, "INSERT INTO owner_roots VALUES(?,?,?,?)");
      insert.text(1, kind); insert.key(2, owner); insert.text(3, pair.native.hex()); insert.text(4, pair.index.hex()); insert.done();
    }
    void require_sealed(object_id const & id, file_kind expected) {
      catalog_detail::statement query(db_, "SELECT kind,bytes,crc FROM objects WHERE id=?");
      query.text(1, id.hex());
      if (!query.row() || query.integer(0) != kind(expected) || query.is_null(1))
        throw std::invalid_argument("chain refers to an unsealed or wrongly typed object");
      auto mapping = mapped_file::open(root_ / object_path(id, expected));
      auto slice = mapping.slice(0, mapping.size());
      auto bytes = slice.bytes();
      auto header = decode_file_header<P>(bytes);
      if (header.kind != expected || query.integer(1) != catalog_detail::integer(bytes.size()) ||
          file_detail::total_bytes<P>(header.extent) != bytes.size() || std::uint64_t(query.integer(2)) != file_detail::get(bytes, 64, 4))
        throw std::invalid_argument("sealed catalog metadata disagrees with object");
    }
    template <class F> catalog_detail::bytes transaction(std::string_view op, std::string_view kind,
        catalog_detail::bytes const & request, F && apply, bool record = true) {
      require_active();
      if (record) catalog_detail::name(op);
      bool began = false, committing = false;
      try {
        catalog_detail::exec(db_, "BEGIN IMMEDIATE"); began = true;
        if (record) {
          if (auto old = lookup_operation(op)) {
            if (old->kind != kind || old->request != request)
              throw std::invalid_argument("Everett operation identity reused with a different request");
            catalog_detail::exec(db_, "ROLLBACK");
            return std::move(old->outcome);
          }
        }
        auto result = apply();
        if (record) {
          catalog_detail::statement insert(db_, "INSERT INTO operations VALUES(?,?,?,?)");
          insert.key(1, op); insert.text(2, kind); insert.blob(3, request); insert.blob(4, result); insert.done();
        }
        committing = true;
        auto code = ops_.commit(db_);
        if (code != SQLITE_OK) catalog_detail::fail(db_, code);
        if (!sqlite3_get_autocommit(db_)) catalog_detail::fail(db_, SQLITE_PROTOCOL);
        return result;
      } catch (catalog_error const & error) {
        bool uncertain = committing || catalog_detail::storage_error(error.code);
        if (began && !sqlite3_get_autocommit(db_) && sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK)
          uncertain = true;
        poisoned_ = poisoned_ || uncertain;
        throw catalog_error(error.what(), error.code, std::string(op), uncertain);
      } catch (...) {
        if (committing) poisoned_ = true;
        if (began && !sqlite3_get_autocommit(db_) && sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK)
          poisoned_ = true;
        throw;
      }
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Stores retained roots, timeline generations and exact file graphs in optional SQLite metadata.
 */
