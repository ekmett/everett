/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises persistent runtime frontiers, file reuse, saves and reopened carries.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/runtime_store.h>

#include <iostream>
#include <map>
#include <string>

namespace {
  using namespace diet;
  using P = storage_policy<tip<encoded_sort<bit_encoding<>>>, 3>;
  using runtime = cola_runtime<P>;
  using store = runtime_store<P>;
  void require(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class F> void rejects(F && action) {
    try { action(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected rejection");
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-runtime-store-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
  };
  profile_record record(std::string_view key, std::string_view value) {
    return {bit_string::from_bytes(key), bit_string::from_bytes(value)};
  }
  std::optional<bit_string> get(runtime::snapshot_type const & snapshot, std::string_view key) {
    auto encoded = bit_string::from_bytes(key);
    auto cursor = snapshot.cursor(encoded.view());
    while (!cursor.done()) {
      cursor.step(1);
      if (cursor.has_match()) return cursor.take_match().value;
    }
    return std::nullopt;
  }
  void check(runtime::snapshot_type const & snapshot, std::map<std::string, std::string> const & oracle) {
    for (auto const & [key, value] : oracle)
      require(get(snapshot, key) == bit_string::from_bytes(value), "persisted query differs from chronological map");
    require(!get(snapshot, "missing"), "absent key found");
  }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t result = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++result;
    return result;
  }
  void lifecycle() {
    temporary dir;
    auto storage = store::create(dir.root);
    runtime engine;
    std::array metadata{std::byte{0}, std::byte{255}, std::byte{17}};
    auto current = storage.create_tap("earth-616", engine.snapshot(), metadata);
    engine = runtime::from_snapshot(current.snapshot);
    std::map<std::string, std::string> oracle, saved_oracle;
    std::optional<stored_runtime<P>> saved;
    for (unsigned n = 0; n != 18; ++n) {
      while (engine.pending()) {
        auto before = engine.snapshot();
        auto after = engine.advance(64);
        if (!before.same_layout(after)) current = storage.publish(current.head, after, metadata);
      }
      engine = runtime::from_snapshot(current.snapshot);
      auto key = "key-" + std::to_string(n % 7), value = "value-" + std::to_string(n);
      auto next = engine.contribute(record(key, value), 0);
      oracle[key] = value;
      current = storage.publish(current.head, next, metadata);
      check(current.snapshot, oracle);
      require(current.semantic == std::vector<std::byte>(metadata.begin(), metadata.end()), "semantic bytes changed");
      if (n == 5) {
        saved = current; saved_oracle = oracle;
        storage.save("six", current.head);
      }
    }
    require(engine.pending(), "fixture did not retain interrupted carry");
    auto count = files(dir.root);
    auto again = storage.publish(current.head, engine.snapshot(), metadata);
    require(files(dir.root) == count && again.head.timeline.generation == current.head.timeline.generation + 1,
      "unchanged snapshot rewrote object files");
    current = std::move(again);
    check(saved->snapshot, saved_oracle);
    auto reopened = store::open(dir.root);
    auto found = reopened.find("earth-616");
    require(found.has_value() && found->head == current.head, "latest named root not reopened");
    check(found->snapshot, oracle);
    auto resumed = runtime::from_snapshot(found->snapshot);
    require(resumed.pending(), "pending frontier did not restore work");
    while (resumed.pending()) resumed.advance(17);
    auto completed = reopened.publish(found->head, resumed.snapshot(), found->semantic);
    check(completed.snapshot, oracle);
    require(completed.snapshot.settled(), "resumed carry not published");
    auto old = reopened.find_save("six");
    require(old && old->head == saved->head, "save no longer names historical generation");
    check(old->snapshot, saved_oracle);
    auto branch = reopened.fork("earth-617", old->head);
    auto fork = runtime::from_snapshot(branch.snapshot);
    auto forked = fork.contribute(record("branch", "alone"));
    auto published = reopened.publish(branch.head, forked, {});
    require(get(published.snapshot, "branch") == bit_string::from_bytes("alone") &&
      !get(completed.snapshot, "branch"), "fork changed original snapshot");
    // A second process-equivalent connection must not overwrite the winner.
    rejects([&] { storage.publish(current.head, engine.snapshot(), metadata); });
    require(storage.failed() && !storage.last_operation().empty(), "conflict did not stop adapter");
    require(store::open(dir.root).find("earth-616")->head == completed.head, "conflict moved durable head");
    auto malformed = completed.head.checkpoint;
    malformed[8] = std::byte{255};
    rejects([&] { (void)runtime_store_detail::checkpoint(malformed); });
    auto cut = completed.head.checkpoint;
    cut.pop_back();
    rejects([&] { (void)runtime_store_detail::checkpoint(cut); });
  }
}
int main() {
  try { lifecycle(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
