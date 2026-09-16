/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks private transaction flushes, persistent nurseries and single checked publication.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>

#include <atomic>
#include <cassert>
#include <iostream>
#include <map>

namespace {
  using namespace everett;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "everett-transaction-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action) {
    bool caught = false;
    try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  template <class Database> auto head(Database const & db) { auto world = db.snapshot(); return world.head(); }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t n = 0;
    for (auto const & item : std::filesystem::recursive_directory_iterator(root))
      n += item.path().extension() == ".kv" || item.path().extension() == ".index";
    return n;
  }
  template <class Core> void private_flushes() {
    temporary directory;
    connection<Core> db(directory.root, "main");
    auto base = db.snapshot();
    auto tx = db.begin();
    auto no_files = files(directory.root);
    (void)tx.flush();
    assert(files(directory.root) == no_files);
    rejects([&] { tx.erase("absent"); });
    assert(!tx.failed());
    tx.put("gone", "temporary"); tx.erase("gone");
    assert(tx.snapshot().buffered_keys() == 0);
    tx.put("a", "first"); tx.put("a", "second");
    auto frozen = tx.snapshot();
    assert(frozen.buffered_keys() == 1 && frozen.get("a") == "second");
    tx.put("a", "third"); tx.put("b", "B");
    auto staged = tx.flush();
    assert(staged.buffered_keys() == 0 && staged.get("a") == "third");
    assert(frozen.get("a") == "second" && !frozen.get("b"));
    assert(files(directory.root) > no_files && head(db) == base.head());
    auto observer = persistent_engine<Core>::connect(directory.root, "main", {.create_if_missing = false});
    assert(!observer.snapshot().get("a") && head(observer) == base.head());
    tx.erase("a"); tx.put("c", "C");
    (void)tx.flush();
    assert(head(db) == base.head() && staged.get("a") == "third");
    tx.put("d", "D");
    auto committed = tx.commit();
    assert(committed.head().timeline.generation == base.head().timeline.generation + 1);
    assert(!committed.get("a") && committed.get("b") == "B" && committed.get("c") == "C" && committed.get("d") == "D");
    assert(committed.live_count() == 3);
    using sort = unsorted<std::optional<std::string>>;
    std::uint64_t hash = 0;
    for (auto key : {"b", "c", "d"}) hash += sort_semantics<sort>::hash_key(key) *
      sort_semantics<sort>::hash_value(key, committed.get(key));
    assert(committed.signature() == hash && !base.get("b"));
    rejects([&] { tx.put("late", "no"); });
    db.save("committed", committed);
    db.shutdown();
    auto reopened = connect<Core>(directory.root, "main", {.create_if_missing = false});
    assert(reopened.get("b") == "B" && !reopened.get("a"));
    assert(reopened.load("committed")->signature() == committed.signature());
  }

  void branches_and_abort() {
    temporary directory;
    auto db = connect(directory.root, "main");
    auto tx = db.begin();
    tx.put("shared", "before");
    auto staged = tx.flush();
    tx.put("shared", "after");
    auto saved = tx.snapshot();
    auto branch = saved.branch();
    tx.abort();
    assert(saved.get("shared") == "after" && staged.get("shared") == "before");
    assert(!db.get("shared"));
    branch.put("branch", "only");
    auto final = branch.commit();
    assert(final.get("shared") == "after" && final.get("branch") == "only");
    auto stale_branch = staged.branch();
    stale_branch.put("loser", "no");
    rejects([&] { (void)stale_branch.commit(); });
    assert(!db.failure() && !db.get("loser"));
    db.put("healthy", "yes");
    auto empty = db.begin(); auto before = db.snapshot();
    auto unchanged = empty.commit();
    // The empty commit itself publishes no new root; equivalent maintenance
    // may have advanced the worker before it claimed that ticket.
    assert(unchanged.logical_identity() == before.logical_identity());
    assert(unchanged.get("healthy") == "yes");
  }

  void conflicts_and_lifetime() {
    temporary directory;
    auto db = connect<typed_engine<>>(directory.root, "main");
    auto tx = db.begin(); tx.put("private", "P"); (void)tx.flush();
    db.put("public", "new");
    rejects([&] { (void)tx.commit(); });
    assert(!db.failure() && !db.get("private") && db.get("public") == "new");
    auto stale = db.begin(); stale.put("private", "again");
    auto independent = connect<typed_engine<>>(directory.root, "main", {.create_if_missing = false});
    independent.put("external", "writer"); independent.shutdown();
    rejects([&] { (void)stale.commit(); });
    assert(db.failure() && !db.get("private"));
    db.shutdown();
    auto reopened = connect<typed_engine<>>(directory.root, "main", {.create_if_missing = false});
    assert(reopened.get("external") == "writer" && !reopened.get("private"));
    using tx_type = transaction<typed_engine<>>;
    std::optional<typename tx_type::snapshot_type> retained;
    {
      auto ephemeral = connect<typed_engine<>>(directory.root, "other");
      auto local = ephemeral.begin(); local.put("held", "snapshot");
      retained.emplace(local.flush());
    }
    assert(retained->get("held") == "snapshot");
    auto orphan = retained->branch();
    rejects([&] { (void)orphan.commit(); });
    orphan.abort();
  }

  void equivalent_revision() {
    using core = typed_engine<>;
    using engine = persistent_engine<core>;
    temporary directory;
    auto live = engine::connect(directory.root, "main");
    live.contribute(core::put("a", "A"));
    auto base = live.contribute(core::put("b", "B"));
    auto candidate = std::make_unique<core>(core::from_snapshot(base));
    candidate->contribute(core::put("c", "C"));
    assert(live.pending());
    while (live.pending()) live.advance(4096);
    auto newer = live.snapshot();
    assert(newer.head() != base.head() && newer.logical_identity() == base.logical_identity());
    auto committed = live.contribute(typename engine::prepared_transaction{std::move(candidate), base, {}});
    assert(committed.get("c") == "C" && committed.head().timeline.generation == newer.head().timeline.generation + 1);
    assert(committed.logical_identity() != base.logical_identity());
  }

  struct append_sort {
    using encoding = byte_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = string_value<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type a, state_type const & b) { return a + b; }
    static state_type compose(std::string const &, state_type a, state_type const & b) { return a + b; }
    static bool present(std::string const &, state_type const & v) { return !v.empty(); }
    static std::uint64_t hash_key(std::string const & k) { return u64_table_hash{}.key(k); }
    static std::uint64_t hash_value(std::string const &, state_type const & v) { return u64_table_hash{}.key(v); }
  };
  struct text_sort : sort_semantics<unsorted<std::optional<std::string>>> {
    using encoding = byte_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
  };
  void arbitrary_arrows() {
    using policy = storage_policy<bin<tip<append_sort>, tip<text_sort>>>;
    using core = typed_engine<policy>;
    temporary directory;
    connection<core> db(directory.root, "arrows", {.schema_id = "transaction/mixed-v1"});
    db.change<append_sort>("same", "base/");
    auto tx = db.begin();
    tx.change<append_sort>("same", "one/");
    tx.change<append_sort>("same", "two/");
    tx.put<text_sort>("same", "text");
    auto old = tx.snapshot();
    assert(old.get<append_sort>("same") == "base/one/two/");
    assert(old.get<text_sort>("same") == "text" && old.buffered_keys() == 2);
    (void)tx.flush();
    tx.change<append_sort>("same", "three");
    auto done = tx.commit();
    assert(done.get<append_sort>("same") == "base/one/two/three");
    assert(done.get<text_sort>("same") == "text" && old.get<append_sort>("same") == "base/one/two/");
    auto expected = append_sort::hash_key("same") * append_sort::hash_value("same", "base/one/two/three") +
      text_sort::hash_key("same") * text_sort::hash_value("same", std::optional<std::string>("text"));
    assert(done.signature() == expected && done.live_count() == 2);
    db.shutdown();
    connection<core> reopened(directory.root, "arrows", {.schema_id = "transaction/mixed-v1", .create_if_missing = false});
    assert(reopened.get<append_sort>("same") == "base/one/two/three");
  }

  struct admission_gate {
    std::atomic<bool> first = true;
    std::promise<void> entered, release;
    std::shared_future<void> proceed = release.get_future().share();
  };
  struct blocked_core : typed_engine<> {
    using base = typed_engine<>;
    using base::base;
    inline static std::shared_ptr<admission_gate> gate;
    static blocked_core from_snapshot(world_type world) { return blocked_core(base::from_snapshot(std::move(world))); }
    world_type contribute(contribution_type input) {
      if (auto active = gate; active && active->first.exchange(false)) {
        active->entered.set_value();
        if (active->proceed.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
          throw std::runtime_error("transaction admission gate timed out");
      }
      return base::contribute(std::move(input));
    }
  private:
    explicit blocked_core(base value) : base(std::move(value)) {}
  };
  void cancellation_and_backpressure() {
    temporary directory;
    auto gate = std::make_shared<admission_gate>();
    auto entered = gate->entered.get_future();
    blocked_core::gate = gate;
    struct reset { ~reset() { blocked_core::gate.reset(); } } cleanup;
    connection_options options;
    options.limits = session_limits{100'000'000, 1'000'000, 2, 4096};
    connection<blocked_core> db(directory.root, "queue", options);
    auto active = db.put_async("active", "A");
    assert(entered.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    auto tx = db.begin(); tx.put("cancelled", "private");
    auto queued = tx.commit_async();
    assert(db.pending_count() == 2 && !queued.ready());
    auto command = blocked_core::put("waiting", "unchanged");
    auto original = command.records().front();
    assert(!db.try_submit(std::move(command)));
    assert(command.records().size() == 1 && command.records().front().key == original.key &&
      command.records().front().value == original.value);
    assert(!db.try_submit(command));
    assert(db.cancel(queued));
    rejects([&] { (void)queued.get(); });
    gate->release.set_value();
    assert(active.get()->world.get("active") == "A");
    db.shutdown();
    assert(!db.failure() && !db.get("cancelled") && !db.get("waiting"));
  }

  struct transformed_sort {
    using encoding = byte_encoding<fixed_values<8>>;
    using key_codec = unsigned_key<16>;
    using value_codec = unsigned_value<64>;
    struct state_type { std::uint64_t value; bool operator==(state_type const &) const = default; };
    static constexpr bool replacement = true;
    static state_type initial(std::uint64_t) { return {0}; }
    static state_type apply(std::uint64_t, state_type, std::uint64_t arrow) { return {2 * arrow}; }
    static std::uint64_t compose(std::uint64_t, std::uint64_t, std::uint64_t newer) { return newer; }
    static std::uint64_t erase(std::uint64_t) { return 0; }
    static bool present(std::uint64_t, state_type state) { return state.value != 0; }
    static std::uint64_t hash_key(std::uint64_t key) { return key; }
    static std::uint64_t hash_value(std::uint64_t, state_type state) { return state.value; }
  };
  void transformed_replacement() {
    using core = typed_engine<storage_policy<tip<transformed_sort>>>;
    temporary directory;
    connection<core> db(directory.root, "transformed", {.schema_id = "transaction/transformed-v1"});
    db.change(7, 4); // Stored state 8, encoded arrow 4: these are distinct types.
    auto tx = db.begin();
    tx.change(7, 6); auto twelve = tx.snapshot();
    tx.change(7, 4); assert(tx.snapshot().buffered_keys() == 0);
    tx.change(7, 5); auto ten = tx.snapshot();
    assert(twelve.get(7).value == 12 && ten.get(7).value == 10);
    auto done = tx.commit();
    assert(done.get(7).value == 10 && done.signature() == 70);
  }

  struct restore_fault : typed_engine<> {
    using base = typed_engine<>;
    using base::base;
    inline static std::atomic<bool> armed = false, fail_private = false;
    world_type contribute(contribution_type input) {
      auto result = base::contribute(std::move(input));
      if (fail_private.exchange(false)) throw std::runtime_error("private execution failed");
      return result;
    }
    static restore_fault from_snapshot(world_type world) { return restore_fault(base::from_snapshot(std::move(world))); }
    static world_type restore_checkpoint(runtime_family::snapshot_type runtime,
        std::span<std::byte const> bytes, std::string_view schema) {
      if (armed.load()) throw std::runtime_error("committed transaction restore failed");
      return world_type::restore(std::move(runtime), metadata_type::decode(bytes), schema);
    }
  private:
    explicit restore_fault(base value) : base(std::move(value)) {}
  };
  void private_failure() {
    temporary directory;
    connection<restore_fault> db(directory.root, "private-failure");
    auto base = db.snapshot();
    auto tx = db.begin(); tx.put("private", "value"); auto saved = tx.snapshot();
    restore_fault::fail_private = true;
    rejects([&] { (void)tx.flush(); });
    assert(tx.failed() && saved.get("private") == "value" && !db.failure());
    assert(head(db) == base.head() && !db.get("private"));
    rejects([&] { (void)tx.commit(); });
    tx.abort();
    db.put("healthy", "yes");
  }
  void committed_failure() {
    temporary directory;
    connection<restore_fault> db(directory.root, "failure");
    auto old = db.snapshot();
    auto tx = db.begin(); tx.put("committed", "value"); (void)tx.flush();
    restore_fault::armed = true;
    auto ticket = tx.commit_async();
    ticket.wait(); assert(ticket.ready());
    rejects([&] { (void)ticket.get(); });
    db.shutdown(); restore_fault::armed = false;
    assert(db.failure() && head(db) == old.head() && !db.get("committed"));
    auto reopened = connect<typed_engine<>>(directory.root, "failure", {.create_if_missing = false});
    assert(reopened.get("committed") == "value");
    assert(head(reopened).timeline.generation == old.head().timeline.generation + 1);
  }
}

int main() try {
  private_flushes<active_engine<>>(); private_flushes<typed_engine<>>();
  branches_and_abort(); conflicts_and_lifetime(); equivalent_revision(); arbitrary_arrows(); cancellation_and_backpressure(); transformed_replacement(); private_failure(); committed_failure();
  std::cout << "transactions: private flush, persistent nursery, branching, conflicts and acknowledgment passed\n";
} catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
