/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks that shared owners reuse exact sealed graphs across concurrent catalog adapters.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/connection.h>

#include <array>
#include <cassert>
#include <future>
#include <iostream>
#include <latch>

namespace {
  using namespace diet;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-owner-binding-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  std::size_t files(std::filesystem::path const & root) {
    std::size_t result = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++result;
    return result;
  }

  template <class Family> void reuse() {
    using core = typed_engine<string_policy, wrapping_fingerprint_algebra, 256, Family>;
    using store = runtime_store<string_policy, random_object_ids, sqlite_catalog_ops, Family>;
    temporary original, clone, independent;
    core active;
    for (unsigned i = 0; i != 19; ++i)
      active.contribute(core::put("key-" + std::to_string(i), std::string(1024, char('a' + i))));
    while (active.pending()) active.advance(1'000'000);
    auto source = active.snapshot();
    auto semantic = source.metadata().encode();
    { auto initialized = store::create(original.root); }

    // Both adapters receive the same unbound immutable owner graph. Its
    // binding slots coordinate physical identities, not just logical equality.
    std::latch start(1);
    std::array<std::future<typename store::stored_type>, 2> publishers;
    for (unsigned i = 0; i != publishers.size(); ++i)
      publishers[i] = std::async(std::launch::async, [&, i] {
        auto adapter = store::open(original.root, {}, {.busy_timeout_ms = 5000});
        start.wait();
        return adapter.create_tap("publisher-" + std::to_string(i), source.runtime(), semantic);
      });
    start.count_down();
    auto first = publishers[0].get(), second = publishers[1].get();
    assert(first.head.timeline.head == second.head.timeline.head);
    assert(first.head.auxiliary == second.head.auxiliary);
    assert(first.snapshot.query_root().head() == second.snapshot.query_root().head());
    auto count = files(original.root);
    {
      auto adapter = store::open(original.root);
      auto third = adapter.create_tap("third", source.runtime(), semantic);
      assert(third.head.timeline.head == first.head.timeline.head && files(original.root) == count);
      assert(third.snapshot.query_root().head() == first.snapshot.query_root().head());
      auto mapped = core::cola_type::restore(third.snapshot, source.metadata(), source.metadata().schema_id);
      assert(mapped.get("key-18") == std::string(1024, 's'));
    }

    // No SQLite connection is live during this directory copy. The backup has
    // the same catalog identity and object names, but a distinct local namespace.
    std::filesystem::copy(original.root, clone.root, std::filesystem::copy_options::recursive);
    {
      auto a = sqlite_catalog<string_policy>::open(original.root);
      auto b = sqlite_catalog<string_policy>::open(clone.root);
      assert(a.identity() == b.identity() && a.root() != b.root());
    }
    {
      auto adapter = store::open(clone.root);
      auto copied = adapter.create_tap("clone", source.runtime(), semantic);
      assert(copied.head.timeline.head != first.head.timeline.head);
      assert(files(clone.root) > count && files(original.root) == count);
      auto reopened = store::open(clone.root).find("clone");
      assert(reopened && reopened->head == copied.head);
    }
    {
      auto adapter = store::create(independent.root);
      auto other = adapter.create_tap("independent", source.runtime(), semantic);
      assert(other.head.timeline.head != first.head.timeline.head);
      auto again = store::open(original.root).find("publisher-0");
      assert(again && again->head == first.head && files(original.root) == count);
    }
  }
}
int main() {
  try {
    reuse<binary_runtime_family<string_policy>>();
    reuse<sort_runtime_family<string_policy>>();
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
