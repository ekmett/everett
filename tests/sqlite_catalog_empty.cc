/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks empty contribution work, durable identity and pending continuation preservation.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>

#include <cassert>
#include <memory>

namespace {
  using namespace everett;
  using binary = typed_engine<>;
  using rebuilt = replacement_rebuild_engine<>;
  using redundant = rebuilt::engine_type;
  template <class E> auto empty() { return std::move(E::batch()).finish(); }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    assert(rejected);
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-empty-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  std::uint64_t operations(std::filesystem::path const & root) {
    sqlite3 * raw = nullptr;
    if (sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
      throw std::runtime_error("open operation census");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt * query = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT count(*) FROM operations", -1, &query, nullptr) != SQLITE_OK)
      throw std::runtime_error("prepare operation census");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(query, sqlite3_finalize);
    if (sqlite3_step(query) != SQLITE_ROW) throw std::runtime_error("read operation census");
    return std::uint64_t(sqlite3_column_int64(query, 0));
  }
  template <class E> auto charged(E const & value) {
    if constexpr (requires { value.work().charged; }) return value.work().charged;
    else return value.work().foreground_charged + value.work().committed;
  }
  template <class E> auto granted(E const & value) {
    if constexpr (requires { value.work().charged; }) return value.work().granted;
    else return value.work().foreground_charged + value.work().granted;
  }
  template <class E> void unchanged(E & value) {
    auto old = value.snapshot();
    auto charge = charged(value), grant = granted(value);
    auto input = empty<E>();
    assert(E::reservation(input) == session_reservation{});
    auto result = value.contribute(std::move(input));
    assert(result.runtime().same_layout(old.runtime()) && result.metadata() == old.metadata());
    assert(charged(value) == charge && granted(value) == grant && !value.failed());
  }
  template <class E> void direct() {
    E value;
    unchanged(value);
    E foreign("other-schema");
    auto before = value.snapshot();
    auto rejected = foreign.snapshot().batch();
    rejects([&] { value.contribute(std::move(rejected).finish()); });
    auto still = value.snapshot();
    assert(!value.failed() && still.runtime().same_layout(before.runtime()));
    value.contribute(E::put("key", "value"));
    unchanged(value);
  }

  void pending_binary() {
    binary value, control;
    for (auto * engine : {&value, &control}) {
      engine->contribute(binary::put("a", "one"));
      engine->contribute(binary::put("b", "two"));
    }
    assert(value.pending() && control.pending());
    unsigned steps = 0;
    while (value.pending()) {
      value.advance(1); control.advance(1);
      unchanged(value);
      assert(value.pending() == control.pending() && value.work().charged == control.work().charged &&
        value.work().granted == control.work().granted);
      assert(++steps < 1000);
    }
    assert(value.snapshot().get("a") == "one" && value.snapshot().get("b") == "two");
  }

  void recovering_rebuild() {
    redundant source;
    auto batch = redundant::batch(); batch.put("a", "one").put("b", "two");
    auto input = std::move(batch).finish();
    auto semantic = source.contribute(input);
    redundant::runtime_type runtime;
    for (auto const & record : input.records()) assert(runtime.try_contribute(record, 0));
    auto state = redundant::world_type::restore(runtime.checkpoint(), semantic.metadata(), semantic.metadata().schema_id);
    auto recovering = rebuilt::from_clean(state);
    assert(recovering.pending() && !recovering.admission_ready());
    auto before = recovering.snapshot();
    auto work = charged(recovering);
    rejects([&] { recovering.contribute(empty<rebuilt>()); });
    auto after = recovering.snapshot();
    assert(!recovering.failed() && charged(recovering) == work && after.runtime().same_layout(before.runtime()));
    while (recovering.pending()) recovering.advance(1'000'000);
    unchanged(recovering);
    assert(recovering.snapshot().get("a") == "one" && recovering.snapshot().get("b") == "two");
  }

  template <class Core> void durable() {
    using E = persistent_engine<Core>;
    temporary dir;
    auto value = E::connect(dir.root, "empty");
    for (unsigned phase = 0; phase != 2; ++phase) {
      auto before = value.snapshot();
      auto count = operations(dir.root);
      auto result = value.contribute(empty<Core>());
      assert(result.head() == before.head() && result.runtime().same_layout(before.runtime()) &&
        result.metadata() == before.metadata() && operations(dir.root) == count);
      Core foreign("foreign-schema");
      auto wrong = foreign.snapshot().batch();
      rejects([&] { value.contribute(std::move(wrong).finish()); });
      auto retained = value.snapshot();
      assert(!value.failed() && retained.head() == before.head() && operations(dir.root) == count);
      auto reopened = E::connect(dir.root, "empty", {.create_if_missing = false});
      auto opened = reopened.snapshot();
      assert(opened.head() == before.head() && opened.metadata() == before.metadata());
      if (!phase) value.contribute(Core::put("key", "value"));
    }
    auto before = value.snapshot();
    auto count = operations(dir.root);
    auto result = value.contribute(Core::put("key", "value"));
    assert(result.signature() == before.signature() && result.head() != before.head() && operations(dir.root) > count);
  }

  void durable_pending() {
    using E = persistent_engine<binary>;
    temporary dir;
    auto value = E::connect(dir.root, "pending");
    value.contribute(binary::put("a", "one"));
    value.contribute(binary::put("b", "two"));
    assert(value.pending());
    assert(!value.advance(1));
    auto old = value.snapshot();
    auto count = operations(dir.root);
    for (unsigned i = 0; i != 3; ++i) {
      auto result = value.contribute(empty<binary>());
      assert(result.head() == old.head() && result.runtime().same_layout(old.runtime()));
      assert(value.pending() && operations(dir.root) == count);
    }
    while (value.pending()) value.advance(16384);
    auto settled = value.snapshot();
    assert(settled.head().timeline.generation == old.head().timeline.generation + 1);
    assert(settled.get("a") == "one" && settled.get("b") == "two");
  }

  struct changing_core : binary {
    using binary::binary;
    changing_core() = default;
    static changing_core from_snapshot(world_type state) { return changing_core(binary::from_snapshot(std::move(state))); }
    world_type contribute(contribution_type input) {
      if (input.records().empty()) return binary::contribute(binary::put("custom-empty", "changed"));
      return binary::contribute(std::move(input));
    }
    static session_reservation reservation(contribution_type const & input) {
      return input.records().empty() ? binary::reservation(binary::put("custom-empty", "changed")) : binary::reservation(input);
    }
  private:
    explicit changing_core(binary value) : binary(std::move(value)) {}
  };
  void custom_change() {
    using E = persistent_engine<changing_core>;
    temporary dir;
    auto value = E::connect(dir.root, "custom");
    auto old = value.snapshot();
    auto count = operations(dir.root);
    auto result = value.contribute(empty<changing_core>());
    assert(result.get("custom-empty") == "changed" && result.head() != old.head() && operations(dir.root) > count);
    auto reopened = E::connect(dir.root, "custom", {.create_if_missing = false});
    assert(reopened.snapshot().get("custom-empty") == "changed");
  }

  void stale_and_local_tickets() {
    using E = persistent_engine<binary>;
    temporary dir;
    auto stale = E::connect(dir.root, "stale");
    auto old = stale.snapshot();
    auto writer = E::connect(dir.root, "stale");
    auto latest = writer.contribute(binary::put("new", "value"));
    auto count = operations(dir.root);
    auto unchanged = stale.contribute(empty<binary>());
    assert(unchanged.head() == old.head() && unchanged.head() != latest.head() && !unchanged.get("new"));
    assert(!stale.failed() && operations(dir.root) == count);

    connection<binary> live(dir.root, "tickets", {.limits = session_limits{0, 0, 2, 1}});
    auto before = live.publication();
    count = operations(dir.root);
    auto receipt = live.submit(empty<binary>());
    auto result = receipt.get();
    assert(result->world.head() == before->world.head() && result->generation == before->generation + 1 &&
      result->revision == before->revision + 1 && operations(dir.root) == count);
    live.shutdown();
    assert(!live.failure());
  }
}
int main() {
  direct<binary>(); direct<redundant>(); direct<rebuilt>();
  pending_binary(); recovering_rebuild();
  durable<binary>(); durable<active_engine<>>();
  durable_pending(); custom_change(); stale_and_local_tickets();
}
