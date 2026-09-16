/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Admits streamed native seals and exercises durable typed runtime rebasing.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/connection.h>
#include <diet/sort_runtime_context.h>
#include <diet/typed_scan.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <set>

namespace {
  using namespace diet;
  using strings = unsorted<std::optional<std::string>>;
  using P = storage_policy<string_registry, 3>;
  using family = streaming_sort_runtime_family<P>;
  using storage = typename family::storage_type;
  using runtime = family::runtime_type<replace_native_value>;
  using core = typed_engine<P, wrapping_fingerprint_algebra, 256, family>;
  using store = runtime_store<P, random_object_ids, sqlite_catalog_ops, family>;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-stream-store-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template<class F> void rejects(F && action) {
    bool caught = false; try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t result = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++result;
    return result;
  }
  void populate(runtime & active) {
    for (unsigned n = 0; n != 4; ++n) {
      while (!active.admission_ready()) active.advance(1024);
      auto input = core::put(std::to_string(n), std::string(8192, char('a' + n)));
      assert(active.try_contribute(input.records()[0]));
    }
    while (active.pending()) active.advance(1024);
  }
  void admission() {
    temporary good, wrong;
    auto same = store::create(good.root), other = store::create(wrong.root);
    auto context = storage::open(good.root); runtime active(context); populate(active);
    auto snapshot = active.snapshot(); std::set<std::string> sealed;
    for (auto const & object : runtime_storage_codec<family>::objects(snapshot.frontier()))
      if (auto token = object->native->sealed()) {
        assert(object->native->mapped() && !object->native->owned());
        sealed.insert(token->receipt.object.hex());
      }
    assert(!sealed.empty() && context.context()->sealed_outputs());
    rejects([&] { (void)other.create_tap("alien", snapshot); });
    assert(other.failed());
    auto alien = sqlite_catalog<P>::open(wrong.root); assert(!alien.find_tap("alien"));
    auto saved = same.create_tap("same", snapshot);
    std::set<std::string> mapped;
    for (auto const & pair : saved.head.auxiliary.pairs) mapped.insert(pair.native.hex());
    for (auto const & native : saved.head.auxiliary.natives) mapped.insert(native.hex());
    mapped.insert(saved.head.timeline.head.native.hex());
    for (auto const & id : sealed) assert(mapped.contains(id));
    auto count = files(good.root);
    auto again = same.publish(saved.head, snapshot); assert(files(good.root) == count);
    assert(again.snapshot.admissions() == 4);
    auto reopened = store::open(good.root).find("same"); assert(reopened && reopened->head == again.head);
  }
  void flip(std::filesystem::path const & path, std::uint64_t offset) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    {
      std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
      file.seekg(static_cast<std::streamoff>(offset)); char value; file.read(&value, 1); assert(file);
      value ^= 1; file.seekp(static_cast<std::streamoff>(offset)); file.write(&value, 1); file.flush(); assert(file);
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);
  }
  void hidden_native() {
    temporary dir; auto adapter = store::create(dir.root); auto context = storage::open(dir.root);
    runtime active(context); auto saved = adapter.create_tap("hidden", active.snapshot());
    for (auto key : {"a", "b"}) {
      auto input = core::put(key, "value"); assert(active.try_contribute(input.records()[0]));
    }
    saved = adapter.publish(saved.head, active.checkpoint());
    bool captured = false;
    while (active.pending()) {
      auto price = active.next_service_cost(), credit = active.credit(); active.advance(price > credit ? price - credit : 1);
      auto snapshot = active.checkpoint();
      for (auto const & level : snapshot.frontier().levels) if (level.job && level.job->merged &&
          level.job->stage == redundant_stage::destination_index) {
        auto token = level.job->merged->sealed(); assert(token);
        auto count = files(dir.root); saved = adapter.publish(saved.head, snapshot);
        assert(files(dir.root) == count); // Only the completed native arrived; no new pair to seal.
        assert(std::find(saved.head.auxiliary.natives.begin(), saved.head.auxiliary.natives.end(),
          token->receipt.object) != saved.head.auxiliary.natives.end());
        auto reopened = store::open(dir.root).find("hidden"); assert(reopened && reopened->head == saved.head);
        auto resumed = runtime::from_snapshot(reopened->snapshot, storage::open(dir.root));
        while (resumed.pending()) resumed.advance(1024);
        assert(resumed.snapshot().admissions() == 2); captured = true; break;
      }
      if (captured) break;
      saved = adapter.publish(saved.head, snapshot);
    }
    assert(captured);
  }
  void header_failure() {
    temporary dir; auto adapter = store::create(dir.root); runtime empty;
    auto before = adapter.create_tap("stable", empty.snapshot());
    auto context = storage::open(dir.root); runtime active(context); populate(active);
    auto snapshot = active.snapshot(); std::filesystem::path path;
    for (auto const & object : runtime_storage_codec<family>::objects(snapshot.frontier()))
      if (auto token = object->native->sealed()) { path = token->receipt.path; break; }
    assert(!path.empty()); flip(path, 64);
    rejects([&] { (void)adapter.publish(before.head, snapshot); });
    assert(adapter.failed());
    auto reopened = store::open(dir.root); auto found = reopened.find("stable");
    assert(found && found->head == before.head && found->snapshot.admissions() == 0);
    flip(path, 64);
  }
  void named_connections() {
    temporary dir;
    {
      auto live = connect<core>(dir.root, "earth-616");
      live.put("key", "old"); auto old = live.snapshot(); live.save("old", old);
      auto count = files(dir.root); auto fork = live.fork("earth-617", old); assert(files(dir.root) == count);
      for (unsigned n = 0; n != 16; ++n) live.put("key", std::to_string(n));
      assert(live.get("key") == "15" && old.get("key") == "old" && fork.get("key") == "old");
      live.put(std::string("a\0b", 3), "embedded"); live.erase("key");
      assert(!live.get("key"));
    }
    auto reopened = persistent_engine<core>::connect(dir.root, "earth-616", {.create_if_missing = false});
    assert(reopened.snapshot().get(std::string("a\0b", 3)) == "embedded");
    auto loser = persistent_engine<core>::connect(dir.root, "earth-616", {.create_if_missing = false});
    auto before = loser.snapshot();
    auto winner = reopened.contribute(core::put("winner", "yes"));
    rejects([&] { (void)loser.contribute(core::put("loser", "no")); });
    auto rejected = loser.snapshot(); assert(loser.failed() && rejected.head() == before.head());
    auto final = persistent_engine<core>::connect(dir.root, "earth-616");
    auto current = final.snapshot();
    assert(current.head() == winner.head() && current.get("winner") == "yes");
    assert(!current.get("loser"));
  }
  struct append {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<golomb<3>>;
    using value_codec = string_value<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type old, state_type const & delta) { return old + delta; }
    static state_type compose(std::string const &, state_type old, state_type const & delta) { return old + delta; }
    static bool present(std::string const &, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::string const & key) { return u64_table_hash{}.key(key); }
    static std::uint64_t hash_value(std::string const &, state_type const & value) { return u64_table_hash{}.key(value); }
  };
  void arrows() {
    using policy = storage_policy<bin<tip<strings>, tip<append>>, 3>;
    using engine = typed_engine<policy, wrapping_fingerprint_algebra, 256, streaming_sort_runtime_family<policy>>;
    temporary dir; std::string expected; std::uint64_t signature = 0;
    for (unsigned round = 0; round != 3; ++round) {
      auto live = connect<engine>(dir.root, "arrows", {.schema_id = "application/ordered-arrows"});
      if (round) assert(live.snapshot().signature() == signature);
      for (unsigned n = round * 6; n != (round + 1) * 6; ++n) {
        auto delta = "[" + std::to_string(n) + "]"; auto batch = engine::batch();
        batch.put<strings>("latest", std::to_string(n)).change<append>("sequence", delta);
        live.apply(std::move(batch).finish()); expected += delta;
      }
      assert(live.get<append>("sequence") == expected);
      auto scan = diet::scan<append>(live.snapshot()); auto row = scan.next();
      assert(row && row->key == "sequence" && row->value == expected && !scan.next());
      signature = live.snapshot().signature();
    }
  }
}
int main() {
  try { admission(); hidden_native(); header_failure(); named_connections(); arrows(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
