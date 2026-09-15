/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/sqlite_catalog.h>

#include <cassert>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
namespace {
  using namespace everett;
  using policy = storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 4>;
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  object_attempt_id attempt(unsigned n) { return object_attempt_id(id(n).hex()); }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto base = std::filesystem::temp_directory_path() / "everett-catalog-XXXXXX";
      auto text = base.string();
      auto result = ::mkdtemp(text.data());
      if (!result) throw std::runtime_error("mkdtemp");
      root = result;
    }
    ~temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
  };
  template<class F> void rejects(F && fn) {
    bool rejected = false;
    try { fn(); } catch (std::exception const &) { rejected = true; }
    assert(rejected);
  }
  std::int64_t count(std::filesystem::path const & root, char const * sql) {
    sqlite3 * db = nullptr;
    assert(sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt * query = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &query, nullptr) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    auto result = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query); sqlite3_close(db); return result;
  }
  template<class P> blob_identity persist(sqlite_catalog<P> & catalog) {
    std::vector<profile_record> records;
    for (unsigned i = 0; i != 23; ++i) {
      char text[8]; std::snprintf(text, sizeof text, "k%03u", i);
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
      auto n = static_cast<unsigned>(chain.size());
      chain.push_back(current); identities.push_back({id(10 + 2 * n), id(11 + 2 * n)});
      outputs.push_back({identities.back().native, file_kind::native_blob});
      outputs.push_back({identities.back().index, file_kind::fractional_index});
    }
    catalog.reserve("reserve", attempt(200), "builder", {}, outputs);
    catalog.reserve("reserve", attempt(200), "builder", {}, outputs);
    rejects([&] { catalog.reserve("changed-reservation", attempt(201), "other", {}, outputs); });
    assert(!catalog.poisoned());
    assert(count(catalog.root(), "SELECT count(*) FROM attempts") == 1);
    for (std::size_t i = chain.size(); i-- != 0;) {
      auto native = encode_native_sections(chain[i]->native());
      auto index = encode_index_sections(*chain[i], identities[i].native,
        i + 1 < chain.size() ? std::optional{identities[i + 1]} : std::nullopt);
      auto nreceipt = native.seal(catalog.root(), identities[i].native, attempt(200));
      auto ireceipt = index.seal(catalog.root(), identities[i].index, attempt(200));
      auto nop = "native-" + std::to_string(i), iop = "index-" + std::to_string(i);
      catalog.record_sealed(nop, nreceipt); catalog.record_sealed(nop, nreceipt);
      catalog.record_sealed(iop, ireceipt);
      auto changed = nreceipt; ++changed.body_crc32c;
      rejects([&] { catalog.record_sealed(nop, changed); });
    }
    auto mapped = open_mapped_query<P>(catalog.root(), identities.front());
    catalog.register_chain("register", mapped, catalog_admission::scan);
    catalog.register_chain("register", mapped, catalog_admission::scan);
    rejects([&] { catalog.register_chain("register", mapped); });
    catalog.register_chain("register-metadata", mapped);
    catalog.save("save", "first", identities.front());
    catalog.save("save", "first", identities.front());
    rejects([&] { catalog.save("second-save", "first", identities.front()); });
    assert(!catalog.poisoned());
    auto pin = catalog.acquire_save("reader", "first", "reader-a");
    auto replay = catalog.acquire_save("reader", "first", "reader-a");
    assert(pin.head == replay.head && pin.head == identities.front());
    auto query = open_mapped_query<P>(catalog.root(), pin.head);
    for (auto const & record : records) {
      auto cursor = query.cursor(record.key.view());
      unsigned found = 0;
      while (!cursor.done()) {
        cursor.step(1);
        if (cursor.has_match()) {
          auto match = cursor.take_match();
          assert(compare_bits(match.value.view(), record.value.view()) == 0); ++found;
        }
      }
      assert(found == 1);
    }
    assert(count(catalog.root(), "SELECT count(*) FROM owner_roots WHERE owner_kind='reader'") == 1);
    assert(count(catalog.root(), "SELECT count(*) FROM objects WHERE bytes IS NULL") == 0);
    return identities.front();
  }
  void normal() {
    temporary directory;
    blob_identity identity{id(1), id(2)};
    {
      auto catalog = sqlite_catalog<policy>::create(directory.root, id(1));
      identity = persist(catalog);
      assert(catalog.find_save("first") == identity);
    }
    auto catalog = sqlite_catalog<policy>::open(directory.root);
    assert(catalog.find_save("first") == identity);
    assert(catalog.lookup_operation("save")->kind == "save");
    assert(!catalog.lookup_operation("missing"));
    rejects([&] { (void)sqlite_catalog<policy>::create(directory.root, id(2)); });
    using wrong = storage_policy<profile_unit::byte, variable_values, 7>;
    rejects([&] { (void)sqlite_catalog<wrong>::open(directory.root); });
    // An attempted reservation whose input is missing rolls back everything.
    std::array outputs{catalog_object_reservation{id(900), file_kind::native_blob}};
    std::array missing{blob_identity{id(901), id(902)}};
    rejects([&] { catalog.reserve("invalid-input", attempt(903), "bad-owner", missing, outputs); });
    assert(!catalog.lookup_operation("invalid-input"));
    assert(count(directory.root, "SELECT count(*) FROM objects WHERE id='00000000000000000000000000000384'") == 0);
    assert(!catalog.poisoned());
    std::exception_ptr errors[2];
    auto run = [&](unsigned i) {
      try {
        auto concurrent = sqlite_catalog<policy>::open(directory.root, {5000});
        for (unsigned n = 0; n != 8; ++n) {
          auto name = "concurrent-" + std::to_string(i) + "-" + std::to_string(n);
          concurrent.acquire_save(name, "first", name);
        }
      } catch (...) { errors[i] = std::current_exception(); }
    };
    std::thread a(run, 0), b(run, 1); a.join(); b.join();
    for (auto error : errors) if (error) std::rethrow_exception(error);
    assert(count(directory.root, "SELECT count(*) FROM owner_roots WHERE owner_kind='reader'") == 17);
    auto moved = std::move(catalog);
    rejects([&] { (void)catalog.find_save("first"); });
    assert(moved.find_save("first") == identity);
  }
  struct failing_ops {
    std::shared_ptr<int> mode;
    int commit(sqlite3 * db) noexcept {
      int choice = std::exchange(*mode, 0);
      if (choice == 1) return SQLITE_IOERR_FSYNC;
      int result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return result == SQLITE_OK && choice == 2 ? SQLITE_IOERR_FSYNC : result;
    }
  };
  void failures() {
    temporary directory;
    {
      auto catalog = sqlite_catalog<policy>::create(directory.root, id(1));
      (void)persist(catalog);
    }
    for (int mode : {1, 2}) {
      auto state = std::make_shared<int>(mode);
      auto catalog = sqlite_catalog<policy, failing_ops>::open(directory.root, {}, {state});
      std::string op = "uncertain-" + std::to_string(mode);
      try { catalog.acquire_save(op, "first", op); assert(false); }
      catch (catalog_error const & error) {
        assert(error.outcome_unknown && error.operation == op && error.code == SQLITE_IOERR_FSYNC);
      }
      assert(catalog.poisoned());
      rejects([&] { (void)catalog.acquire_save("after-failure", "first", "blocked"); });
      auto reopened = sqlite_catalog<policy>::open(directory.root);
      assert(bool(reopened.lookup_operation(op)) == (mode == 2));
      auto before = count(directory.root, "SELECT count(*) FROM owner_roots WHERE owner_kind='reader'");
      (void)reopened.acquire_save(op, "first", op);
      assert(count(directory.root, "SELECT count(*) FROM owner_roots WHERE owner_kind='reader'") == before + (mode == 1));
    }
  }
}

int main() {
  normal(); failures();
  {
    temporary directory;
    using bits = everett::storage_policy<everett::profile_unit::bit, everett::fixed_values<0>, 7, everett::golomb<3>, 16>;
    auto catalog = everett::sqlite_catalog<bits>::create(directory.root, id(1));
    (void)persist(catalog);
  }
  std::cout << "SQLite catalog: " << sqlite_catalog<policy>::runtime_version() << '\n';
}

#else
int main() { std::cout << "SQLite catalog creation requires POSIX directory barriers\n"; }
#endif

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises reservation-to-save publication, reopening and ambiguous commit outcomes.
 */
