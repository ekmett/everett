/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks independent reservation batches, owner claims and partial acknowledgments.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/runtime_graph_sealer.h>
#include <diet/sort_runtime.h>
#include <diet/sort_runtime_store.h>
#include <diet/typed_scan.h>

#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <latch>
#include <thread>

namespace {
  using namespace diet;
  using P = storage_policy<string_registry, 3>;
  using input_core = typed_engine<P, wrapping_fingerprint_algebra, 256, sort_runtime_family<P>>;
  object_id id(std::uint64_t value) {
    char text[33]; std::snprintf(text, sizeof text, "%032llx", static_cast<unsigned long long>(value));
    return object_id(text);
  }
  struct ids {
    std::shared_ptr<std::atomic<std::uint64_t>> next = std::make_shared<std::atomic<std::uint64_t>>(1000);
    object_id operator()() { return id(next->fetch_add(1)); }
  };
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-ready-reservation-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && f) {
    bool caught = false; try { f(); } catch (std::exception const &) { caught = true; } assert(caught);
  }
  std::int64_t scalar(std::filesystem::path const & root, char const * sql) {
    sqlite3 * db = nullptr;
    assert(sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt * statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    auto value = sqlite3_column_int64(statement, 0);
    assert(sqlite3_finalize(statement) == SQLITE_OK); assert(sqlite3_close(db) == SQLITE_OK); return value;
  }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t count = 0;
    for (auto const & file : std::filesystem::recursive_directory_iterator(root))
      if (file.path().extension() == ".kv" || file.path().extension() == ".index") ++count;
    return count;
  }
  template <class Storage> struct family {
    using storage_type = Storage;
    using native_type = typename Storage::native_type;
    using node_type = redundant_node<P, Storage>;
  };
  template <class Storage> using node = typename family<Storage>::node_type;
  template <class Storage> auto native(std::string const & key, std::string const & value = "value") {
    auto record = input_core::put(key, value); return Storage::singleton(record.records()[0]);
  }
  template <class Storage> auto pair(typename node<Storage>::native_pointer own,
      typename node<Storage>::pair_type main = {}, typename node<Storage>::native_pointer secondary = {}) {
    cola_index_builder<P, typename Storage::native_type, node<Storage>> writer(own, main, secondary);
    while (!writer.done()) writer.step(10);
    return node<Storage>::from_built(writer.finish());
  }
  template <class Storage, class Ops = sqlite_catalog_ops>
  using sealer = runtime_store_detail::graph_sealer<P, ids, Ops, family<Storage>>;
  template <class Mapped> void query(std::shared_ptr<Mapped const> const & source, std::string const & key) {
    auto expected = input_core::put(key, "value");
    auto root = cola_query_root<P, Mapped>::adopt_prepared(source);
    auto cursor = root.cursor(expected.records()[0].key.view());
    while (!cursor.done() && !cursor.has_match()) cursor.step();
    assert(cursor.has_match() && cursor.take_match().value == expected.records()[0].value);
  }

  void claims() {
    catalog_bindings<unsigned> binding;
    auto first = binding.prepare(id(1), "/one");
    auto second = binding.prepare(id(1), "/one");
    auto other = binding.prepare(id(1), "/two");
    std::latch locked(1), release(1);
    auto worker = std::async(std::launch::async, [&] {
      assert(first.try_lock()); assert(!first.value()); locked.count_down(); release.wait();
      first.install(std::make_shared<unsigned const>(19));
    });
    locked.wait(); assert(!second.try_lock());
    assert(other.try_lock()); other.install(std::make_shared<unsigned const>(23));
    release.count_down(); worker.get();
    assert(second.try_lock() && *second.value() == 19); second.release();
    assert(*binding.find(id(1), "/two") == 23);
  }

  template <class Storage> void independent() {
    temporary dir; ids next;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    sealer<Storage> graph(catalog, next);
    auto left = pair<Storage>(native<Storage>("left"));
    auto right = pair<Storage>(native<Storage>("right"));
    std::array roots{left, right};
    assert(graph.prepare_ready(roots, {}) == 2);
    assert(files(dir.root) == 4);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='reserve'") == 1);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == 2);
    assert(scalar(dir.root, "SELECT count(DISTINCT attempt) FROM objects") == 1);
    auto a = graph.ensure_pair(left), b = graph.ensure_pair(right);
    assert(a->identity.native != b->identity.native); query(a->mapped, "left"); query(b->mapped, "right");
    assert(graph.prepare_ready(roots, {}) == 0);
    assert(scalar(dir.root, "SELECT count(*) FROM operations") == 3);

    // Both captured main/secondary dependencies are already acknowledged. Own
    // natives are already bound too, so only the two new indexes are reserved.
    auto first = pair<Storage>(left->native_owner(), left, right->native_owner());
    auto second = pair<Storage>(right->native_owner(), right, left->native_owner());
    std::array indices{first, second};
    assert(graph.prepare_ready(indices, {}) == 2);
    assert(scalar(dir.root, "SELECT count(*) FROM objects") == 6);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='reserve'") == 2);
    auto x = graph.ensure_pair(first), y = graph.ensure_pair(second);
    assert(x->identity.native == a->identity.native && y->identity.native == b->identity.native);
    assert(x->mapped->main_target() == a->mapped && x->mapped->secondary_target() == b->mapped->native_object());

    // A genuinely hidden native has no owning pair: it can share the next
    // reservation with a complete fused pair without forcing extra files.
    auto hidden = native<Storage>("hidden");
    auto third = pair<Storage>(native<Storage>("third"));
    std::array more{third}; std::array natives{hidden, hidden, third->native_owner()};
    assert(graph.prepare_ready(more, natives) == 2);
    assert(scalar(dir.root, "SELECT count(*) FROM objects") == 9);
    assert(graph.ensure_native(hidden)->receipt.object != graph.pair_id(third).native);
  }

  template <class Storage> void aliases_and_unready() {
    temporary dir; ids next;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    sealer<Storage> graph(catalog, next);
    auto shared = native<Storage>("shared");
    auto left = pair<Storage>(shared), right = pair<Storage>(shared);
    std::array aliases{left, right, left}; std::array natives{shared};
    assert(graph.prepare_ready(aliases, natives) == 0); // One complete producer.
    assert(files(dir.root) == 0 && scalar(dir.root, "SELECT count(*) FROM operations") == 0);
    auto a = graph.ensure_pair(left), b = graph.ensure_pair(right);
    assert(a->identity.native == b->identity.native && files(dir.root) == 3);

    auto leaf = pair<Storage>(native<Storage>("leaf"));
    auto own = native<Storage>("parent");
    auto parent = pair<Storage>(own, leaf);
    std::array roots{parent}; std::array native_roots{own, leaf->native_owner()};
    auto before = scalar(dir.root, "SELECT count(*) FROM operations");
    assert(graph.prepare_ready(roots, native_roots) == 0);
    assert(scalar(dir.root, "SELECT count(*) FROM operations") == before);
    (void)graph.ensure_pair(parent);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == 3);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal'") == 0);
  }

  void bounded_batch() {
    using Storage = sort_runtime_storage<P>;
    temporary dir; ids next;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    sealer<Storage> graph(catalog, next);
    std::vector<typename node<Storage>::pair_type> roots;
    for (unsigned i = 0; i != 18; ++i) roots.push_back(pair<Storage>(native<Storage>("key-" + std::to_string(i))));
    assert(graph.prepare_ready(roots, {}) == 16);
    assert(files(dir.root) == 32 && scalar(dir.root, "SELECT count(*) FROM attempts") == 1);
    for (auto const & root : roots) (void)graph.ensure_pair(root);
    assert(files(dir.root) == 36 && scalar(dir.root, "SELECT count(*) FROM attempts") == 3);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == 18);
  }

  void secondary_alias() {
    using Storage = sort_runtime_storage<P>;
    temporary dir; ids next;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    sealer<Storage> graph(catalog, next);
    auto shared = native<Storage>("shared");
    auto alias = pair<Storage>(shared, {}, shared);
    auto left = pair<Storage>(native<Storage>("left"));
    auto right = pair<Storage>(native<Storage>("right"));
    std::array roots{alias, left, right}; std::array natives{shared};
    assert(graph.prepare_ready(roots, natives) == 2);
    assert(files(dir.root) == 4);
    rejects([&] { (void)graph.native_id(shared); });
    auto result = graph.ensure_pair(alias);
    assert(result->mapped->native_object() == result->mapped->secondary_target());
    assert(files(dir.root) == 6);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == 2);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal'") == 1);
  }

  void real_frontier() {
    using Q = string_policy;
    using F = sort_runtime_family<Q>;
    using Core = typed_engine<Q, wrapping_fingerprint_algebra, 256, F>;
    using Codec = runtime_storage_codec<F>;
    Core active; auto input = Core::batch();
    for (unsigned i = 64; i; --i) input.put("key-" + std::to_string(i), "value-" + std::to_string(i));
    auto source = active.contribute(std::move(input).finish());
    assert(!active.pending() && source.runtime().admissions() == 64);
    struct counts { std::size_t selected, files; std::int64_t reservations, fused; };
    auto prepare = [&](std::filesystem::path const & root, bool batched) {
      ids next; auto catalog = sqlite_catalog<Q>::create_taps(root, id(1));
      runtime_store_detail::graph_sealer<Q, ids, sqlite_catalog_ops, F> graph(catalog, next);
      std::vector<typename F::node_type::pair_type> pairs;
      std::vector<typename F::node_type::native_pointer> natives;
      Codec::collect(source.runtime(), [&](auto p) { if (p) pairs.push_back(std::move(p)); },
        [&](auto n) { if (n) natives.push_back(std::move(n)); });
      auto selected = batched ? graph.prepare_ready(pairs, natives) : 0;
      for (auto const & pair : pairs) (void)graph.ensure_pair(pair);
      for (auto const & native : natives) (void)graph.ensure_native(native);
      auto head = graph.pair_id(source.runtime().query_root().head());
      catalog_auxiliary_roots auxiliary;
      for (auto const & pair : pairs) auxiliary.pairs.push_back(graph.pair_id(pair));
      for (auto const & native : natives) auxiliary.natives.push_back(graph.native_id(native));
      auto checkpoint = Codec::encode(source.runtime(), source.metadata().encode(),
        [&](auto const & pair) { return graph.pair_id(pair); },
        [&](auto const & native) { return graph.native_id(native); });
      auto published = catalog.create_tap("publication", "main", head, checkpoint, auxiliary);
      auto reopened = runtime_store<Q, ids, sqlite_catalog_ops, F>::open(root);
      auto saved = reopened.find("main"); assert(saved && saved->head == published);
      assert(Codec::object_count(saved->snapshot) == Codec::object_count(source.runtime()));
      assert(saved->snapshot.admissions() == source.runtime().admissions());
      assert(saved->semantic == source.metadata().encode());
      auto restored = Core::cola_type::restore(saved->snapshot, source.metadata(), source.metadata().schema_id);
      for (unsigned i = 1; i <= 64; ++i)
        assert(restored.get("key-" + std::to_string(i)) == "value-" + std::to_string(i));
      assert(!restored.get("missing") && restored.signature() == source.signature() && restored.live_count() == 64);
      auto scan = diet::scan(restored); unsigned found = 0;
      while (auto row = scan.next()) { assert(row->value == "value-" + row->key.substr(4)); ++found; }
      assert(found == 64);
      return counts{selected, files(root), scalar(root, "SELECT count(*) FROM operations WHERE kind='reserve'"),
        scalar(root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'")};
    };
    temporary serial_dir, batched_dir;
    auto serial = prepare(serial_dir.root, false), batched = prepare(batched_dir.root, true);
    assert(batched.selected >= 2 && serial.files == batched.files && batched.fused >= serial.fused);
    auto extra_fusion = batched.fused - serial.fused;
    assert(serial.reservations - batched.reservations == std::int64_t(batched.selected - 1) + extra_fusion);
    std::cout << "64-row frontier: " << batched.selected << " ready units, reservations " << serial.reservations
      << " -> " << batched.reservations << ", extra fusions " << extra_fusion << ", files " << batched.files << '\n';
  }

  struct fault_state {
    std::string kind;
    unsigned remaining = 1;
    bool armed = false, after = false;
    std::string operation;
    std::vector<std::byte> request;
  };
  struct fault_ops {
    std::shared_ptr<fault_state> state;
    int commit(sqlite3 * db) noexcept {
      try {
        sqlite3_stmt * raw = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id,kind,request FROM operations ORDER BY rowid DESC LIMIT 1", -1, &raw, nullptr) != SQLITE_OK)
          return SQLITE_ERROR;
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(raw, sqlite3_finalize);
        bool hit = false;
        if (sqlite3_step(query.get()) == SQLITE_ROW && state->armed) {
          auto kind = reinterpret_cast<char const *>(sqlite3_column_text(query.get(), 1));
          if (kind && state->kind == kind && --state->remaining == 0) {
            auto op = static_cast<char const *>(sqlite3_column_blob(query.get(), 0));
            state->operation.assign(op, sqlite3_column_bytes(query.get(), 0));
            auto request = static_cast<std::byte const *>(sqlite3_column_blob(query.get(), 2));
            state->request.assign(request, request + sqlite3_column_bytes(query.get(), 2));
            state->armed = false; hit = true;
          }
        }
        query.reset();
        if (hit && !state->after) return SQLITE_IOERR_FSYNC;
        auto code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        return hit && code == SQLITE_OK ? SQLITE_IOERR_FSYNC : code;
      } catch (...) { return SQLITE_NOMEM; }
    }
  };
  template <class Storage> void failures() {
    for (bool reservation : {false, true}) for (bool after : {false, true}) {
      temporary dir; ids next;
      auto state = std::make_shared<fault_state>();
      auto catalog = sqlite_catalog<P, fault_ops>::create_taps(dir.root, id(1), {}, fault_ops{state});
      std::string last;
      sealer<Storage, fault_ops> graph(catalog, next, &last);
      auto old = pair<Storage>(native<Storage>("old"));
      auto old_id = graph.ensure_pair(old)->identity;
      auto old_head = catalog.create_tap("old-head", "main", old_id, {});
      auto left = pair<Storage>(native<Storage>("left")), right = pair<Storage>(native<Storage>("right"));
      std::array roots{left, right};
      state->kind = reservation ? "reserve" : "seal_native_cola_pair";
      state->remaining = reservation ? 1 : 2;
      state->after = after; state->armed = true;
      rejects([&] { (void)graph.prepare_ready(roots, {}); });
      assert(!state->armed && catalog.poisoned() && last == state->operation);
      auto healthy = sqlite_catalog<P>::open(dir.root);
      sealer<Storage> retry(healthy, next);
      if (reservation) {
        assert(files(dir.root) == 2);
        rejects([&] { (void)retry.pair_id(left); });
      } else {
        assert(files(dir.root) == 6);
        assert(retry.pair_id(left).native != old_id.native);
      }
      rejects([&] { (void)retry.pair_id(right); });
      assert(bool(healthy.lookup_operation(last)) == after);
      assert(healthy.find_tap("main") == old_head);
      if (reservation) {
        // Reconcile the exact uncertain multi-object reservation; successful
        // replay must neither add pins nor duplicate the four output rows.
        catalog_detail::outcome_reader r{state->request};
        object_attempt_id attempt(r.field()); auto owner = r.field();
        std::vector<blob_identity> inputs;
        auto count = r.number(); while (count--) { auto n = r.field(), i = r.field(); inputs.push_back({object_id(n), object_id(i)}); }
        std::vector<catalog_object_reservation> outputs;
        count = r.number(); while (count--) { auto object = r.field(); auto kind = r.number(); outputs.push_back({object_id(object), kind ? file_kind::fractional_index : file_kind::native_blob}); }
        r.end(); assert(outputs.size() == 4);
        healthy.reserve(last, attempt, owner, inputs, outputs);
        auto pins = scalar(dir.root, "SELECT count(*) FROM owner_objects");
        healthy.reserve(last, attempt, owner, inputs, outputs);
        assert(scalar(dir.root, "SELECT count(*) FROM owner_objects") == pins);
      }
      std::optional<blob_identity> acknowledged;
      if (!reservation) acknowledged = retry.pair_id(left);
      (void)retry.prepare_ready(roots, {});
      auto a = retry.ensure_pair(left), b = retry.ensure_pair(right);
      if (acknowledged) assert(a->identity == *acknowledged);
      assert(healthy.find_tap("main") == old_head);
      query(a->mapped, "left"); query(b->mapped, "right");
    }
  }

  template <class Storage> void concurrent() {
    temporary dir; ids next;
    { auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1)); }
    std::array roots{pair<Storage>(native<Storage>("a")), pair<Storage>(native<Storage>("b")),
      pair<Storage>(native<Storage>("c")), pair<Storage>(native<Storage>("d"))};
    std::latch start(1);
    auto run = [&](bool reverse) {
      auto catalog = sqlite_catalog<P>::open(dir.root, {.busy_timeout_ms = 5000});
      sealer<Storage> graph(catalog, next);
      auto order = roots; if (reverse) std::reverse(order.begin(), order.end());
      start.wait(); (void)graph.prepare_ready(order, {});
      std::vector<blob_identity> result;
      for (auto const & root : roots) result.push_back(graph.ensure_pair(root)->identity);
      return result;
    };
    auto a = std::async(std::launch::async, run, false), b = std::async(std::launch::async, run, true);
    start.count_down();
    assert(a.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    assert(b.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    assert(a.get() == b.get());
    assert(files(dir.root) == 8 && scalar(dir.root, "SELECT count(*) FROM objects") == 8);
    assert(scalar(dir.root, "SELECT count(*) FROM operations WHERE kind='seal_native_cola_pair'") == 4);
  }
}

int main() {
  claims();
  independent<profile_runtime_storage<P>>(); independent<sort_runtime_storage<P>>();
  aliases_and_unready<sort_runtime_storage<P>>();
  bounded_batch();
  secondary_alias();
  real_frontier();
  failures<sort_runtime_storage<P>>(); concurrent<sort_runtime_storage<P>>();
  std::cout << "Ready reservation batches passed\n";
}
