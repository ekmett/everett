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
  void reindex_native_reuse() {
    temporary dir;
    auto storage = store::create(dir.root);
    runtime a, b;
    auto left = storage.create_tap("left", a.contribute(record("a", "left"), 0));
    auto right = storage.create_tap("right", b.contribute(record("b", "right"), 0));
    auto old_native = left.snapshot.runs()[0].node->mapped()->identity().native;
    auto native = left.snapshot.runs()[0].native_owner();
    using node = cola_runtime_node<P>;
    auto changed = node::from_built(node::built_type::adopt_native(native, right.snapshot.query_root().head()));
    std::array intervals{cola_runtime_interval{0, 1}, cola_runtime_interval{1, 2}};
    auto snapshot = cola_runtime_snapshot<P>::restore(changed, intervals);
    auto before = files(dir.root);
    auto combined = storage.create_tap("combined", snapshot);
    require(files(dir.root) == before + 1, "reindex rewrote unchanged native or target");
    require(combined.snapshot.runs()[1].node->mapped()->identity().native == old_native,
      "reindex changed native identity");
    check(combined.snapshot, {{"a", "left"}, {"b", "right"}});
  }
  struct hidden_after_commit {
    bool armed = false, renamed = false;
    std::filesystem::path source, hidden;
  };
  struct hide_after_commit_ops {
    std::shared_ptr<hidden_after_commit> state;
    int commit(sqlite3 * db) noexcept {
      auto code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      if (code == SQLITE_OK && state->armed) {
        state->armed = false;
        state->renamed = ::rename(state->source.c_str(), state->hidden.c_str()) == 0;
      }
      return code;
    }
  };
  void mapped_publication_retains_owner() {
    temporary dir;
    auto normal = store::create(dir.root);
    runtime engine;
    auto first = normal.create_tap("earth-616", engine.contribute(record("a", "value"), 0));
    auto state = std::make_shared<hidden_after_commit>();
    state->source = dir.root / object_path(first.head.timeline.head.index, file_kind::fractional_index);
    state->hidden = state->source.string() + ".held";
    using faulty_store = runtime_store<P, random_object_ids, hide_after_commit_ops>;
    auto faulty = faulty_store::open(dir.root, {}, {}, hide_after_commit_ops{state});
    auto source = faulty.find("earth-616");
    std::array metadata{std::byte{42}};
    state->armed = true;
    auto published = faulty.publish(source->head, source->snapshot, metadata);
    require(state->renamed && !faulty.failed(), "publication did not retain its existing mapping");
    require(published.snapshot.query_root().head() == source->snapshot.query_root().head(),
      "publication rebuilt an already retained mapped suffix");
    require(get(published.snapshot, "a") == bit_string::from_bytes("value"), "retained mapping stopped answering");
    rejects([&] { (void)store::open(dir.root).find("earth-616"); });
    require(::rename(state->hidden.c_str(), state->source.c_str()) == 0, "restore fixture file");
    auto reopened = store::open(dir.root).find("earth-616");
    require(reopened && reopened->head.timeline.generation == source->head.timeline.generation + 1 &&
      reopened->head.timeline.head == source->head.timeline.head && reopened->semantic == std::vector<std::byte>{std::byte{42}},
      "reopen lost committed root/checkpoint after restoring its filename");
    require(get(source->snapshot, "a") == bit_string::from_bytes("value"), "publication invalidated old mapping");
  }
}
int main() {
  try { lifecycle(); reindex_native_reuse(); mapped_publication_retains_owner(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
