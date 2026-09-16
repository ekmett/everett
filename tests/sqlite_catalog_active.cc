/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises the ordinary streamed session, custom arrows and byte-registry fallback.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>
#include <everett/typed_scan.h>

#include <cassert>
#include <iostream>
#include <map>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  static_assert(std::same_as<multiverse<>::active_engine, active_engine<>>);
  static_assert(std::same_as<connection<>::core_type, active_engine<>>);
  static_assert(std::same_as<persistent_engine<>::core_type, active_engine<>>);
  static_assert(std::same_as<active_engine<>::runtime_family, streaming_sort_runtime_family<>>);
  static_assert(active_engine<>::charged_service);
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-active-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action) {
    bool caught = false;
    try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  template <class World> void check(World const & saved, std::map<std::string, std::string> const & expected) {
    assert(saved.live_count() == expected.size());
    std::uint64_t signature = 0;
    for (auto const & [key, value] : expected) {
      assert(saved.get(key) == value);
      signature += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, value);
    }
    assert(saved.signature() == signature);
    std::map<std::string, std::string> scanned;
    typed_scan<strings, World> cursor(saved);
    while (auto row = cursor.next()) assert(scanned.emplace(row->key, *row->value).second);
    assert(scanned == expected);
  }
  void ordinary() {
    temporary dir;
    auto storage = multiverse<>::create(dir.root / "multiverse");
    std::map<std::string, std::string> expected;
    {
      auto db = storage.connect("earth-616");
      std::vector<connection<>::ticket> tickets;
      for (unsigned i = 0; i != 20; ++i) {
        auto key = "prefix/" + std::to_string(i), value = "value/" + std::to_string(i);
        expected.emplace(key, value); tickets.push_back(db.put_async(key, value));
      }
      for (unsigned i = 0; i != tickets.size(); ++i)
        assert(tickets[i].get()->world.get("prefix/" + std::to_string(i)) == "value/" + std::to_string(i));
      auto old = db.snapshot(); auto original = expected;
      check(old, expected); db.save("original", old);
      for (unsigned i = 0; i != 4; ++i) {
        auto key = "prefix/" + std::to_string(i); db.erase(key); expected.erase(key);
      }
      std::string key("key\0tail", 8), value(128 * 1024, 'x');
      value[7] = '\0'; db.put(key, value); expected[key] = value;
      auto base = db.snapshot();
      auto left = base.put("partition/a", "A"), right = base.put("partition/b", "B");
      db.apply(std::move(right)); db.apply(std::move(left));
      expected["partition/a"] = "A"; expected["partition/b"] = "B";
      check(db.snapshot(), expected); check(old, original);
      rejects([&] { db.erase("absent"); }); assert(!db.failure());
      auto fork = db.fork("earth-617", old); fork.put("prefix/0", "branch");
      assert(fork.get("prefix/0") == "branch" && old.get("prefix/0") == "value/0" && !db.get("prefix/0"));
      auto restored = db.load("original"); assert(restored); check(*restored, original);
    }
    auto live = persistent_engine<>::connect(storage.root(), "earth-616", {.create_if_missing = false});
    check(live.snapshot(), expected);
    while (live.pending()) (void)live.advance(1 << 20);
    auto mapped = live.snapshot(); check(mapped, expected);
    mapped_cola_scan<mapped_sort_cola<string_policy>> scan;
    scan(*mapped.runtime().query_root().head()->mapped());
    for (auto const & object : runtime_storage_codec<streaming_sort_runtime_family<>>::objects(mapped.runtime().frontier())) {
      assert(object->native->mapped() && !object->native->owned());
      if (object->pair) scan(*object->pair->mapped());
    }
    // Both wrapped static factories return the public engine type and preserve
    // the concrete storage handle across a settled rebase.
    using core = active_engine<>;
    auto attached = core::from_snapshot(mapped, core::runtime_family::open_storage(storage.root()));
    static_assert(std::same_as<decltype(attached), core>);
    auto context = attached.storage().context();
    attached.rebase(mapped); assert(attached.storage().context() == context);
  }

  struct append_sort {
    using key_codec = fc_string_key<>;
    using value_codec = string_value<>;
    using encoding = bit_encoding<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type older, state_type const & newer) { return older + newer; }
    static state_type compose(std::string const &, state_type older, state_type const & newer) { return older + newer; }
    static bool present(std::string const &, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::string const & key) { return u64_table_hash{}.key(key); }
    static std::uint64_t hash_value(std::string const &, state_type const & value) { return u64_table_hash{}.key(value); }
  };
  void registries() {
    using byte_policy = storage_policy<strings>;
    static_assert(std::same_as<active_engine<byte_policy>::runtime_family, redundant_runtime_family<byte_policy>>);
    static_assert(active_engine<byte_policy>::charged_service);
    temporary bytes;
    {
      auto db = multiverse<byte_policy>(bytes.root).connect("bytes");
      for (unsigned i = 0; i != 8; ++i) db.put("key", std::to_string(i));
      assert(db.get("key") == "7");
    }
    auto reopened = multiverse<byte_policy>(bytes.root).connect("bytes", {.create_if_missing = false});
    assert(reopened.get("key") == "7");
    using policy = storage_policy<bin<tip<append_sort>, tip<strings>>, 3>;
    connection_options options{.schema_id = "tests.active.mixed/v1"};
    temporary mixed;
    std::string expected;
    {
      auto db = multiverse<policy>(mixed.root).connect("mixed", options);
      for (unsigned i = 0; i != 14; ++i) {
        auto value = std::to_string(i) + "/"; expected += value;
        db.change<append_sort>("key", value); db.put<strings>("key", value);
      }
      assert(db.get<append_sort>("key") == expected && db.get<strings>("key") == "13/");
    }
    auto db = multiverse<policy>(mixed.root).connect("mixed", options);
    assert(db.get<append_sort>("key") == expected && db.get<strings>("key") == "13/");
  }
}
int main() {
  try { ordinary(); registries(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
