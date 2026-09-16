/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks durable ordered native-merge reuse without sharing fractional indexes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>
#include <everett/sort_runtime_context.h>
#include <everett/typed_scan.h>

#include <barrier>
#include <fstream>
#include <thread>
#include <iostream>
#include <map>
#include <set>

namespace {
  using namespace everett;
  using P = string_policy;
  using strings = unsorted<std::optional<std::string>>;
  using family = streaming_sort_runtime_family<P>;
  using native = family::native_type;
  using storage = family::storage_type;
  using runtime = family::runtime_type<replace_native_value>;
  using store = runtime_store<P, random_object_ids, sqlite_catalog_ops, family>;
  using typed = typed_world<P, wrapping_fingerprint_algebra, family>;
  using core = typed_engine<P, wrapping_fingerprint_algebra, 256, family>;
  std::string const schema = "native-reuse-fixture/1";

  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class F> void rejects(F && fn) {
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected rejection");
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-native-reuse-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  std::uint64_t scalar(std::filesystem::path const & root, char const * sql) {
    sqlite3 * db = nullptr;
    check(sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "SQL fixture open");
    struct cleanup { sqlite3 * db; ~cleanup() { sqlite3_close(db); } } close{db};
    sqlite3_stmt * statement = nullptr;
    check(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK, "SQL fixture prepare");
    struct finalizer { sqlite3_stmt * value; ~finalizer() { sqlite3_finalize(value); } } finalize{statement};
    check(sqlite3_step(statement) == SQLITE_ROW, "SQL fixture row");
    return std::uint64_t(sqlite3_column_int64(statement, 0));
  }
  bool extension(std::filesystem::path const & root) {
    return scalar(root, "SELECT count(*) FROM sqlite_schema WHERE name='completed_native_merges'") != 0;
  }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t n = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++n;
    return n;
  }
  auto input(std::string value, std::string key = "key") {
    sort_profile_writer<P> out; out.append<strings>(key, std::optional<std::string>(std::move(value)));
    return native::from_owned(out.finish());
  }
  template <class Context, class Older, class Newer, class Compose = replace_native_value>
  auto merge(Context const & context, Older older, Newer newer, Compose compose = {}) {
    auto job = context->make_merge(std::move(older), std::move(newer), std::move(compose));
    while (!job->done()) job->step(32);
    return context->finish_merge(*job);
  }
  struct prepared {
    temporary dir;
    store saved = store::create(dir.root);
    sqlite_catalog<P> catalog = sqlite_catalog<P>::open(dir.root);
    random_object_ids ids;
    runtime_store_detail::graph_sealer<P, random_object_ids, sqlite_catalog_ops, family> sealer{catalog, ids};
    native::pointer older = input(std::string(96 * 1024, 'a'));
    native::pointer newer = input(std::string(100 * 1024, 'b'));
    object_id old_id = sealer.ensure_native(older)->receipt.object;
    object_id new_id = sealer.ensure_native(newer)->receipt.object;
    catalog_native_merge key{schema, old_id, new_id};
    auto context(std::string const & domain = schema) {
      return storage::context_type::open(dir.root, {}, {}, {}, {}, {}, domain);
    }
  };
  struct other_compose {
    unsigned * calls;
    bit_view operator()(bit_view, bit_view, bit_view newer) const { ++*calls; return newer; }
  };

  void ordered_domains_and_replay() {
    prepared f; auto ctx = f.context();
    check(!extension(f.dir.root), "catalog open installed advisory table");
    check(!ctx->reuse_merge<replace_native_value>(f.older, f.newer), "absent hint hit");
    check(!extension(f.dir.root), "cache miss installed advisory table");
    auto result = merge(ctx, f.older, f.newer);
    check(result->sealed() && ctx->sealed_outputs() == 1 && extension(f.dir.root), "streamed completion omitted hint");
    auto output = result->sealed()->receipt;
    check(scalar(f.dir.root, "SELECT count(*) FROM completed_native_merges") == 1, "duplicate hint");
    auto hit = f.context()->reuse_merge<replace_native_value>(f.older, f.newer);
    check(hit && hit->sealed()->receipt.object == output.object, "reopened context did not reuse exact native");
    check(!ctx->reuse_merge<replace_native_value>(f.newer, f.older), "ordered inputs were canonicalized");
    check(!f.context("native-reuse-fixture/2")->reuse_merge<replace_native_value>(f.older, f.newer), "schema identity ignored");
    check(!storage::open(f.dir.root).context()->reuse_merge<replace_native_value>(f.older, f.newer), "empty schema enabled reuse");
    unsigned calls = 0;
    check(!ctx->reuse_merge<other_compose>(f.older, f.newer), "custom composition was authenticated by its type");
    auto custom = merge(ctx, f.older, f.newer, other_compose{&calls});
    check(calls == 1 && custom->sealed() && scalar(f.dir.root, "SELECT count(*) FROM completed_native_merges") == 1,
      "custom composition installed a replacement hint");

    auto before = scalar(f.dir.root, "SELECT count(*) FROM owner_objects");
    auto pin = f.catalog.acquire_native_merge("acquire-fixed", f.key, "reader-fixed");
    auto replay = f.catalog.acquire_native_merge("acquire-fixed", f.key, "reader-fixed");
    check(pin && replay && pin->object == output.object && replay->object == output.object &&
      scalar(f.dir.root, "SELECT count(*) FROM owner_objects") == before + 1, "acquisition replay duplicated or lost its pin");
    rejects([&] { f.catalog.acquire_native_merge("acquire-fixed", {schema, f.new_id, f.old_id}, "reader-fixed"); });
    check(!f.catalog.poisoned(), "exact replay rejection poisoned a healthy catalog");
    f.catalog.record_native_merge("record-fixed", f.key, output, "unused-winner-pin");
    f.catalog.record_native_merge("record-fixed", f.key, output, "unused-winner-pin");
    rejects([&] { f.catalog.record_native_merge("record-fixed", {"another", f.old_id, f.new_id}, output, "unused-winner-pin"); });
    // A second valid producer can finish after the first: first acknowledgment
    // wins without poisoning the loser or adding another permanent cache pin.
    auto duplicate = merge(f.context(), f.older, f.newer);
    check(duplicate->sealed()->receipt.object != output.object, "fixture did not make a competing output");
    auto winner = ctx->reuse_merge<replace_native_value>(f.older, f.newer);
    check(winner->sealed()->receipt.object == output.object && !ctx->failed(), "competing completion displaced the first result");
    check(scalar(f.dir.root, "SELECT count(*) FROM completed_native_merges m JOIN owner_objects p ON p.owner_kind=m.owner_kind AND p.owner_id=m.owner_id AND p.object_id=m.output") == 1,
      "hint did not own its output");
    auto reopened = sqlite_catalog<P>::open(f.dir.root);
    check(reopened.schema_version() == 4, "advisory extension changed core version");
  }

  void adaptive_and_copied_catalog() {
    prepared f; auto ctx = f.context();
    auto small_a = input("a"), small_b = input("b");
    auto before = scalar(f.dir.root, "SELECT count(*) FROM operations"); auto count = files(f.dir.root);
    check(!ctx->reuse_merge<replace_native_value>(small_a, small_b), "unbound native hit");
    auto small = merge(ctx, small_a, small_b);
    check(small->owned() && !small->sealed() && !extension(f.dir.root) && files(f.dir.root) == count &&
      scalar(f.dir.root, "SELECT count(*) FROM operations") == before, "small output or lookup acquired durable identity");
    f.sealer.ensure_native(small_a); f.sealer.ensure_native(small_b);
    small = merge(ctx, small_a, small_b);
    check(small->owned() && !extension(f.dir.root), "naturally owned output installed a durable hint");
    auto large = merge(ctx, f.older, f.newer); (void)large;
    temporary copy;
    // The catalog UUID survives a backup. Owner authority does not cross into
    // the backup's distinct canonical file namespace.
    std::filesystem::copy(f.dir.root, copy.root,
      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    auto clone = storage::context_type::open(copy.root, {}, {}, {}, {}, {}, schema);
    auto operations = scalar(copy.root, "SELECT count(*) FROM operations");
    check(!clone->reuse_merge<replace_native_value>(f.older, f.newer) &&
      scalar(copy.root, "SELECT count(*) FROM operations") == operations, "copied catalog accepted source owner authority");
  }

  void flip(std::filesystem::path const & path) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    {
      std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
      file.seekg(64); char byte; file.read(&byte, 1); check(bool(file), "read corruption fixture");
      byte ^= 1; file.seekp(64); file.write(&byte, 1); file.flush(); check(bool(file), "write corruption fixture");
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);
  }
  void sql(std::filesystem::path const & root, char const * command) {
    sqlite3 * db = nullptr;
    check(sqlite3_open_v2((root / "catalog.sqlite3").c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "SQL edit open");
    struct cleanup { sqlite3 * db; ~cleanup() { sqlite3_close(db); } } close{db};
    check(sqlite3_exec(db, command, nullptr, nullptr, nullptr) == SQLITE_OK, "SQL fixture edit");
  }
  void invalid_evidence() {
    prepared f;
    auto result = merge(storage::open(f.dir.root).context(), f.older, f.newer);
    auto receipt = result->sealed()->receipt;
    check(!extension(f.dir.root), "schema-free merge installed a hint");
    auto counterfeit = receipt; counterfeit.attempt = object_attempt_id(f.ids().hex());
    rejects([&] { f.catalog.record_native_merge("counterfeit", f.key, counterfeit, "bad-pin"); });
    check(!extension(f.dir.root), "failed receipt installed extension");
    auto id = f.ids(); object_attempt_id attempt(f.ids().hex());
    std::array<catalog_object_reservation, 1> reservation{{{id, file_kind::native_blob}}};
    f.catalog.reserve("unfinished", attempt, "unfinished-owner", {}, reservation);
    auto sealed_file = encoded_sort_sections<P>::from(*f.older->owned()).seal(f.dir.root, id, attempt);
    rejects([&] { f.catalog.record_native_merge("unacknowledged-output", f.key, sealed_file, "bad-pin"); });
    rejects([&] { f.catalog.record_native_merge("unacknowledged-input", {schema, id, f.new_id}, receipt, "bad-pin"); });
    check(!extension(f.dir.root), "unacknowledged bytes installed a hint");
    f.catalog.record_native_merge("valid-output", f.key, receipt, "cache-pin");
    auto before = scalar(f.dir.root, "SELECT count(*) FROM owner_objects");
    flip(receipt.path);
    rejects([&] { f.catalog.acquire_native_merge("corrupt-acquire", f.key, "corrupt-pin"); });
    rejects([&] { f.catalog.record_native_merge("valid-output", f.key, receipt, "cache-pin"); });
    auto ctx = f.context();
    rejects([&] { (void)ctx->reuse_merge<replace_native_value>(f.older, f.newer); });
    check(ctx->failed() && scalar(f.dir.root, "SELECT count(*) FROM owner_objects") == before,
      "corrupt output acquired authority before envelope validation");
    flip(receipt.path);
    auto healthy = f.context();
    rejects([&] { (void)healthy->reuse_merge<replace_native_value>({}, f.newer); });
    check(healthy->failed(), "null cache input did not fail safely");
    sql(f.dir.root, "CREATE TRIGGER extra_native_merge AFTER INSERT ON completed_native_merges BEGIN SELECT 1; END");
    rejects([&] { (void)sqlite_catalog<P>::open(f.dir.root); });
    temporary malformed; auto empty_catalog = store::create(malformed.root); (void)empty_catalog;
    sql(malformed.root, "CREATE TABLE completed_native_merges(wrong TEXT) STRICT");
    rejects([&] { (void)sqlite_catalog<P>::open(malformed.root); });
  }
  void simultaneous_completions() {
    prepared f;
    auto open = [&] {
      return storage::context_type::open(f.dir.root, {}, {.busy_timeout_ms = 5'000}, {}, {}, {}, schema);
    };
    auto a = open(), b = open();
    auto job_a = a->make_merge(f.older, f.newer, replace_native_value{});
    auto job_b = b->make_merge(f.older, f.newer, replace_native_value{});
    while (!job_a->done()) job_a->step(32);
    while (!job_b->done()) job_b->step(32);
    std::barrier start(2);
    native::pointer output_a, output_b; std::exception_ptr error_a, error_b;
    auto worker = [&](auto const & context, auto & job, auto & output, auto & error) {
      start.arrive_and_wait();
      try { output = context->finish_merge(*job); } catch (...) { error = std::current_exception(); }
    };
    std::thread left([&] { worker(a, job_a, output_a, error_a); });
    std::thread right([&] { worker(b, job_b, output_b, error_b); });
    left.join(); right.join();
    if (error_a) std::rethrow_exception(error_a);
    if (error_b) std::rethrow_exception(error_b);
    check(output_a && output_b && !a->failed() && !b->failed(), "competing valid completion was poisoned");
    auto winner = f.context()->reuse_merge<replace_native_value>(f.older, f.newer);
    auto id = winner->sealed()->receipt.object;
    check((id == output_a->sealed()->receipt.object || id == output_b->sealed()->receipt.object) &&
      scalar(f.dir.root, "SELECT count(*) FROM completed_native_merges") == 1, "concurrent completion lost unique winner");
  }

  using oracle = std::map<std::string, std::string>;
  typed restore(runtime::snapshot_type state, oracle const & expected) {
    typed_world_metadata<> metadata;
    metadata.schema_id = schema; metadata.live_count = expected.size();
    for (auto const & [key, value] : expected)
      metadata.signature += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, value);
    return typed::restore(std::move(state), std::move(metadata), schema);
  }
  void verify(typed const & state, oracle const & expected) {
    auto reference = restore(state.runtime(), expected);
    check(state.signature() == reference.signature() && state.live_count() == expected.size(), "reuse changed semantic metadata");
    for (auto const & [key, value] : expected) check(state.get(key) == value, "reuse changed lookup");
    auto scan = everett::scan(state); auto at = expected.begin();
    while (auto row = scan.next()) {
      check(at != expected.end() && row->key == at->first && row->value == at->second, "reuse changed scan"); ++at;
    }
    check(at == expected.end(), "reuse scan truncated");
  }
  void fork_and_checkpoint() {
    temporary dir; auto saved = store::create(dir.root);
    auto disk = storage::open_for_schema(dir.root, schema);
    runtime active(disk);
    oracle expected{{"key", std::string(100 * 1024, 'b')}};
    auto a = core::put("key", std::string(96 * 1024, 'a'));
    auto b = core::put("key", expected.begin()->second);
    check(bool(active.try_contribute(a.records().front(), 0)), "first admission");
    auto old = restore(active.snapshot(), {{"key", std::string(96 * 1024, 'a')}});
    auto first = saved.create_session("first", active.checkpoint(), old.metadata().encode());
    check(bool(active.try_contribute(b.records().front(), 0)), "second admission");
    auto state = restore(active.checkpoint(), expected);
    auto shared = saved.publish(first.head, state.runtime(), state.metadata().encode());
    auto branch = saved.fork("second", shared.head);
    auto runtime_a = runtime::from_snapshot(shared.snapshot, storage::open_for_schema(dir.root, schema));
    while (runtime_a.pending()) runtime_a.advance(1'000'000);
    auto ready_a = typed::restore(runtime_a.snapshot(), state.metadata(), schema); verify(ready_a, expected);
    auto published_a = saved.publish(shared.head, ready_a.runtime(), ready_a.metadata().encode());
    check(runtime_a.work().native_inputs == 2 && runtime_a.work().native_reuses == 0, "first fork did not perform its merge");
    check(published_a.snapshot.runs().size() == 1, "first fork did not settle one native run");
    auto merged_pair = published_a.snapshot.runs().front()->pair->mapped()->identity();
    auto merged_id = merged_pair.native;
    // Reopen the second branch rather than sharing in-memory facade addresses.
    auto other_store = store::open(dir.root); auto reopened = other_store.find("second");
    check(reopened && reopened->head == branch.head, "fork reopen lost exact checkpoint");
    auto runtime_b = runtime::from_snapshot(reopened->snapshot, storage::open_for_schema(dir.root, schema));
    bool captured = false;
    while (runtime_b.pending()) {
      auto price = runtime_b.next_service_cost(), credit = runtime_b.credit();
      runtime_b.advance(price > credit ? price - credit : 1);
      auto frontier = runtime_b.checkpoint();
      for (auto const & level : frontier.frontier().levels)
        if (level.job && level.job->merged && level.job->stage == redundant_stage::destination_index) {
          auto native = level.job->merged;
          check(native->sealed() && native->sealed()->receipt.object == merged_id, "hidden reuse chose another native");
          auto hidden = typed::restore(frontier, state.metadata(), schema); verify(hidden, expected);
          auto pinned = other_store.publish(reopened->head, frontier, hidden.metadata().encode());
          check(std::find(pinned.head.auxiliary.natives.begin(), pinned.head.auxiliary.natives.end(), merged_id) != pinned.head.auxiliary.natives.end(),
            "hidden reused native missing durable pin");
          auto retry = store::open(dir.root).find("second"); check(bool(retry), "hidden checkpoint reopen");
          auto resumed = runtime::from_snapshot(retry->snapshot, storage::open_for_schema(dir.root, schema));
          while (resumed.pending()) resumed.advance(1'000'000);
          auto final = typed::restore(resumed.snapshot(), state.metadata(), schema); verify(final, expected);
          auto published_b = other_store.publish(pinned.head, final.runtime(), final.metadata().encode());
          check(published_b.snapshot.runs().size() == 1, "second fork did not settle one native run");
          auto final_pair = published_b.snapshot.runs().front()->pair->mapped()->identity();
          check(final_pair.native == merged_id && final_pair.index != merged_pair.index,
            "fork reused another fork's fractional index");
          check(runtime_b.work().native_inputs == 0 && runtime_b.work().native_reuses == 1 &&
            runtime_b.work().native_work < runtime_a.work().native_work, "reused merge fabricated native work");
          auto durable = persistent_engine<core>::connect(dir.root, "second", {.schema_id = schema, .create_if_missing = false});
          verify(durable.snapshot(), expected);
          auto changed = durable.contribute(core::put("key", "after-reuse"));
          verify(changed, {{"key", "after-reuse"}});
          verify(ready_a, expected);
          captured = true; break;
        }
      if (captured) break;
    }
    check(captured, "reuse fixture missed hidden completed native stage");
    verify(old, {{"key", std::string(96 * 1024, 'a')}});
  }

  void rebuilding_reuse_restart() {
    using rebuild = replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, family>;
    using rebuilt = rebuild::world_type;
    using durable = persistent_engine<rebuild>;
    auto settle = [](auto & active) {
      unsigned rounds = 0;
      while (active.pending()) {
        active.advance(1'000'000);
        check(++rounds < 10'000, "combined reuse/rebuild did not settle");
      }
      check(active.admission_ready(), "settled rebuild retained admission debt");
    };
    auto key = [](unsigned n) { return "row-" + std::to_string(n); };
    temporary dir;
    auto saved = store::create(dir.root);
    auto disk = storage::open_for_schema(dir.root, schema);
    rebuild seed(schema);
    auto active = rebuild::from_snapshot(seed.snapshot(), disk);
    oracle original;
    auto initial = rebuild::batch();
    for (unsigned n = 0; n != 128; ++n) {
      original[key(n)] = "original-" + std::to_string(n);
      initial.put(key(n), original.at(key(n)));
    }
    active.contribute(std::move(initial).finish());
    settle(active);
    auto old = active.snapshot();
    verify(old, original);
    auto recorded = saved.create_session("original", old.runtime(), old.metadata().encode());
    saved.save("before-deletes", recorded.head);

    auto expected = original;
    for (unsigned n = 0; n != 32; ++n) {
      while (!active.admission_ready()) active.advance(1'000'000);
      active.contribute(rebuild::erase(key(n)));
      expected.erase(key(n));
    }
    auto frozen = active.snapshot();
    verify(frozen, expected);
    check(frozen.metadata().rebuilding && frozen.metadata().clean_base == 128 &&
      frozen.metadata().mutations == 32 && frozen.runtime().admissions() == 160,
      "checked deletions did not reach the large rebuild trigger");

    // Keep the real active generation, but expose a lower-level service cut.
    // Each overwrite first passes through the typed engine: its checked
    // metadata, exact encoded record and actual admission count are reused
    // below. Only service timing differs; no semantic hash is patched by hand.
    auto pending = runtime::from_snapshot(frozen.runtime(), disk);
    settle(pending);
    auto semantic = core::from_snapshot(frozen, disk);
    settle(semantic);
    std::uint64_t admitted = 0;
    for (unsigned n = 0; n != 2; ++n) {
      auto value = std::string((96 + 4 * n) * 1024, char('a' + n));
      auto batch = core::batch(); batch.put(key(32), value);
      auto command = std::move(batch).finish();
      check(command.records().size() == 1, "overwrite fixture unexpectedly coalesced");
      auto record = command.records().front();
      admitted += command.records().size();
      semantic.contribute(std::move(command));
      expected[key(32)] = std::move(value);
      check(bool(pending.try_contribute(record, 0)), "zero-service checked overwrite rejected");
    }
    auto semantic_state = semantic.snapshot();
    auto frontier = pending.checkpoint();
    check(frontier.admissions() == semantic_state.runtime().admissions() &&
      frontier.admissions() == frozen.runtime().admissions() + admitted && frontier.frontier().service_due,
      "checked overwrite lost admission mass or its service obligation");
    rebuilt shared(typed::restore(frontier, semantic_state.metadata(), schema),
      frozen.metadata().clean_base, frozen.metadata().mutations + admitted, true);
    verify(shared, expected);
    auto first = saved.create_session("cleanup-first", shared.runtime(), shared.metadata().encode());
    auto second = saved.fork("cleanup-second", first.head);
    auto finished = runtime::from_snapshot(first.snapshot, storage::open_for_schema(dir.root, schema));
    settle(finished);
    rebuilt first_state(typed::restore(finished.snapshot(), semantic_state.metadata(), schema),
      shared.metadata().clean_base, shared.metadata().mutations, true);
    auto winner = saved.publish(first.head, first_state.runtime(), first_state.metadata().encode());
    std::optional<blob_identity> winning_pair;
    for (auto const & run : winner.snapshot.runs())
      if (run->first == 160 && run->last == 162) winning_pair = run->pair->mapped()->identity();
    check(winning_pair && finished.work().native_reuses == 0 && finished.work().native_inputs == 2,
      "first active-generation fork did not create the expected native merge");

    auto reopened = store::open(dir.root).find("cleanup-second");
    check(reopened && reopened->head == second.head, "active fork lost exact checkpoint");
    auto borrowing = runtime::from_snapshot(reopened->snapshot, storage::open_for_schema(dir.root, schema));
    std::optional<catalog_session_head> hidden_head;
    std::uint64_t hidden_due = 0;
    while (borrowing.pending() && !hidden_head) {
      auto price = borrowing.next_service_cost(), credit = borrowing.credit();
      borrowing.advance(price > credit ? price - credit : 1);
      auto cut = borrowing.checkpoint();
      for (auto const & level : cut.frontier().levels) {
        if (!level.job || !level.job->merged || level.job->stage != redundant_stage::destination_index) continue;
        auto const & merged = level.job->merged;
        check(merged->sealed() && merged->sealed()->receipt.object == winning_pair->native,
          "active-generation reuse selected a different native");
        hidden_due = cut.frontier().service_due;
        check(hidden_due && !borrowing.admission_ready() && borrowing.work().native_reuses == 1 &&
          !borrowing.work().native_inputs && !borrowing.work().native_outputs,
          "native reuse forgave debt or invented scanned work");
        rebuilt hidden(typed::restore(cut, semantic_state.metadata(), schema),
          shared.metadata().clean_base, shared.metadata().mutations, true);
        verify(hidden, expected);
        auto held = saved.publish(second.head, hidden.runtime(), hidden.metadata().encode());
        check(std::find(held.head.auxiliary.natives.begin(), held.head.auxiliary.natives.end(), winning_pair->native) !=
          held.head.auxiliary.natives.end(), "active rebuild did not pin its hidden reused output");
        hidden_head = held.head;
        break;
      }
    }
    check(bool(hidden_head), "active rebuild missed hidden native reuse");

    std::optional<durable> recovering(durable::connect(dir.root, "cleanup-second",
      {.schema_id = schema, .create_if_missing = false}));
    auto before = recovering->snapshot();
    verify(before, expected);
    check(before.head() == *hidden_head && before.metadata() == shared.metadata() &&
      before.runtime().frontier().service_due == hidden_due && !recovering->admission_ready(),
      "reopen lost hidden-index debt or active rebuild accounting");
    rejects([&] { recovering->contribute(rebuild::put("blocked", "not admitted")); });
    auto rejected = recovering->snapshot();
    check(!recovering->failed() && rejected.head() == *hidden_head,
      "recovery admission refusal changed the acknowledged generation");
    // The first service call completes only the blocked foreground. Its
    // branch-local index must exist before the cleanup candidate can run.
    recovering->advance(1'000'000);
    auto indexed = recovering->snapshot();
    check(indexed.metadata() == shared.metadata() && !indexed.runtime().frontier().service_due &&
      recovering->pending() && !recovering->admission_ready(),
      "index completion prematurely cleared replacement cleanup debt");
    bool own_index = false;
    for (auto const & run : indexed.runtime().runs())
      if (run->first == 160 && run->last == 162) {
        auto identity = run->pair->mapped()->identity();
        own_index = identity.native == winning_pair->native && identity.index != winning_pair->index;
      }
    check(own_index, "recovered rebuild reused another fork's fractional index");
    recovering.reset();
    recovering.emplace(durable::connect(dir.root, "cleanup-second",
      {.schema_id = schema, .create_if_missing = false}));
    check(!recovering->admission_ready() && recovering->snapshot().metadata() == shared.metadata(),
      "second restart treated unfinished cleanup as a clean generation");
    settle(*recovering);
    auto cleaned = recovering->snapshot();
    verify(cleaned, expected);
    check(!cleaned.metadata().rebuilding && cleaned.metadata().clean_base == 96 &&
      !cleaned.metadata().mutations && cleaned.runtime().admissions() == 96 &&
      cleaned.head().timeline.generation > hidden_head->timeline.generation,
      "recovered cleanup did not shrink the admitted universe at durable handoff");
    for (unsigned n = 0; n != 32; ++n) check(!cleaned.get(key(n)), "cleanup resurrected a deleted key");
    auto old_save = store::open(dir.root).find_save("before-deletes");
    check(bool(old_save), "cleanup discarded the old saved snapshot");
    auto old_state = rebuilt::restore(old_save->snapshot, rebuild::metadata_type::decode(old_save->semantic), schema);
    verify(old_state, original); verify(old, original); verify(first_state, expected);
    check(old_state.get(key(32)) == "original-32" && cleaned.get(key(32)) == expected.at(key(32)),
      "old snapshot or overwritten survivor changed during handoff");
  }

  struct fault { std::string kind; bool armed = false, after = false; unsigned hits = 0; };
  std::shared_ptr<fault> injection;
  struct catalog_ops {
    std::shared_ptr<fault> state = injection;
    int commit(sqlite3 * db) noexcept {
      bool fail = false;
      if (state && state->armed) {
        sqlite3_stmt * row = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT kind FROM operations ORDER BY rowid DESC LIMIT 1", -1, &row, nullptr) != SQLITE_OK) return SQLITE_ERROR;
        if (sqlite3_step(row) == SQLITE_ROW) {
          auto kind = reinterpret_cast<char const *>(sqlite3_column_text(row, 0));
          fail = kind && state->kind == kind;
        }
        sqlite3_finalize(row);
        if (fail) { ++state->hits; state->armed = false; }
      }
      if (fail && !state->after) return SQLITE_IOERR_FSYNC;
      auto code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return fail && code == SQLITE_OK ? SQLITE_IOERR_FSYNC : code;
    }
  };
  using fault_context = sort_runtime_context<P, registry_selector<string_registry>, random_object_ids, catalog_ops>;
  void interrupted_hints() {
    for (auto kind : {"remember_native_merge", "acquire_native_merge"}) for (bool after : {false, true}) {
      prepared f;
      runtime empty;
      auto baseline = f.saved.create_session("stable", empty.snapshot());
      if (std::string_view(kind) == "acquire_native_merge") merge(f.context(), f.older, f.newer);
      injection = std::make_shared<fault>(fault{kind, true, after, 0});
      auto ctx = fault_context::open(f.dir.root, {}, {}, {}, {}, {}, schema);
      auto before = scalar(f.dir.root, "SELECT count(*) FROM owner_objects");
      rejects([&] {
        if (std::string_view(kind) == "remember_native_merge") (void)merge(ctx, f.older, f.newer);
        else (void)ctx->reuse_merge<replace_native_value>(f.older, f.newer);
      });
      check(injection->hits == 1 && ctx->failed() && !ctx->reused_outputs(), "uncertain hint/pin escaped context poisoning");
      auto held = scalar(f.dir.root, "SELECT count(*) FROM owner_objects");
      if (std::string_view(kind) == "acquire_native_merge") check(held == before + after, "acquisition COMMIT pin boundary");
      else check(extension(f.dir.root) == after, "hint extension COMMIT boundary");
      injection.reset();
      auto stable = store::open(f.dir.root).find("stable");
      check(stable && stable->head == baseline.head && !stable->snapshot.admissions(),
        "failed hint changed the last logical publication");
      auto fresh = f.context()->reuse_merge<replace_native_value>(f.older, f.newer);
      check(bool(fresh) == (after || std::string_view(kind) == "acquire_native_merge"), "reopen disagrees with acknowledged hint");
    }
  }

  void reconcile_interrupted_operation() {
    for (auto kind : {"remember_native_merge", "acquire_native_merge"}) for (bool after : {false, true}) {
      prepared f;
      // Seal the output without registering a recipe, so this fixture controls
      // the exact operation and pin IDs even if the acknowledgment is lost.
      auto output = merge(f.context(""), f.older, f.newer)->sealed()->receipt;
      bool acquire = std::string_view(kind) == "acquire_native_merge";
      if (acquire) f.catalog.record_native_merge("initial-hint", f.key, output, "initial-hint-pin");
      auto before = scalar(f.dir.root, "SELECT count(*) FROM owner_objects");
      injection = std::make_shared<fault>(fault{kind, true, after, 0});
      {
        auto interrupted = sqlite_catalog<P, catalog_ops>::open(f.dir.root);
        rejects([&] {
          if (acquire) (void)interrupted.acquire_native_merge("uncertain-op", f.key, "uncertain-pin");
          else interrupted.record_native_merge("uncertain-op", f.key, output, "uncertain-pin");
        });
        check(injection->hits == 1 && interrupted.poisoned(), "direct uncertain operation stayed healthy");
      }
      injection.reset();
      auto recovered = sqlite_catalog<P>::open(f.dir.root);
      check(bool(recovered.lookup_operation("uncertain-op")) == after, "lost acknowledgment operation boundary");
      check(scalar(f.dir.root, "SELECT count(*) FROM owner_objects") == before + after,
        "lost acknowledgment pin boundary");
      for (unsigned replay = 0; replay != 2; ++replay) {
        if (acquire) {
          auto receipt = recovered.acquire_native_merge("uncertain-op", f.key, "uncertain-pin");
          check(receipt && receipt->object == output.object, "acquisition reconciliation changed the winner");
        } else recovered.record_native_merge("uncertain-op", f.key, output, "uncertain-pin");
        check(recovered.lookup_operation("uncertain-op").has_value() &&
          scalar(f.dir.root, "SELECT count(*) FROM owner_objects") == before + 1 &&
          scalar(f.dir.root, "SELECT count(*) FROM completed_native_merges") == 1,
          "same-operation reconciliation duplicated its pin or hint");
      }
      auto winner = recovered.acquire_native_merge("winner-check", f.key, "winner-check-pin");
      check(winner && winner->object == output.object, "reconciled hint changed its winner");
    }
  }

  struct observed_storage : storage {
    using storage::storage;
    inline static std::string seen;
    static observed_storage open(std::filesystem::path const & root) { seen = "bare"; return observed_storage(context_type::open(root)); }
    static observed_storage open_for_schema(std::filesystem::path const & root, std::string_view schema) {
      seen = schema; return observed_storage(context_type::open(root, {}, {}, {}, {}, {}, std::string(schema)));
    }
  };
  struct legacy_storage : storage {
    using storage::storage;
    inline static bool opened = false;
    static legacy_storage open(std::filesystem::path const & root) { opened = true; return legacy_storage(context_type::open(root)); }
  };
  void schema_plumbing() {
    using observed_family = sort_runtime_family<P, registry_selector<string_registry>, observed_storage>;
    using observed_engine = typed_engine<P, wrapping_fingerprint_algebra, 256, observed_family>;
    using legacy_family = sort_runtime_family<P, registry_selector<string_registry>, legacy_storage>;
    using legacy_engine = typed_engine<P, wrapping_fingerprint_algebra, 256, legacy_family>;
    temporary dir;
    {
      auto db = connect<observed_engine>(dir.root, "typed", {.schema_id = schema}); db.put("a", "value");
      check(observed_storage::seen == schema, "connection did not pass exact schema");
    }
    {
      auto db = connect<legacy_engine>(dir.root, "typed", {.schema_id = schema});
      check(legacy_storage::opened && db.get("a") == "value", "custom open(root) fallback was bypassed");
    }
    auto disk = storage::open_for_schema(dir.root, "wrong");
    core empty(schema);
    rejects([&] { (void)core::from_snapshot(empty.snapshot(), disk); });
    check(!disk.context()->failed() && !extension(dir.root), "schema preflight poisoned context or created hint table");
  }
}
int main() {
  try {
    ordered_domains_and_replay(); adaptive_and_copied_catalog(); invalid_evidence(); simultaneous_completions();
    fork_and_checkpoint(); rebuilding_reuse_restart(); interrupted_hints(); reconcile_interrupted_operation(); schema_plumbing();
    std::cout << "durable ordered native merge reuse passed\n";
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
