/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks ordinary strong deletes, exact queue limits and code-stable registry extension.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>

#include <cassert>
#include <iostream>
#include <map>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using core = active_engine<>;
  static_assert(std::same_as<core::metadata_type, replacement_metadata<>>);
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-rebuild-default-XXXXXX").string();
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
  template <class World> void check(World const & value, std::map<std::string, std::string> const & expected) {
    std::uint64_t signature = 0;
    for (auto const & [key, data] : expected) {
      assert(value.template get<strings>(key) == data);
      signature += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, data);
    }
    assert(value.live_count() == expected.size() && value.signature() == signature);
    typed_scan<strings, World> rows(value);
    auto item = expected.begin();
    while (auto row = rows.next()) {
      assert(item != expected.end() && row->key == item->first && row->value == item->second); ++item;
    }
    assert(item == expected.end());
  }
  std::string key(unsigned i) { return "key/" + std::to_string(i); }
  auto batch(unsigned count) {
    auto input = core::batch();
    for (unsigned i = 0; i != count; ++i) input.put(key(i), "value");
    return std::move(input).finish();
  }
  void limits_and_cleanup() {
    temporary dir;
    auto storage = multiverse<>::create(dir.root);
    assert(core::reservation_work(1) == 40'361'922);
    assert(core::reservation(batch(64)).work == core::reservation_work(64));
    auto live = storage.connect("ordinary");
    auto limits = live.limits();
    assert(limits.work == core::reservation_work(1024) && limits.bytes == 64 * 1024 * 1024 &&
      limits.contributions == 64 && limits.maintenance_budget == 4096);
    auto full = live.apply(batch(64));
    std::map<std::string, std::string> expected;
    for (unsigned i = 0; i != 64; ++i) expected[key(i)] = "value";
    check(full, expected);
    assert(full.metadata().clean_base == 64 && !full.metadata().mutations && full.runtime().admissions() == 64);
    live.save("full", full);
    auto removals = core::batch();
    for (unsigned i = 0; i != 64; ++i) removals.erase(key(i));
    auto empty = live.apply(std::move(removals).finish());
    check(empty, {}); check(full, expected);
    assert(!empty.runtime().admissions() && !empty.metadata().clean_base && !empty.metadata().mutations);
    auto loaded = live.load("full"); assert(loaded); check(*loaded, expected);

    session_limits explicit_limits{core::reservation_work(3), 4096, 2, 17};
    connection_options options{.limits = explicit_limits};
    auto limited = storage.connect("limited", options);
    auto actual = limited.limits();
    assert(actual.work == explicit_limits.work && actual.bytes == explicit_limits.bytes &&
      actual.contributions == explicit_limits.contributions && actual.maintenance_budget == 17);
    auto too_large = batch(4);
    rejects([&] { (void)limited.try_submit(std::move(too_large)); });
    assert(too_large.records().size() == 4 && !limited.failure() && !limited.snapshot().live_count());
    limited.apply(batch(3));
    auto fork = limited.fork("limited-fork");
    assert(fork.limits().work == explicit_limits.work && fork.limits().maintenance_budget == 17);
    auto zero = storage.connect("zero", {.limits = session_limits{0, 0, 1, 1}});
    rejects([&] { zero.put("key", "value"); });
    assert(!zero.failure() && !zero.limits().work && !zero.limits().bytes);
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
    static std::uint64_t hash_key(std::string const & value) { return u64_table_hash{}.key(value); }
    static std::uint64_t hash_value(std::string const &, state_type const & value) { return u64_table_hash{}.key(value); }
  };
  using extended_policy = storage_policy<bin<tip<strings>, tip<append_sort>>>;
  using extended_core = active_engine<extended_policy>;
  using extended_store = runtime_store<extended_policy, random_object_ids, sqlite_catalog_ops, extended_core::runtime_family>;
  static_assert(std::same_as<extended_core::metadata_type, typed_world_metadata<>>);
  void registry_extension() {
    temporary dir;
    auto storage = multiverse<>::create(dir.root);
    std::map<std::string, std::string> expected;
    for (unsigned i = 0; i != 64; ++i) expected[key(i)] = "value";
    auto old = [&] {
      auto engine = persistent_engine<>::connect(storage.root(), "evolving");
      engine.contribute(batch(64));
      for (unsigned i = 0; i != 16; ++i) engine.contribute(core::put(key(0), "changed"));
      expected[key(0)] = "changed";
      auto saved = engine.snapshot();
      assert(saved.metadata().rebuilding && saved.metadata().clean_base == 64 && saved.metadata().mutations == 16);
      return saved;
    }();
    auto schema = old.metadata().schema_id;
    connection_options options{.schema_id = schema, .create_if_missing = false};
    auto store = extended_store::open(storage.root());
    store.save("before-extension", old.head());
    auto loaded = store.find("evolving"); assert(loaded);
    auto bad = old.metadata(); ++bad.mutations;
    (void)store.create_session("bad-accounting", loaded->snapshot, bad.encode());
    rejects([&] { (void)persistent_engine<extended_core>::connect(storage.root(), "bad-accounting", options); });
    auto projected = extended_core::restore_checkpoint(loaded->snapshot, loaded->semantic, schema);
    check(projected, expected);
    assert(projected.runtime().admissions() == old.runtime().admissions());
    assert(projected.metadata().schema_id == schema && projected.signature() == old.signature());
    auto truncated = loaded->semantic; truncated.pop_back();
    rejects([&] { (void)extended_core::restore_checkpoint(loaded->snapshot, truncated, schema); });
    auto wrong_schema = schema; wrong_schema.back() ^= 1;
    rejects([&] { (void)extended_core::restore_checkpoint(loaded->snapshot, loaded->semantic, wrong_schema); });
    auto typed = projected.metadata();
    // A typed fingerprint can spell the other format's magic; its exact
    // expected-schema length still identifies the ordinary typed envelope.
    constexpr std::array<unsigned char, 8> magic{'E', 'V', 'R', 'T','.','R','B',0};
    typed.signature = 0;
    for (unsigned i = 0; i != 8; ++i) typed.signature |= std::uint64_t(magic[i]) << (i << 3);
    auto collision = extended_core::restore_checkpoint(loaded->snapshot, typed.encode(), schema);
    assert(collision.signature() == typed.signature);
    {
      connection<extended_core> live(storage.root(), "evolving", options);
      check(live.snapshot(), expected);
      assert(live.limits().work == extended_core::reservation_work(1024));
      live.change<append_sort>("log", "A"); live.change<append_sort>("log", "B");
      assert(live.get<append_sort>("log") == "AB");
      auto before = live.load("before-extension"); assert(before); check(*before, expected);
      auto fork = live.fork("extended-fork", *before);
      fork.change<append_sort>("log", "fork");
      assert(fork.get<append_sort>("log") == "fork" && before->get<append_sort>("log").empty());
      assert(live.get<strings>(key(0)) == "changed");
    }
    auto now = store.find("evolving"); assert(now && now->semantic.size() == 16 + schema.size());
    auto saved = store.find_save("before-extension"); assert(saved && saved->semantic.size() == 56 + schema.size());
    auto reopened = persistent_engine<extended_core>::connect(storage.root(), "evolving", options);
    assert(reopened.snapshot().get<append_sort>("log") == "AB" && reopened.snapshot().get<strings>(key(0)) == "changed");
    // Projecting an old cleanup obligation is one-way. An ordinary typed
    // checkpoint does not supply a clean-base/mutation history to invent.
    rejects([&] { (void)persistent_engine<>::connect(storage.root(), "evolving", {.create_if_missing = false}); });
  }
}
int main() {
  try { limits_and_cleanup(); registry_extension(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
