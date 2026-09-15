/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests durable COLA graph admission, exact replay and timeline selection.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sqlite_catalog.h>

#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
namespace {
  using namespace diet;
  void require(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid COLA catalog operation accepted");
  }
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  object_attempt_id attempt(unsigned n) { return object_attempt_id(id(n).hex()); }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-catalog-cola-XXXXXX").string();
      auto result = ::mkdtemp(name.data());
      if (!result) throw std::runtime_error("mkdtemp");
      root = result;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  struct connection {
    sqlite3 * db = nullptr;
    explicit connection(std::filesystem::path const & root) {
      require(sqlite3_open((root / "catalog.sqlite3").c_str(), &db) == SQLITE_OK, "test SQLite open");
    }
    ~connection() { sqlite3_close(db); }
    void exec(std::string const & sql) {
      require(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "test SQL execution");
    }
    std::int64_t scalar(char const * sql) {
      sqlite3_stmt * query = nullptr;
      require(sqlite3_prepare_v2(db, sql, -1, &query, nullptr) == SQLITE_OK, "test SQL prepare");
      auto code = sqlite3_step(query); auto result = sqlite3_column_int64(query, 0);
      sqlite3_finalize(query); require(code == SQLITE_ROW, "test SQL row"); return result;
    }
  };
  template <class P> std::vector<profile_record> rows(unsigned salt, unsigned count) {
    std::vector<profile_record> result;
    for (unsigned i = 0; i < count; ++i) {
      char key[16]; std::snprintf(key, sizeof key, "%04u", i * 2 + salt % 2);
      char value[32]; std::snprintf(value, sizeof value, "%u:%u", salt, i);
      result.push_back({bit_string::from_bytes(key), P::fixed_width ? bit_string{} : bit_string::from_bytes(value)});
    }
    return result;
  }
  bool bits_equal(bit_view a, bit_view b) {
    if (a.size() != b.size()) return false;
    for (std::uint64_t i = 0; i < a.size(); ++i) {
      auto bit = [i](bit_view x) {
        auto p = x.offset() + i;
        return (std::to_integer<unsigned>(x.storage()[p / 8]) >> (7 - p % 8)) & 1;
      };
      if (bit(a) != bit(b)) return false;
    }
    return true;
  }
  template <class P> struct fixture {
    blob_identity head;
    std::vector<profile_record> logical;
    std::vector<blob_identity> nodes;
    std::vector<std::optional<object_id>> secondaries;
    std::optional<object_seal_receipt> omitted;
  };
  template <class P> fixture<P> persist(sqlite_catalog<P> & db, unsigned seed,
                                       bool omit_secondary = false, bool wrong_secondary = false) {
    using node = cola_index<P>;
    auto first = rows<P>(seed, 19), side = rows<P>(seed + 1, 11), local = rows<P>(seed + 2, 5);
    auto zero = rows<P>(seed + 3, 9);
    auto side_owner = std::make_shared<profile_array<P> const>(profile_array<P>::build(side));
    auto main = std::make_shared<node const>(node::build(first));
    auto local_node = std::make_shared<node const>(node::build(local, main, side_owner));
    auto root = node::prepare_root(local_node,
      std::make_shared<profile_array<P> const>(profile_array<P>::build(zero)));
    std::vector<typename node::pair_type> chain;
    fixture<P> result{{id(seed), id(seed + 1)}, {}, {}, {}, {}};
    for (auto const * records : {&first, &side, &local, &zero})
      result.logical.insert(result.logical.end(), records->begin(), records->end());
    std::vector<catalog_object_reservation> outputs;
    for (auto current = root; current; current = current->main_target()) {
      auto at = unsigned(chain.size()); chain.push_back(current);
      result.nodes.push_back({id(seed + at * 3), id(seed + at * 3 + 1)});
      result.secondaries.push_back(current->secondary_target() ? std::optional{id(seed + at * 3 + 2)} : std::nullopt);
      outputs.push_back({result.nodes.back().native, file_kind::native_blob});
      outputs.push_back({result.nodes.back().index, file_kind::fractional_index});
      if (result.secondaries.back()) outputs.push_back({*result.secondaries.back(), file_kind::native_blob});
    }
    auto suffix = std::to_string(seed); auto job = attempt(seed + 99);
    db.reserve("reserve-" + suffix, job, "owner-" + suffix, {}, outputs);
    for (std::size_t i = chain.size(); i-- > 0;) {
      auto tag = suffix + "-" + std::to_string(i);
      db.record_sealed("native-" + tag,
        encode_native_sections(chain[i]->native()).seal(db.root(), result.nodes[i].native, job));
      if (result.secondaries[i]) {
        auto owner = chain[i]->secondary_target();
        std::optional<profile_array<P>> wrong;
        if (wrong_secondary) {
          auto changed = rows<P>(seed + 20, unsigned(owner->size()));
          // Strictly increasing replacements, all greater than the declared source samples.
          for (auto & record : changed) {
            auto prefixed = bit_string::from_bytes("x");
            for (auto b : record.key.bytes) prefixed.bytes.push_back(b);
            prefixed.bit_size += record.key.bit_size; record.key = std::move(prefixed);
          }
          wrong.emplace(profile_array<P>::build(changed));
        }
        auto encoded = encode_native_sections(wrong ? *wrong : *owner);
        auto receipt = encoded.seal(db.root(), *result.secondaries[i], job);
        if (omit_secondary && !result.omitted) result.omitted.emplace(std::move(receipt));
        else db.record_sealed("secondary-" + tag, receipt);
      }
      auto index = encode_cola_sections(*chain[i], result.nodes[i].native,
        i + 1 < chain.size() ? std::optional{result.nodes[i + 1]} : std::nullopt, result.secondaries[i]);
      db.record_sealed("index-" + tag, index.seal(db.root(), result.nodes[i].index, job));
    }
    return result;
  }
  template <class P> void query(fixture<P> const & fixture, std::filesystem::path const & root) {
    auto mapped = open_mapped_cola_query<P>(root, fixture.head);
    mapped.head()->scan();
    for (auto const & sought : fixture.logical) {
      std::vector<bit_string> expected, actual;
      for (auto const & row : fixture.logical)
        if (bits_equal(row.key.view(), sought.key.view())) expected.push_back(row.value);
      auto cursor = mapped.cursor(sought.key.view());
      unsigned steps = 0;
      while (!cursor.done()) {
        require(++steps < 100, "COLA query stalled"); cursor.step(1);
        while (cursor.has_match()) actual.push_back(cursor.take_match().value);
      }
      require(actual.size() == expected.size(), "COLA publication lost or duplicated native occurrence");
      for (auto const & value : actual) {
        auto found = std::find_if(expected.begin(), expected.end(), [&](auto const & x) { return bits_equal(x.view(), value.view()); });
        require(found != expected.end(), "COLA published value mismatch"); expected.erase(found);
      }
    }
  }
  template <class P> blob_identity linear(sqlite_catalog<P> & db, unsigned seed, bool register_now = true) {
    auto source = profile_blob<P>::build(rows<P>(seed, 2));
    blob_identity ids{id(seed), id(seed + 1)}; auto job = attempt(seed + 99);
    std::array outputs{catalog_object_reservation{ids.native, file_kind::native_blob},
      catalog_object_reservation{ids.index, file_kind::fractional_index}};
    db.reserve("linear-reserve", job, "linear-owner", {}, outputs);
    db.record_sealed("linear-native", encode_native_sections(source.native()).seal(db.root(), ids.native, job));
    db.record_sealed("linear-index", encode_index_sections(source, ids.native).seal(db.root(), ids.index, job));
    auto mapped = open_mapped_query<P>(db.root(), ids);
    if (register_now) {
      db.register_chain("linear-register", mapped, catalog_admission::scan);
      db.register_chain("linear-register", mapped, catalog_admission::scan);
    }
    return ids;
  }
  template <class P> void lifecycle() {
    temporary directory;
    auto db = sqlite_catalog<P>::create_cola(directory.root, id(1));
    require(db.schema_version() == 3, "COLA creation did not choose version 3");
    auto source = persist(db, 100, true);
    auto mapped = open_mapped_cola_query<P>(directory.root, source.head);
    rejects([&] { db.register_chain("register", mapped); });
    require(!db.lookup_operation("register") && !db.poisoned(), "missing seal committed or poisoned");
    require(connection(directory.root).scalar("SELECT count(*) FROM pairs") == 0, "partial COLA registration survived rollback");
    db.record_sealed("omitted-secondary", *source.omitted);
    db.register_chain("register", mapped, catalog_admission::scan);
    auto operation = db.lookup_operation("register");
    db.register_chain("register", mapped, catalog_admission::scan);
    db.register_chain("register-again", mapped);
    rejects([&] { db.register_chain("register", mapped); });
    require(db.lookup_operation("register")->request == operation->request, "replay changed request");
    connection sql(directory.root);
    require(sql.scalar("SELECT count(*) FROM pairs WHERE layout=3") == std::int64_t(source.nodes.size()), "missing COLA rows");
    require(sql.scalar("SELECT count(*) FROM pairs WHERE secondary_native IS NOT NULL") == 2, "missing secondary edges");
    require(sql.scalar("SELECT count(*) FROM pairs WHERE native_count+borrowed_count+secondary_borrowed_count<>virtual_count") == 0, "wrong COLA counts");
    require(sql.scalar("SELECT count(*) FROM pairs p JOIN objects o ON o.id=p.secondary_native WHERE o.kind<>0 OR o.bytes IS NULL") == 0, "secondary not sealed native");
    auto old = linear(db, 1000);
    using namespace std::string_literals;
    auto name = "cola\0';DROP TABLE pairs;--\xff"s;
    db.save("save\0cola"s, name, source.head);
    auto initial = db.create_timeline("timeline\0create"s, name, old);
    auto published = db.publish_timeline("publish\0cola"s, initial, source.head);
    require(published.published && published.head.generation == 1, "COLA timeline publication failed");
    auto fork = db.fork_timeline("fork", "fork", initial);
    require(fork.head == old, "historical linear root not retained");
    auto reader = db.acquire_save("reader", name, "owner\0reader"s);
    require(reader.head == source.head, "saved COLA root pin differs");
    auto reopened = sqlite_catalog<P>::open(directory.root);
    require(reopened.schema_version() == 3 && reopened.find_timeline(name) == published.head && reopened.find_save(name) == source.head, "reopen lost COLA root");
    require(reopened.publish_timeline("publish\0cola"s, initial, source.head) == published, "timeline replay changed");
    require(sql.scalar("SELECT count(*) FROM pragma_foreign_key_check") == 0, "COLA catalog foreign-key failure");
    query(source, directory.root);

    auto malformed = persist(db, 2000, false, true);
    auto bad = open_mapped_cola_query<P>(directory.root, malformed.head);
    rejects([&] { db.register_chain("wrong-secondary", bad, catalog_admission::scan); });
    require(!db.lookup_operation("wrong-secondary"), "wrong secondary sample graph admitted by scan");
    rejects([&] { db.register_chain("register", bad); });
    // Trusted admission deliberately does not authenticate sampled key contents.
    db.register_chain("wrong-trusted", bad);
    require(db.lookup_operation("wrong-trusted").has_value(), "trusted admission unexpectedly scanned samples");
    rejects([&] { mapped_cola_blob<P>::bind(mapped.head()->identity(), mapped.head()->native_object(),
      mapped.head()->index_object(), mapped.head()->main_target(), {}, id(9999)); });
  }
  using policy = storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>;
  void layout_mismatch() {
    temporary directory;
    auto db = sqlite_catalog<policy>::create_cola(directory.root, id(1));
    auto source = persist(db, 100);
    auto mapped = open_mapped_cola_query<policy>(directory.root, source.head);
    auto tail = mapped.head();
    while (tail->main_target()) tail = tail->main_target();
    connection sql(directory.root);
    // Adversarial SQL fixture: a previously recorded identity claims the other
    // layout while retaining the exact native/count fields. Registration must
    // compare the format discriminator as well as those otherwise equal fields.
    auto insert = [&](blob_identity const & ids, std::uint64_t size, unsigned layout) {
      catalog_detail::statement row(sql.db, "INSERT INTO pairs(index_id,native_id,native_count,borrowed_count,virtual_count,layout) VALUES(?,?,?,0,?,?)");
      row.text(1, ids.index.hex()); row.text(2, ids.native.hex());
      row.integer(3, catalog_detail::integer(size)); row.integer(4, catalog_detail::integer(size));
      row.integer(5, layout); row.done();
    };
    insert(tail->identity(), tail->virtual_size(), 2);
    rejects([&] { db.register_chain("wrong-layout-cola", mapped); });
    require(!db.lookup_operation("wrong-layout-cola") && sql.scalar("SELECT count(*) FROM pairs") == 1,
      "COLA layout mismatch committed partial metadata");
    auto ids = linear(db, 1000, false);
    insert(ids, 2, 3);
    auto old = open_mapped_query<policy>(directory.root, ids);
    rejects([&] { db.register_chain("wrong-layout-linear", old); });
    require(!db.lookup_operation("wrong-layout-linear") && !db.poisoned(), "linear layout mismatch committed or poisoned");
  }
  struct fault_ops {
    std::shared_ptr<int> mode;
    int commit(sqlite3 * db) noexcept {
      auto chosen = std::exchange(*mode, 0);
      if (chosen == 1) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return result == SQLITE_OK && chosen == 2 ? SQLITE_IOERR_FSYNC : result;
    }
  };
  void commit_faults() {
    for (int mode : {1, 2}) {
      temporary directory;
      auto db = sqlite_catalog<policy>::create_cola(directory.root, id(1));
      auto source = persist(db, 100);
      auto mapped = open_mapped_cola_query<policy>(directory.root, source.head);
      {
        auto fault = sqlite_catalog<policy, fault_ops>::open(directory.root, {}, {std::make_shared<int>(mode)});
        bool caught = false;
        try { fault.register_chain("uncertain-register", mapped); }
        catch (catalog_error const & e) { caught = true; require(e.outcome_unknown, "COMMIT error not uncertain"); }
        require(caught && fault.poisoned(), "COMMIT failure did not poison");
        rejects([&] { fault.register_chain("uncertain-register", mapped); });
      }
      auto reopened = sqlite_catalog<policy>::open(directory.root);
      require(bool(reopened.lookup_operation("uncertain-register")) == (mode == 2), "lost COMMIT outcome disagrees");
      require(connection(directory.root).scalar("SELECT count(*) FROM pairs") == (mode == 2 ? std::int64_t(source.nodes.size()) : 0), "partial COLA graph committed");
      reopened.register_chain("uncertain-register", mapped);
      reopened.register_chain("uncertain-register", mapped);
      reopened.save("save", "saved", source.head);
      query(source, directory.root);
    }
  }
  void old_versions() {
    temporary directory;
    auto db = sqlite_catalog<policy>::create(directory.root, id(1));
    require(db.schema_version() == 2, "default create changed schema");
    auto source = persist(db, 100);
    auto mapped = open_mapped_cola_query<policy>(directory.root, source.head);
    rejects([&] { db.register_chain("unsupported", mapped); });
    require(!db.lookup_operation("unsupported") && !db.poisoned(), "version rejection changed old catalog");
    // Remove a graph file: version rejection must still happen before opening it.
    std::filesystem::remove(directory.root / object_path(source.head.index, file_kind::fractional_index));
    bool version = false;
    try { db.register_chain("unsupported", mapped); }
    catch (std::logic_error const &) { version = true; }
    require(version && sqlite_catalog<policy>::open(directory.root).schema_version() == 2, "old schema migrated or did file work");
  }
}
#endif
int main() {
#if defined(__APPLE__) || defined(__linux__)
  try {
    lifecycle<policy>();
    lifecycle<diet::storage_policy<diet::profile_unit::bit, diet::fixed_values<0>, 7, diet::exponential_golomb<0>, 15>>();
    commit_faults(); old_versions(); layout_mismatch();
    std::cout << "SQLite COLA catalog checks passed\n";
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
#endif
}
