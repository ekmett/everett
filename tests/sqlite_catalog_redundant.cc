/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises durable redundant frontiers with hidden job artifacts and exact historical pins.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/redundant_checkpoint.h>
#include <diet/connection.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <map>

namespace {
  using namespace diet;
  using P = storage_policy<tip<encoded_sort<bit_encoding<>>>, 3>;
  using family = redundant_runtime_family<P>;
  using runtime = redundant_runtime<P>;
  using store = runtime_store<P, random_object_ids, sqlite_catalog_ops, family>;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-redundant-store-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action, std::string_view why) {
    bool caught = false; try { action(); } catch (std::exception const &) { caught = true; }
    if (!caught) throw std::runtime_error(std::string(why));
  }
  profile_record row(std::string_view key, std::string_view value) {
    return {bit_string::from_bytes(key), bit_string::from_bytes(value)};
  }
  std::optional<bit_string> get(redundant_snapshot<P> const & snapshot, std::string_view key) {
    auto encoded = bit_string::from_bytes(key); auto cursor = snapshot.cursor(encoded.view());
    while (!cursor.done()) { cursor.step(1); if (cursor.has_match()) return cursor.take_match().value; }
    return std::nullopt;
  }
  void check(redundant_snapshot<P> const & snapshot, std::map<std::string, std::string> const & expected) {
    for (auto const & [key, value] : expected) assert(get(snapshot, key) == bit_string::from_bytes(value));
  }
  void drain(runtime & active) {
    unsigned steps = 0;
    while (active.pending()) { active.advance(31); assert(++steps < 100000); }
    assert(active.admission_ready());
  }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t count = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++count;
    return count;
  }
  void restart_every_stage() {
    temporary dir; auto storage = store::create(dir.root); runtime active;
    auto published = storage.create_tap("stages", active.snapshot());
    active.try_contribute(row("a", "first")); active.try_contribute(row("b", "second"));
    unsigned seen = 0, steps = 0;
    while (active.pending()) {
      assert(++steps < 10000);
      auto snapshot = active.checkpoint();
      unsigned now = 0;
      for (auto const & level : snapshot.frontier().levels) if (level.job) now |= 1u << static_cast<unsigned>(level.job->stage);
      if (now & ~seen) {
        std::array semantic{std::byte(now)};
        published = storage.publish(published.head, snapshot, semantic);
        auto count = files(dir.root);
        auto duplicated = storage.publish(published.head, snapshot, semantic);
        assert(files(dir.root) == count); published = std::move(duplicated);
        assert(!published.head.auxiliary.pairs.empty() && !published.head.auxiliary.natives.empty());
        auto reopened = store::open(dir.root); auto saved = reopened.find("stages");
        assert(saved && saved->head == published.head && saved->semantic == std::vector<std::byte>(semantic.begin(), semantic.end()));
        auto const & frontier = saved->snapshot.frontier();
        assert(frontier.admissions == snapshot.admissions() && frontier.service_due == snapshot.frontier().service_due);
        for (auto const & object : runtime_storage_codec<family>::objects(frontier)) {
          assert(object->native->mapped() && !object->native->owned());
          if (object->pair) assert(object->pair->mapped() && object->pair->native_owner() == object->native);
        }
        auto restored = runtime::from_snapshot(saved->snapshot);
        assert(restored.recovering() && !restored.admission_ready());
        assert(!restored.try_contribute(row("forbidden", "until recovery finishes")));
        drain(restored); check(restored.snapshot(), {{"a", "first"}, {"b", "second"}});
        if (now == 2 || now == 8) {
          // The catalog treats checkpoint bytes as opaque; the family loader
          // must still refuse a hidden artifact omitted from durable pins.
          auto catalog = sqlite_catalog<P>::open(dir.root);
          auto name = "missing-pin-" + std::to_string(now);
          auto forged = catalog.fork_tap(name + "-fork", name, saved->head);
          auto pins = forged.auxiliary;
          if (now == 2) pins.natives.clear(); else pins.pairs.clear();
          (void)catalog.publish_tap(name + "-publish", forged, forged.timeline.head, forged.checkpoint, pins);
          auto invalid = store::open(dir.root);
          rejects([&] { (void)invalid.find(name); }, name);
        }
        if (!seen) {
          reopened.save("initial-carry", saved->head);
          auto forked = reopened.fork("branch", saved->head);
          assert(forked.head.auxiliary == saved->head.auxiliary && files(dir.root) == count);
          auto old = reopened.find_save("initial-carry"); assert(old && old->head == saved->head);
        }
        seen |= now;
      }
      if (!active.pending()) break;
      auto cost = active.next_service_cost(), credit = active.credit();
      active.advance(cost > credit ? cost - credit : 1);
    }
    assert(seen == 15);
    auto final = storage.publish(published.head, active.snapshot());
    check(final.snapshot, {{"a", "first"}, {"b", "second"}});
  }
  void lifecycle() {
    temporary dir; auto storage = store::create(dir.root); runtime active;
    auto current = storage.create_tap("live", active.snapshot());
    std::map<std::string, std::string> expected, history;
    std::optional<stored_runtime<P, family>> saved;
    for (unsigned n = 0; n != 48; ++n) {
      auto key = "key-" + std::to_string(n % 9), value = "value-" + std::to_string(n);
      expected[key] = value;
      current = storage.publish(current.head, active.contribute(row(key, value)));
      check(current.snapshot, expected);
      if (n == 17) { saved = current; history = expected; storage.save("eighteen", current.head); }
      if (n % 7 == 0) {
        auto reopened = store::open(dir.root); auto found = reopened.find("live"); assert(found);
        auto resumed = runtime::from_snapshot(found->snapshot); drain(resumed); check(resumed.snapshot(), expected);
      }
    }
    check(saved->snapshot, history);
    auto reopened = store::open(dir.root); auto historical = reopened.find_save("eighteen");
    assert(historical && historical->head == saved->head); check(historical->snapshot, history);
    auto branch = reopened.fork("branch", historical->head);
    auto fork = runtime::from_snapshot(branch.snapshot); drain(fork);
    auto changed = reopened.publish(branch.head, fork.contribute(row("branch", "only")));
    assert(get(changed.snapshot, "branch") && !get(current.snapshot, "branch"));
  }
  void metadata_only_reopen() {
    temporary dir; auto storage = store::create(dir.root); runtime active;
    auto current = storage.create_tap("metadata", active.contribute(row("key", "payload")));
    auto object = current.snapshot.frontier().root.main;
    auto native = object->native->mapped(); assert(native && native->view().bytes().size());
    auto id = object->pair->mapped()->identity().native;
    auto path = dir.root / object_path(id, file_kind::native_blob);
    auto offset = file_detail::header_bytes + native->layout().sections[0].offset;
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    {
      std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
      assert(file);
      file.seekg(static_cast<std::streamoff>(offset)); char byte; file.get(byte);
      file.seekp(static_cast<std::streamoff>(offset)); file.put(char(byte ^ 1));
      file.flush(); assert(file);
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);
    auto reopened = store::open(dir.root); auto found = reopened.find("metadata");
    assert(found && found->head == current.head); // No eager body CRC or key scan.
    rejects([&] { found->snapshot.frontier().root.main->native->mapped()->scan(); }, "payload corruption escaped scan");
  }
  void typed_connection() {
    using core = typed_engine<string_policy, wrapping_fingerprint_algebra, 256, redundant_runtime_family<string_policy>>;
    temporary dir;
    {
      auto live = connect<core>(dir.root, "typed");
      live.put("key", "before"); auto old = live.snapshot();
      for (unsigned i = 0; i != 12; ++i) live.put("key", std::to_string(i));
      assert(live.get("key") == "11" && old.get("key") == "before");
      live.save("old", old); auto branch = live.fork("branch", old); branch.put("key", "independent");
      live.erase("key"); assert(!live.get("key") && branch.get("key") == "independent");
    }
    auto reopened = connect<core>(dir.root, "typed", {.create_if_missing = false});
    assert(!reopened.get("key") && reopened.load("old")->get("key") == "before");
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
  void noncommutative() {
    using policy = storage_policy<tip<append_sort>, 3>;
    using core = typed_engine<policy, wrapping_fingerprint_algebra, 256, redundant_runtime_family<policy>>;
    temporary dir;
    connection_options options{.schema_id = "test.redundant-append/v1"};
    std::string expected;
    std::uint64_t signature = 0;
    for (unsigned phase = 0; phase != 3; ++phase) {
      auto live = connect<core>(dir.root, "append", options);
      assert(live.get("key") == expected);
      if (phase) assert(live.snapshot().signature() == signature);
      for (unsigned n = phase * 7; n != (phase + 1) * 7; ++n) {
        auto change = std::to_string(n) + "/"; expected += change;
        auto result = live.change("key", change);
        assert(result.get("key") == expected && result.live_count() == 1);
      }
      signature = live.snapshot().signature();
    }
  }
}
int main() {
  try { restart_every_stage(); lifecycle(); metadata_only_reopen(); typed_connection(); noncommutative(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
