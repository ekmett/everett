/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks lazy index reservations, retained output lifetimes and uncertain seals.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sort_runtime_context.h>

#include <cassert>
#include <iostream>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using P = storage_policy<string_registry, 3>;
  using family = streaming_sort_runtime_family<P>;
  using storage = family::storage_type;
  using Node = family::node_type;
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "everett-adaptive-index-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp");
      root = name;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && fn) {
    bool caught = false; try { fn(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  std::size_t objects(std::filesystem::path const & root) {
    std::size_t count = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++count;
    return count;
  }
  auto native(std::string key = "key") {
    sort_profile_writer<P> writer; writer.append<strings>(key, "value");
    return family::native_type::from_owned(writer.finish());
  }
  void deferred() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id(1));
    auto disk = storage::open(dir.root); auto context = disk.context();
    auto input = native(); auto initial = objects(dir.root);
    auto job = context->make_index<Node>(input, {}, {});
    assert(!job->spilled() && objects(dir.root) == initial);
    while (!job->done()) job->step(1);
    auto result = context->finish_index<Node>(*job);
    assert(result->built() && !result->mapped() && result->native_owner() == input);
    assert(!context->sealed_indexes() && objects(dir.root) == initial && context->retained_output_bytes());
    std::weak_ptr<storage::context_type> weak = context;
    auto charge = context->retained_output_bytes();
    auto another = context->make_index<Node>(input, result, input);
    while (!another->done()) another->step(1);
    auto next = context->finish_index<Node>(*another);
    assert(next->built() && next->main_target() == result && next->secondary_target() == input);
    assert(context->retained_output_bytes() > charge && objects(dir.root) == initial);
    job.reset(); another.reset(); disk = storage{}; context.reset();
    assert(weak.expired()); // Output leases retain the allowance, not its SQLite connection.
    auto key = sort_key_transport<P>::encode<strings>("key");
    auto query = cola_query_root<P, Node>::adopt_prepared(next).cursor(key.view());
    while (!query.done() && !query.has_match()) query.step(1);
    assert(query.has_match());
  }
  void spill_and_lifetime() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id(1));
    auto disk = storage::open(dir.root); auto context = disk.context();
    auto a = native("a"), b = native(std::string(150000, 'b'));
    auto job = context->make_index<Node>(a, {}, b);
    assert(!job->spilled() && objects(dir.root) == 0);
    std::weak_ptr<storage::context_type> weak = context;
    context.reset(); disk = storage{}; assert(!weak.expired());
    while (!job->done()) job->step(1);
    assert(job->spilled() && job->spooled_bytes() > 65536);
    auto result = weak.lock()->finish_index<Node>(*job);
    assert(result->mapped() && !result->built()); result->mapped()->scan();
    assert(result->native_owner() == a && result->secondary_target() == b);
    job.reset(); assert(weak.expired());
  }
  struct fault {
    unsigned commits = 0, stop = 0;
    bool after = false;
  };
  struct catalog_ops {
    std::shared_ptr<fault> state;
    int commit(sqlite3 * db) noexcept {
      auto fail = ++state->commits == state->stop;
      if (fail && !state->after) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return fail && result == SQLITE_OK ? SQLITE_IOERR_FSYNC : result;
    }
  };
  void uncertain_seal() {
    using F = streaming_sort_runtime_family<P, registry_selector<string_registry>, random_object_ids, catalog_ops>;
    using N = F::node_type;
    for (bool after : {false, true}) {
      temporary dir; auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id(1));
      auto state = std::make_shared<fault>();
      auto disk = F::storage_type::open(dir.root, {}, {}, catalog_ops{state}, {}, {0, 0});
      auto context = disk.context();
      auto job = context->template make_index<N>(native(), {}, {});
      assert(job->spilled()); while (!job->done()) job->step(1);
      state->stop = state->commits + 1; state->after = after;
      rejects([&] { (void)context->template finish_index<N>(*job); });
      assert(context->failed() && !context->sealed_indexes());
      rejects([&] { (void)context->template finish_index<N>(*job); });
      auto reopened = sqlite_catalog<P>::open(dir.root); assert(reopened.identity() == id(1));
    }
  }
}
int main() {
  try { deferred(); spill_and_lifetime(); uncertain_seal(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
