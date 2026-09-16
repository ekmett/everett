/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks durable replacement generations, gated recovery and active saves and forks.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/replacement_rebuild.h>
#include <diet/connection.h>
#include <iostream>
#include <csignal>
#include <map>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using namespace diet;
  using core = replacement_rebuild_engine<>;
  using engine = persistent_engine<core>;
  using store = engine::store_type;
  using strings = unsorted<std::optional<std::string>>;

  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-rebuild-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  };

  void check(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }

  template <class F> void rejects(F && action) {
    bool caught = false;
    try {
      action();
    } catch (std::exception const &) {
      caught = true;
    }
    check(caught, "expected rejection");
  }

  template <class Cola> void verify(Cola const & state,
      std::map<std::string, std::string> const & expected) {
    check(state.live_count() == expected.size(), "live count");
    std::uint64_t signature = 0;
    for (auto const & [k, v] : expected) {
      check(state.get(k) == v, "mapped value");
      signature += sort_semantics<strings>::hash_key(k) * sort_semantics<strings>::hash_value(k, v);
    }
    check(state.signature() == signature, "independent signature");
    auto const & m = state.metadata();
    check(m.clean_base + m.mutations == state.runtime().admissions(), "durable generation mass");
    for (auto const & run : state.runtime().runs())
      check(run->native->mapped() && !run->native->owned(), "published native is not mapped");
  }

  void drain(engine & e) {
    unsigned steps = 0;
    while (e.pending()) {
      e.advance(1000000);
      check(++steps < 10000, "durable recovery stalled");
    }
    check(e.admission_ready(), "settled engine refuses admission");
  }

  void lifecycle() {
    temporary dir;
    std::map<std::string, std::string> expected;
    core seed;
    auto batch = core::batch();
    for (unsigned i = 0; i != 128; ++i) {
      auto k = "key/" + std::to_string(i);
      batch.put(k, "initial");
      expected[k] = "initial";
    }
    seed.contribute(std::move(batch).finish());
    while (seed.pending()) seed.advance(1000000);
    seed.contribute(core::put("key/0", "dirty"));
    expected["key/0"] = "dirty";
    auto dirty = seed.snapshot();
    check(!dirty.metadata().rebuilding && dirty.metadata().mutations, "dirty inactive fixture");
    {
      auto storage = store::create(dir.root);
      storage.create_tap("latest", dirty.runtime(), dirty.metadata().encode());
    }
    std::optional<engine> active(engine::connect(dir.root, "latest", {.create_if_missing = false}));
    verify(active->snapshot(), expected);
    check(active->snapshot().metadata() == dirty.metadata(), "reopen reset dirty b/u");
    while (!active->snapshot().metadata().rebuilding) {
      while (!active->admission_ready()) active->advance(1000000);
      active->contribute(core::put("key/0", "at-freeze"));
      expected["key/0"] = "at-freeze";
    }
    // These mutations live in the private FIFO as well as the foreground.
    // Losing that FIFO must still recover the latest acknowledged state.
    active->contribute(core::put("key/0", "queued-after-freeze"));
    expected["key/0"] = "queued-after-freeze";
    active->contribute(core::erase("key/127"));
    expected.erase("key/127");
    auto frozen = active->snapshot();
    verify(frozen, expected);
    auto marker = frozen.metadata();
    check(marker.rebuilding, "active marker missing");
    {
      auto storage = store::open(dir.root);
      storage.save("active-save", frozen.head());
      auto branch = storage.fork("active-fork", frozen.head());
      check(branch.semantic == marker.encode(), "fork changed generation metadata");
    }
    // Discard all private scan/candidate state. The acknowledged marker is
    // sufficient to restart a full cleanup from the latest logical state.
    active.reset();
    active.emplace(engine::connect(dir.root, "latest", {.create_if_missing = false}));
    check(active->pending() && !active->admission_ready(), "active reconnect did not gate admissions");
    verify(active->snapshot(), expected);
    rejects([&] { active->contribute(core::put("unpaid", "must wait")); });
    check(!active->failed() && active->snapshot().metadata() == marker, "refused admission changed recovery");
    active->advance(1000000);
    check(active->pending() && !active->admission_ready(), "partial recovery completed too early");
    auto partial = active->snapshot();
    check(partial.metadata() == marker, "partial recovery erased generation");
    active.reset();
    // A process interrupted during the next recovery also leaves a complete
    // acknowledged foreground and marker. No live SQLite connection crosses
    // fork; this child never acknowledges a new contribution.
    auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork");
    if (!child) {
      try {
        auto e = engine::connect(dir.root, "latest", {.create_if_missing = false});
        e.advance(1000000);
        if (!e.pending() || e.admission_ready()) ::_exit(2);
        ::kill(::getpid(), SIGKILL);
        ::_exit(3);
      } catch (...) {
        ::_exit(4);
      }
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
      "interruption child failed");
    active.emplace(engine::connect(dir.root, "latest", {.create_if_missing = false}));
    check(!active->admission_ready() && active->snapshot().metadata() == marker,
      "interrupted recovery lost gate/counters");
    drain(*active);
    auto cleaned = active->snapshot();
    verify(cleaned, expected);
    check(!cleaned.metadata().rebuilding && cleaned.metadata().clean_base == expected.size() &&
      cleaned.metadata().mutations == 0, "clean handoff metadata");
    check(cleaned.head().timeline.generation > frozen.head().timeline.generation,
      "recovery completion was not published");
    active->contribute(core::template change<strings>("key/0", "after-recovery"));
    expected["key/0"] = "after-recovery";
    verify(active->snapshot(), expected);
    check(active->snapshot().metadata().mutations == 1, "new generation mutation missing");
    active.reset();
    auto reopened = engine::connect(dir.root, "latest", {.create_if_missing = false});
    verify(reopened.snapshot(), expected);
    check(reopened.snapshot().metadata().mutations == 1, "settled reconnect reset counter");
    // Ordinary connection methods retain the active snapshot exactly, while
    // their worker pays recovery before claiming a queued contribution.
    connection<core> observer(dir.root, "observer");
    auto saved = observer.load("active-save");
    check(saved && saved->metadata() == marker, "saved marker not preserved");
    auto old_expected = expected;
    old_expected["key/0"] = "queued-after-freeze";
    verify(*saved, old_expected);
    observer.save("active-save-again", *saved);
    auto copied = observer.load("active-save-again");
    check(copied && copied->metadata() == marker, "save copied active state incorrectly");
    auto forked = observer.fork("connection-fork", *saved);
    auto ticket = forked.template put_async<strings>("branch", "new");
    auto result = ticket.get()->cola;
    check(result.get("branch") == "new" && !result.metadata().rebuilding && !result.metadata().mutations &&
      result.metadata().clean_base == old_expected.size() + 1,
      "worker claimed admission before cleanup");
    check(!saved->get("branch"), "fork changed saved state");
    forked.shutdown();
    observer.shutdown();
    auto branch = engine::connect(dir.root, "active-fork", {.create_if_missing = false});
    check(branch.snapshot().metadata() == marker && !branch.admission_ready(), "saved branch lost recovery gate");
    drain(branch);
    verify(branch.snapshot(), old_expected);
  }

  void malformed() {
    temporary dir;
    core seed;
    seed.contribute(core::put("key", "value"));
    auto value = seed.snapshot();
    auto storage = store::create(dir.root);
    auto metadata = value.metadata();
    metadata.mutations = 7;
    storage.create_tap("malformed", value.runtime(), metadata.encode());
    rejects([&] { (void)engine::connect(dir.root, "malformed", {.create_if_missing = false}); });
    storage.create_tap("plain", value.runtime(), static_cast<core::typed_cola_type const &>(value).metadata().encode());
    rejects([&] { (void)engine::connect(dir.root, "plain", {.create_if_missing = false}); });
  }
}

int main() {
  lifecycle();
  malformed();
  std::cout << "durable replacement recovery passed\n";
}
