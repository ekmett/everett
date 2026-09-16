/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks joint native/index acknowledgment, replay and shared-owner publication.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/runtime_graph_sealer.h>
#include <diet/sort_runtime.h>

#include <atomic>
#include <barrier>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
  using namespace diet;
  using P = storage_policy<string_registry, 3>;
  using input_core = typed_engine<P, wrapping_fingerprint_algebra, 256, sort_runtime_family<P>>;

  object_id id(std::uint64_t value) {
    char text[33];
    std::snprintf(text, sizeof text, "%032llx", static_cast<unsigned long long>(value));
    return object_id(text);
  }
  struct ids {
    std::shared_ptr<std::atomic<std::uint64_t>> next = std::make_shared<std::atomic<std::uint64_t>>(1000);
    object_id operator()() { return id(next->fetch_add(1)); }
  };
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-native-pair-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    assert(rejected);
  }
  std::int64_t scalar(std::filesystem::path const & root, char const * sql) {
    sqlite3 * db = nullptr;
    assert(sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt * statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    auto result = sqlite3_column_int64(statement, 0);
    assert(sqlite3_finalize(statement) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    return result;
  }
  void sql(std::filesystem::path const & root, char const * command) {
    sqlite3 * db = nullptr;
    assert(sqlite3_open((root / "catalog.sqlite3").c_str(), &db) == SQLITE_OK);
    auto result = sqlite3_exec(db, command, nullptr, nullptr, nullptr);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(result == SQLITE_OK);
  }
  struct fault_state { bool armed = false, after = false; };
  struct fault_ops {
    std::shared_ptr<fault_state> state;
    int commit(sqlite3 * db) noexcept {
      sqlite3_stmt * statement = nullptr;
      int code = sqlite3_prepare_v2(db, "SELECT kind FROM operations ORDER BY rowid DESC LIMIT 1", -1, &statement, nullptr);
      bool joint = false;
      if (code == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        auto kind = reinterpret_cast<char const *>(sqlite3_column_text(statement, 0));
        joint = kind && std::string_view(kind) == "seal_native_cola_pair";
      }
      sqlite3_finalize(statement);
      if (code != SQLITE_OK) return code;
      if (joint && state->armed) {
        state->armed = false;
        if (!state->after) return SQLITE_IOERR_FSYNC;
        code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        return code == SQLITE_OK ? SQLITE_IOERR_FSYNC : code;
      }
      return sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    }
  };
  template <class Storage> struct family {
    using storage_type = Storage;
    using native_type = typename Storage::native_type;
    using node_type = redundant_node<P, Storage>;
  };
  template <class Storage> using node = typename family<Storage>::node_type;
  template <class Storage> auto singleton(std::string const & key, std::string const & value) {
    auto input = input_core::put(key, value);
    return Storage::singleton(input.records()[0]);
  }
  template <class Storage> auto pair(typename node<Storage>::native_pointer native,
      typename node<Storage>::pair_type main = {}, typename node<Storage>::native_pointer secondary = {}) {
    cola_index_builder<P, typename Storage::native_type, node<Storage>> builder(
      std::move(native), std::move(main), std::move(secondary));
    while (!builder.done()) builder.step(2);
    return node<Storage>::from_built(builder.finish());
  }
  template <class Mapped> void query(std::shared_ptr<Mapped const> const & source,
      std::string const & key, std::vector<std::string> const & values) {
    auto input = input_core::put(key, "");
    auto root = cola_query_root<P, Mapped>::adopt_prepared(source);
    auto cursor = root.cursor(input.records()[0].key.view());
    std::size_t found = 0;
    while (!cursor.done()) {
      if (!cursor.has_match()) cursor.step();
      if (cursor.has_match()) {
        assert(found < values.size());
        auto expected = input_core::put(key, values[found++]);
        assert(cursor.take_match().value == expected.records()[0].value);
      }
    }
    assert(found == values.size());
  }

  template <class Storage> void direct_acknowledgment() {
    using mapped = typename Storage::mapped_pair_type;
    for (unsigned mode = 0; mode != 3; ++mode) {
      temporary dir;
      auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
      auto native = singleton<Storage>("key", "value");
      auto source = pair<Storage>(native);
      blob_identity identity{id(2), id(3)};
      object_attempt_id attempt(id(4).hex());
      std::array outputs{catalog_object_reservation{identity.native, file_kind::native_blob},
        catalog_object_reservation{identity.index, file_kind::fractional_index}};
      catalog.reserve("reserve", attempt, "producer", {}, outputs);
      auto native_receipt = Storage::encode_native(*native->owned()).seal(dir.root, id(2), attempt);
      auto index_receipt = encode_cola_sections(*source->built(), id(2)).seal(dir.root, id(3), attempt);
      if (!mode) {
        for (bool change_native : {false, true}) for (unsigned field = 0; field != 6; ++field) {
          auto n = native_receipt, i = index_receipt;
          auto & bad = change_native ? n : i;
          switch (field) {
            case 0: bad.attempt = object_attempt_id(id(99).hex()); break;
            case 1: ++bad.bytes; break;
            case 2: bad.body_crc32c ^= 1; break;
            case 3: bad.barrier = static_cast<object_sync_barrier>(2); break;
            case 4: bad.path = change_native ? index_receipt.path : native_receipt.path; break;
            case 5: bad.object = id(99); break;
          }
          rejects([&] { catalog.template seal_native_pair<mapped>("bad", identity, n, i); });
          assert(!catalog.lookup_operation("bad") && !catalog.poisoned());
          assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 0);
        }
        // The second UPDATE and pair INSERT must roll the native UPDATE back.
        sql(dir.root, "CREATE TRIGGER reject_pair BEFORE INSERT ON pairs BEGIN SELECT RAISE(ABORT,'test pair rejection'); END");
        rejects([&] { catalog.template seal_native_pair<mapped>("rollback", identity, native_receipt, index_receipt); });
        assert(!catalog.poisoned() && !catalog.lookup_operation("rollback"));
        assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 0);
        sql(dir.root, "DROP TRIGGER reject_pair");
        auto [n, i] = catalog.template seal_native_pair<mapped>("joint", identity, native_receipt, index_receipt);
        n->scan(); i->scan();
        query(mapped::bind(identity, n, i), "key", {"value"});
      } else {
        auto state = std::make_shared<fault_state>(fault_state{true, mode == 2});
        auto faulty = sqlite_catalog<P, fault_ops>::open(dir.root, {}, fault_ops{state});
        bool returned = false;
        try {
          (void)faulty.template seal_native_pair<mapped>("joint", identity, native_receipt, index_receipt);
          returned = true;
        } catch (catalog_error const & error) {
          assert(error.outcome_unknown && error.operation == "joint");
        }
        assert(!returned && faulty.poisoned());
        auto reopened = sqlite_catalog<P>::open(dir.root);
        assert(bool(reopened.lookup_operation("joint")) == (mode == 2));
        assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == (mode == 2 ? 2 : 0));
        assert(scalar(dir.root, "SELECT count(*) FROM pairs") == (mode == 2 ? 1 : 0));
        auto [n, i] = reopened.template seal_native_pair<mapped>("joint", identity, native_receipt, index_receipt);
        query(mapped::bind(identity, n, i), "key", {"value"});
      }
      auto recorded = catalog.lookup_operation("joint");
      assert(recorded && recorded->kind == "seal_native_cola_pair");
      auto [n, i] = catalog.template seal_native_pair<mapped>("joint", identity, native_receipt, index_receipt);
      query(mapped::bind(identity, n, i), "key", {"value"});
      assert(catalog.lookup_operation("joint")->request == recorded->request);
      assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 2);
      assert(scalar(dir.root, "SELECT count(*) FROM pairs") == 1);
      auto bad = native_receipt;
      bad.attempt = object_attempt_id(id(98).hex());
      rejects([&] { catalog.template seal_native_pair<mapped>("joint", identity, bad, index_receipt); });
      rejects([&] { catalog.template seal_native_pair<mapped>("joint", identity, index_receipt, native_receipt); });
      rejects([&] { catalog.template seal_native_pair<mapped>("fresh", identity, native_receipt, index_receipt); });
      assert(!catalog.poisoned() && !catalog.lookup_operation("fresh"));

      // Exact replay still validates both immutable envelopes.
      for (auto const & receipt : {native_receipt, index_receipt}) {
        auto mapping = mapped_file::open(receipt.path);
        auto slice = mapping.slice(0, mapping.size());
        auto bytes = slice.bytes();
        std::array<std::byte, file_detail::header_bytes> original;
        std::copy_n(bytes.begin(), original.size(), original.begin());
        auto altered = encode_file_header(decode_file_header<P>(original), receipt.body_crc32c ^ 1);
        assert(::chmod(receipt.path.c_str(), 0600) == 0);
        int fd = ::open(receipt.path.c_str(), O_WRONLY | O_CLOEXEC);
        assert(fd >= 0);
        assert(::pwrite(fd, altered.data(), altered.size(), 0) == static_cast<ssize_t>(altered.size()));
        rejects([&] { catalog.template seal_native_pair<mapped>("joint", identity, native_receipt, index_receipt); });
        assert(::pwrite(fd, original.data(), original.size(), 0) == static_cast<ssize_t>(original.size()));
        assert(::close(fd) == 0 && ::chmod(receipt.path.c_str(), 0400) == 0);
      }
      catalog.template seal_native_pair<mapped>("joint", identity, native_receipt, index_receipt);
    }
  }

  template <class Storage> void aliases_and_races() {
    using sealer = runtime_store_detail::graph_sealer<P, ids, sqlite_catalog_ops, family<Storage>>;
    temporary dir;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    ids identity;
    sealer seal(catalog, identity);
    auto native = singleton<Storage>("key", "value");
    auto leaf = pair<Storage>(native);
    auto first = seal.ensure_pair(leaf);
    assert(scalar(dir.root, "SELECT count(*) FROM operations") == 2);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == 1);
    assert(seal.ensure_pair(leaf) == first);
    assert(scalar(dir.root, "SELECT count(*) FROM operations") == 2);
    // The dependency's own native aliases the parent's own native.
    auto main_alias = pair<Storage>(native, leaf);
    auto aliased = seal.ensure_pair(main_alias);
    assert(aliased->identity.native == first->identity.native);
    query(aliased->mapped, "key", {"value", "value"});
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_cola_pair'") == 1);
    auto side_native = singleton<Storage>("side", "secondary");
    auto side_alias = pair<Storage>(side_native, {}, side_native);
    auto side = seal.ensure_pair(side_alias);
    query(side->mapped, "side", {"secondary", "secondary"});
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal'") == 1);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_cola_pair'") == 2);
    // Hidden-only publication remains a native reserve/seal, with no index.
    auto hidden = singleton<Storage>("hidden", "hidden");
    auto hidden_binding = seal.ensure_native(hidden);
    assert(hidden_binding->mapped->size() == 1);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal'") == 2);

    auto older_a = pair<Storage>(singleton<Storage>("older", "A"));
    auto older_b = pair<Storage>(singleton<Storage>("older", "B"));
    seal.ensure_pair(older_a); seal.ensure_pair(older_b);
    auto shared = singleton<Storage>("newer", "shared");
    std::array branches{pair<Storage>(shared, older_a), pair<Storage>(shared, older_b)};
    using binding = typename sealer::pair_binding_type;
    std::array<std::shared_ptr<binding const>, 2> results;
    std::array<std::exception_ptr, 2> errors;
    std::barrier ready(2);
    auto before = scalar(dir.root, "SELECT count(*) FROM operations");
    auto fusions = scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'");
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i != 2; ++i) threads.emplace_back([&, i] {
      ready.arrive_and_wait();
      try {
        auto concurrent = sqlite_catalog<P>::open(dir.root, catalog_options{5000});
        auto local_ids = identity;
        sealer local(concurrent, local_ids);
        results[i] = local.ensure_pair(branches[i]);
      } catch (...) { errors[i] = std::current_exception(); }
    });
    threads.clear();
    for (auto const & error : errors) if (error) std::rethrow_exception(error);
    assert(results[0]->identity.native == results[1]->identity.native);
    assert(results[0]->identity.index != results[1]->identity.index);
    assert(scalar(dir.root, "SELECT count(*) FROM operations") == before + 4);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == fusions + 1);
    for (unsigned i = 0; i != 2; ++i) {
      results[i]->mapped->scan();
      query(results[i]->mapped, "newer", {"shared"});
      query(results[i]->mapped, "older", {i ? "B" : "A"});
    }
  }

  template <class Storage> void uncertain_producer() {
    for (bool after : {false, true}) {
      temporary dir;
      auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
      ids identity;
      using regular = runtime_store_detail::graph_sealer<P, ids, sqlite_catalog_ops, family<Storage>>;
      regular original(catalog, identity);
      auto old = original.ensure_pair(pair<Storage>(singleton<Storage>("old", "retained")));
      catalog.save("save", "old", old->identity);
      auto native = singleton<Storage>("new", "candidate");
      auto candidate = pair<Storage>(native);
      auto state = std::make_shared<fault_state>(fault_state{true, after});
      auto faulty = sqlite_catalog<P, fault_ops>::open(dir.root, {}, fault_ops{state});
      runtime_store_detail::graph_sealer<P, ids, fault_ops, family<Storage>> producer(faulty, identity);
      rejects([&] { producer.ensure_pair(candidate); });
      assert(faulty.poisoned());
      // Inspect through the healthy catalog: the failed handle would reject
      // identity lookup before reaching the shared owner slots.
      rejects([&] { (void)original.native_id(native); });
      rejects([&] { (void)original.pair_id(candidate); });
      assert(catalog.find_save("old") == old->identity);
      query(old->mapped, "old", {"retained"});
      auto reopened = sqlite_catalog<P>::open(dir.root);
      regular healthy(reopened, identity);
      auto completed = healthy.ensure_pair(candidate);
      query(completed->mapped, "new", {"candidate"});
      assert(reopened.find_save("old") == old->identity);
    }
  }

  void reservation_mismatch() {
    using storage = sort_runtime_storage<P>;
    using mapped = storage::mapped_pair_type;
    for (unsigned mode = 0; mode != 3; ++mode) {
      temporary dir;
      auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
      auto native = singleton<storage>("key", "value");
      auto source = pair<storage>(native);
      blob_identity identity{id(2), id(3)};
      object_attempt_id attempt(id(4).hex());
      std::vector<catalog_object_reservation> outputs{{id(3), file_kind::fractional_index}};
      if (mode != 1) outputs.push_back({id(2), mode == 0 ? file_kind::fractional_index : file_kind::native_blob});
      catalog.reserve("reserve", attempt, "producer", {}, outputs);
      auto n = storage::encode_native(*native->owned()).seal(dir.root, id(2), attempt);
      auto i = encode_cola_sections(*source->built(), id(2)).seal(dir.root, id(3), attempt);
      if (mode == 2) catalog.record_sealed("native-only", n);
      rejects([&] { catalog.seal_native_pair<mapped>("invalid", identity, n, i); });
      assert(!catalog.poisoned() && !catalog.lookup_operation("invalid"));
      assert(scalar(dir.root, "SELECT count(*) FROM pairs") == 0);
      assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == (mode == 2 ? 1 : 0));
      if (mode == 2) catalog.seal_pair<mapped>("fallback", identity, i)->scan();
    }
  }

  template <class Storage> void interrupted_installation() {
    temporary dir;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    ids identity;
    using sealer = runtime_store_detail::graph_sealer<P, ids, sqlite_catalog_ops, family<Storage>>;
    sealer seal(catalog, identity);
    auto old = seal.ensure_pair(pair<Storage>(singleton<Storage>("old", "retained")));
    catalog.save("save", "old", old->identity);
    auto native = singleton<Storage>("new", "candidate");
    auto source = pair<Storage>(native);
    auto native_id = id(identity.next->load());
    auto index_id = id(identity.next->load() + 1);
    auto collision = dir.root / object_path(index_id, file_kind::fractional_index);
    std::filesystem::create_directories(collision.parent_path());
    { std::ofstream output(collision); output << "occupied"; output.close(); assert(output); }
    // Native sealing succeeds; the index's real no-replace installation fails.
    rejects([&] { seal.ensure_pair(source); });
    assert(!catalog.poisoned());
    auto completed_native = Storage::mapped_native_type::open(dir.root / object_path(native_id, file_kind::native_blob));
    completed_native.scan();
    assert(completed_native.size() == 1);
    rejects([&] { (void)seal.native_id(native); });
    rejects([&] { (void)seal.pair_id(source); });
    assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE bytes IS NULL") == 2);
    assert(scalar(dir.root, "SELECT count(*) FROM pairs") == 1);
    assert(catalog.find_save("old") == old->identity);
    query(old->mapped, "old", {"retained"});
    auto retry = seal.ensure_pair(source);
    assert(retry->identity.native != native_id && retry->identity.index != index_id);
    query(retry->mapped, "new", {"candidate"});
    assert(catalog.find_save("old") == old->identity);
    std::ifstream input(collision);
    std::string marker;
    input >> marker;
    assert(marker == "occupied");
  }

  // A custom mapper can allocate after joint acknowledgment. Its exception
  // must leave the already acknowledged native reusable and the pair unbound.
  struct failing_mapped {
    using actual = mapped_sort_cola<P>;
    using policy_type = P;
    using native_type = typename actual::native_type;
    using index_type = typename actual::index_type;
    using native_pointer = std::shared_ptr<native_type const>;
    using pair_type = std::shared_ptr<failing_mapped const>;
    inline static bool fail = false;
    std::shared_ptr<actual const> value;
    static pair_type bind(blob_identity identity, native_pointer native,
        std::shared_ptr<index_type const> index, pair_type main = {}, native_pointer secondary = {},
        std::optional<object_id> secondary_id = {}) {
      if (fail) { fail = false; throw std::bad_alloc(); }
      auto value = actual::bind(std::move(identity), std::move(native), std::move(index),
        main ? main->value : nullptr, std::move(secondary), std::move(secondary_id));
      return std::make_shared<failing_mapped const>(failing_mapped{std::move(value)});
    }
    auto view() const { return value->view(); }
  };
  struct failing_storage : sort_runtime_storage<P> { using mapped_pair_type = failing_mapped; };
  void acknowledged_mapping_failure() {
    temporary dir;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    ids identity;
    runtime_store_detail::graph_sealer<P, ids, sqlite_catalog_ops, family<failing_storage>> seal(catalog, identity);
    auto native = singleton<failing_storage>("key", "value");
    auto source = pair<failing_storage>(native);
    failing_mapped::fail = true;
    rejects([&] { seal.ensure_pair(source); });
    assert(!catalog.poisoned());
    auto retained = seal.native_id(native);
    rejects([&] { (void)seal.pair_id(source); });
    assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE bytes IS NOT NULL") == 2);
    assert(scalar(dir.root, "SELECT count(*) FROM pairs") == 1);
    auto retry = seal.ensure_pair(source);
    assert(retry->identity.native == retained);
    query(retry->mapped->value, "key", {"value"});
    assert(scalar(dir.root, "SELECT count(*) FROM objects WHERE kind=0") == 1);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == 1);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_cola_pair'") == 1);
  }
}

int main() {
  try {
    direct_acknowledgment<profile_runtime_storage<P>>();
    direct_acknowledgment<sort_runtime_storage<P>>();
    aliases_and_races<profile_runtime_storage<P>>();
    aliases_and_races<sort_runtime_storage<P>>();
    uncertain_producer<profile_runtime_storage<P>>();
    uncertain_producer<sort_runtime_storage<P>>();
    reservation_mismatch();
    interrupted_installation<profile_runtime_storage<P>>();
    interrupted_installation<sort_runtime_storage<P>>();
    acknowledged_mapping_failure();
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
