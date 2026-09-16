/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises durable mutable typed sessions, snapshots, reopening and failed publications.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>

#include <array>
#include <cassert>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <thread>

namespace {
  using namespace everett;
  using core = typed_engine<>;
  using engine = persistent_engine<core>;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-connection-XXXXXX").string();
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
  std::size_t files(std::filesystem::path const & root) {
    std::size_t count = 0;
    for (auto const & item : std::filesystem::recursive_directory_iterator(root))
      if (item.path().extension() == ".kv" || item.path().extension() == ".index") ++count;
    return count;
  }
  template <class F> void eventually(F && ready) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!ready()) {
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("background publication timed out");
      std::this_thread::yield();
    }
  }
  void simple() {
    temporary directory;
    auto storage = multiverse<>::create(directory.root / "nested" / "storage");
    static_assert(std::same_as<decltype(storage)::policy_type, string_policy>);
    auto current = connect<core>(storage.root(), "earth-616");
    assert(!current.get("name"));
    auto first = current.put("name", "Everett");
    assert(first.get("name") == "Everett" && first.live_count() == 1);
    for (auto node = first.runtime().query_root().head(); node; node = node->main_target())
      assert(node->mapped() && node->native_owner()->mapped() && !node->native_owner()->owned());
    auto second_ticket = current.put_async("other", std::string("value\0bytes", 11));
    auto second = second_ticket.get();
    assert(second->world.get("other") == std::string("value\0bytes", 11));
    assert(!second->world.runtime().settled());
    eventually([&] { return current.publication()->world.runtime().settled(); });
    auto compacted = current.publication();
    assert(compacted->logical == second->logical && compacted->generation == second->generation);
    assert(compacted->revision > second->revision);
    assert(compacted->world.signature() == second->world.signature());
    assert(compacted->world.live_count() == second->world.live_count());
    assert(first.get("name") == "Everett" && !first.get("other"));
    auto before_save = files(storage.root());
    current.save("first", first);
    auto saved = current.load("first");
    assert(saved && saved->head() == first.head() && saved->get("name") == "Everett");
    assert(files(storage.root()) == before_save);
    assert(!current.load("absent"));
    auto branch = current.fork("earth-617", *saved);
    branch.put("name", "Branch");
    assert(branch.get("name") == "Branch" && current.get("name") == "Everett");
    assert(saved->get("name") == "Everett");
    current.erase("name");
    assert(!current.get("name") && current.snapshot().live_count() == 1);
    auto rejected = current.erase_async("absent");
    rejects([&] { (void)rejected.get(); });
    assert(!current.failure());
    current.put("still", "healthy");
    assert(current.get("still") == "healthy");
    current.shutdown(); branch.shutdown();
    auto opened = connect<core>(storage.root(), "earth-616", {.create_if_missing = false});
    assert(!opened.get("name") && opened.get("still") == "healthy");
    assert(opened.snapshot().live_count() == 2);
    auto old = opened.load("first");
    assert(old && old->get("name") == "Everett" && !old->get("still"));
  }

  void queued_commands() {
    temporary directory;
    auto current = connect<core>(directory.root, "commands");
    std::vector<connection<core>::ticket> tickets;
    for (unsigned i = 0; i != 20; ++i) tickets.push_back(current.put_async("same", std::to_string(i)));
    for (unsigned i = 0; i != tickets.size(); ++i) assert(tickets[i].get()->world.get("same") == std::to_string(i));
    assert(current.get("same") == "19");
    auto base = current.snapshot();
    auto left = base.put("left", "L"), right = base.put("right", "R");
    current.apply(std::move(left)); current.apply(std::move(right));
    assert(current.get("left") == "L" && current.get("right") == "R");
    auto stale = base.put("same", "stale");
    current.put("same", "new");
    rejects([&] { current.apply(std::move(stale)); });
    assert(!current.failure() && current.get("same") == "new");
    std::array<std::future<void>, 4> writers;
    for (unsigned i = 0; i != writers.size(); ++i) writers[i] = std::async(std::launch::async, [&, i] {
      for (unsigned j = 0; j != 4; ++j) {
        auto value = std::to_string(i) + "-" + std::to_string(j);
        auto done = current.put_async("shared", value).get();
        assert(done->world.get("shared") == value);
      }
    });
    for (auto & task : writers) task.get();
    assert(!current.failure() && current.snapshot().live_count() == 4);
  }

  void pending_restart() {
    temporary directory;
    auto store = engine::connect(directory.root, "restart");
    auto first = store.contribute(core::put("k", "before"));
    auto second = store.contribute(core::put("k", "after"));
    assert(store.pending() && !second.runtime().settled());
    auto resumed = engine::connect(directory.root, "restart", {.create_if_missing = false});
    auto restored = resumed.snapshot();
    assert(resumed.pending() && restored.head() == second.head());
    assert(resumed.snapshot().signature() == second.signature());
    auto unchanged = restored.head();
    assert(!resumed.advance(1)); // cannot pay merge setup yet
    restored = resumed.snapshot();
    assert(restored.head() == unchanged);
    unsigned steps = 0;
    while (resumed.pending()) { (void)resumed.advance(1); assert(++steps < 1000); }
    restored = resumed.snapshot();
    assert(restored.runtime().settled() && restored.get("k") == "after");
    assert(restored.head().timeline.generation == second.head().timeline.generation + 1);
    assert(first.get("k") == "before");
    auto after_merge = resumed.contribute(core::put("new", "value"));
    assert(after_merge.get("new") == "value");
    // A connection that retained the pre-maintenance generation loses its CAS.
    rejects([&] { store.contribute(core::put("loser", "value")); });
    assert(store.failed());
    assert(!store.snapshot().get("loser") && store.snapshot().get("k") == "after");
    auto reopened = engine::connect(directory.root, "restart", {.create_if_missing = false});
    assert(reopened.snapshot().get("new") == "value" && !reopened.snapshot().get("loser"));
  }

  struct faulty_ids {
    std::shared_ptr<bool> armed;
    object_id operator()() const {
      if (*armed) throw std::runtime_error("intentional identity allocation failure");
      return random_object_ids{}();
    }
  };
  void failed_publication() {
    temporary directory;
    auto fail = std::make_shared<bool>(false);
    using faulty = persistent_engine<core, faulty_ids>;
    auto store = faulty::connect(directory.root, "failure", {}, faulty_ids{fail});
    auto first = store.contribute(core::put("k", "old"));
    rejects([&] { store.contribute(core::erase("absent")); });
    auto still = store.snapshot();
    assert(!store.failed() && still.head() == first.head());
    *fail = true;
    rejects([&] { store.contribute(core::put("k", "unpublished")); });
    still = store.snapshot();
    assert(store.failed() && still.head() == first.head());
    rejects([&] { store.advance(100); });
    auto reopened = engine::connect(directory.root, "failure", {.create_if_missing = false});
    assert(reopened.snapshot().get("k") == "old");
  }

  struct restore_gate {
    std::promise<void> entered, release;
    std::shared_future<void> proceed = release.get_future().share();
  };
  struct decode_failure {
    inline static bool armed = false;
    inline static std::shared_ptr<restore_gate> blocked;
    static core::metadata_type decode(std::span<std::byte const> data) {
      // The test installs this gate before starting the worker and clears it
      // only after joining, so its shared pointer needs no concurrent mutation.
      if (auto gate = blocked) {
        gate->entered.set_value();
        if (gate->proceed.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
          throw std::runtime_error("metadata restore gate timed out");
        throw std::runtime_error("intentional metadata restore failure");
      }
      if (armed) throw std::runtime_error("intentional metadata restore failure");
      return core::metadata_type::decode(data);
    }
  };
  struct restore_fault_core : core {
    using core::core;
    using metadata_type = decode_failure;
    static restore_fault_core from_snapshot(world_type value) {
      return restore_fault_core(core::from_snapshot(std::move(value)));
    }
  private:
    explicit restore_fault_core(core value) : core(std::move(value)) {}
  };
  void committed_restore_failure() {
    temporary directory;
    using faulty = persistent_engine<restore_fault_core>;
    auto current = faulty::connect(directory.root, "committed");
    auto first = current.contribute(core::put("k", "old"));
    decode_failure::armed = true;
    rejects([&] { current.contribute(core::put("k", "committed")); });
    decode_failure::armed = false;
    auto visible = current.snapshot();
    assert(current.failed() && visible.head() == first.head() && visible.get("k") == "old");
    auto restored = engine::connect(directory.root, "committed", {.create_if_missing = false});
    auto durable = restored.snapshot();
    assert(durable.get("k") == "committed");
    assert(durable.head().timeline.generation == first.head().timeline.generation + 1);
    assert(durable.metadata().schema_id == first.metadata().schema_id && durable.live_count() == 1);
    core oracle;
    oracle.contribute(core::put("k", "old"));
    auto expected = oracle.contribute(core::put("k", "committed"));
    assert(durable.metadata() == expected.metadata());
  }

  void async_committed_restore_failure() {
    temporary directory;
    using faulty = persistent_engine<restore_fault_core>;
    auto current = faulty::connect(directory.root, "async-committed");
    current.contribute(core::put("k", "old"));
    while (current.pending()) current.advance(16384);
    auto first = current.snapshot();
    auto gate = std::make_shared<restore_gate>();
    auto entered = gate->entered.get_future();
    decode_failure::blocked = gate;
    struct reset_gate {
      ~reset_gate() { decode_failure::blocked.reset(); }
    } reset;
    session<faulty> live(std::move(current), {std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), 4, 16384});
    auto before = live.snapshot();
    auto active = live.submit(core::put("k", "committed"));
    assert(entered.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    auto queued = live.submit(core::put("queued", "never-applied"));
    assert(!active.ready() && !queued.ready() && live.pending_count() == 2);
    assert(live.snapshot() == before && before->world.get("k") == "old");
    // The durable head is already newer while no successful ticket exists.
    auto committed = engine::connect(directory.root, "async-committed", {.create_if_missing = false});
    auto observed = committed.snapshot();
    assert(observed.get("k") == "committed" && !observed.get("queued"));
    assert(observed.head().timeline.generation == first.head().timeline.generation + 1);
    live.close();
    gate->release.set_value();
    active.wait(); queued.wait(); // Waiting observes completion, not success.
    assert(active.ready() && queued.ready());
    std::exception_ptr failure;
    for (auto const * ticket : {&active, &queued}) {
      bool threw = false;
      try { (void)ticket->get(); }
      catch (std::runtime_error const & error) {
        assert(std::string_view(error.what()) == "intentional metadata restore failure");
        if (!failure) failure = std::current_exception();
        else assert(failure == std::current_exception());
        threw = true;
      }
      assert(threw);
    }
    live.shutdown(); // Joins normally despite the failed worker and tickets.
    assert(live.failure() == failure && live.pending_count() == 0);
    assert(live.outstanding() == session_reservation{});
    assert(live.snapshot() == before && first.get("k") == "old");
    auto reopened = engine::connect(directory.root, "async-committed", {.create_if_missing = false});
    auto durable = reopened.snapshot();
    assert(durable.head() == observed.head());
    assert(durable.get("k") == "committed" && !durable.get("queued"));
    core oracle;
    oracle.contribute(core::put("k", "old"));
    auto expected = oracle.contribute(core::put("k", "committed"));
    assert(durable.metadata() == expected.metadata());
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
    using append_core = typed_engine<policy>;
    using append_engine = persistent_engine<append_core>;
    connection_options options{.schema_id = "tests.append/v1"};
    temporary directory;
    std::string expected;
    {
      auto current = connect<append_core>(directory.root, "append", options);
      for (unsigned i = 0; i != 13; ++i) {
        auto value = std::to_string(i) + "/";
        expected += value;
        assert(current.change("k", value).get("k") == expected);
      }
    }
    auto resumed = append_engine::connect(directory.root, "append", options);
    assert(resumed.snapshot().get("k") == expected);
    for (unsigned i = 13; i != 18; ++i) {
      auto value = std::to_string(i) + "/";
      expected += value;
      assert(resumed.contribute(append_core::change("k", value)).get("k") == expected);
    }
    assert(resumed.pending());
    auto restarted = append_engine::connect(directory.root, "append", options);
    assert(restarted.pending());
    auto signature = restarted.snapshot().signature();
    while (restarted.pending()) restarted.advance(11);
    assert(restarted.snapshot().get("k") == expected && restarted.snapshot().signature() == signature);
    rejects([&] { (void)append_engine::connect(directory.root, "append"); });
    options.schema_id = "tests.wrong/v1";
    rejects([&] { (void)append_engine::connect(directory.root, "append", options); });
  }

  void creation_checks() {
    temporary directory;
    connection_options invalid;
    invalid.limits = session_limits{128'000'000, 64 * 1024 * 1024, 0, 4096};
    rejects([&] { (void)connect(directory.root, "x", invalid); });
    rejects([&] { (void)connect(directory.root, ""); });
    rejects([&] { (void)connect(directory.root / "missing", "x"); });
    rejects([&] { (void)connect(directory.root, "unknown", {.create_if_missing = false}); });
    assert(!std::filesystem::exists(directory.root / "catalog.sqlite3"));
    { std::ofstream incomplete(directory.root / "catalog.sqlite3"); }
    rejects([&] { (void)connect(directory.root, "x"); });
    assert(std::filesystem::file_size(directory.root / "catalog.sqlite3") == 0);
    { std::ofstream incomplete(directory.root / "catalog.sqlite3"); incomplete << "not a database"; }
    rejects([&] { (void)connect(directory.root, "x"); });
    std::ifstream existing(directory.root / "catalog.sqlite3");
    std::string data((std::istreambuf_iterator<char>(existing)), {});
    assert(data == "not a database");
  }
}

int main() {
  try { simple(); queued_commands(); pending_restart(); failed_publication(); committed_restore_failure();
    async_committed_restore_failure();
    noncommutative(); creation_checks(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
