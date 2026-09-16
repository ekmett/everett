/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks atomic durable range deletion, saved roots and byte/bit reopening.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>
#include <cassert>
#include <map>
#include <ranges>

namespace {
  using namespace everett;
  template <class P> void verify() {
    auto root = std::filesystem::temp_directory_path() / ("everett-range-" + random_object_ids{}().hex());
    struct cleanup {
      std::filesystem::path root;
      ~cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } guard{root};
    auto storage = multiverse<P>::create(root);
    std::map<std::string, std::string> expected;
    auto compare = [&](auto snapshot) {
      auto rows = range(snapshot);
      auto oracle = expected.begin();
      for (auto row : rows) {
        assert(oracle != expected.end() && row.key == oracle->first && row.value == oracle->second); ++oracle;
      }
      assert(oracle == expected.end() && snapshot.live_count() == expected.size());
      auto hash = std::uint64_t{0};
      for (auto const & [key, value] : expected) hash += sort_semantics<unsorted<std::optional<std::string>>>::hash_key(key) *
        sort_semantics<unsorted<std::optional<std::string>>>::hash_value(key, value);
      assert(snapshot.signature() == hash);
    };
    {
      auto db = storage.connect("main");
      auto batch = decltype(db)::core_type::batch();
      for (unsigned i = 0; i != 96; ++i) {
        auto key = "key/" + std::to_string(100 + i), value = "value/" + std::to_string(i);
        batch.put(key, value); expected[key] = value;
      }
      batch.put("", "empty").put(std::string("\0a", 2), "binary");
      expected[""] = "empty"; expected[std::string("\0a", 2)] = "binary";
      db.apply(std::move(batch).finish());
      db.save("before");
      auto old = db.snapshot();
      db.put("key/130", "replacement"); expected["key/130"] = "replacement";
      db.erase("key/131"); expected.erase("key/131");
      auto taken = db.range(std::string("key/120"), std::string("key/140")) | std::views::take(3);
      assert(std::ranges::distance(taken) == 3);
      auto contribution = erase_range(db.snapshot(), std::string("key/120"), std::string("key/140"));
      db.put("elsewhere", "disjoint"); expected["elsewhere"] = "disjoint";
      db.apply(std::move(contribution));
      for (unsigned i = 120; i != 140; ++i) expected.erase("key/" + std::to_string(i));
      compare(db.snapshot());
      assert(old.get("key/130") == "value/30" && old.live_count() == 98);
      db.erase_range_async(std::string("key/150"), std::string("key/170")).get();
      for (unsigned i = 150; i != 170; ++i) expected.erase("key/" + std::to_string(i));
      compare(db.snapshot());
      auto stale = erase_range(db.snapshot(), std::string("key/180"), std::string("key/190"));
      db.put("key/185", "changed"); expected["key/185"] = "changed";
      bool rejected = false;
      try { db.apply(std::move(stale)); } catch (std::invalid_argument const &) { rejected = true; }
      assert(rejected && !db.failure()); compare(db.snapshot());
      db.erase_range(std::nullopt, std::string("key/100"));
      expected.erase(""); expected.erase(std::string("\0a", 2)); expected.erase("elsewhere");
      compare(db.snapshot());
      db.save("after");
    }
    auto detached = [&] {
      auto db = storage.connect("main");
      return db.range().begin();
    }();
    auto oracle = expected.begin();
    while (detached != std::default_sentinel) {
      auto row = *detached;
      assert(oracle != expected.end() && row.key == oracle->first && row.value == oracle->second);
      ++oracle; ++detached;
    }
    assert(oracle == expected.end());
    {
      auto db = storage.connect("main"); compare(db.snapshot());
      auto before = db.load("before"), after = db.load("after");
      assert(before && after && before->live_count() == 98 && before->get("key/130") == "value/30");
      compare(*after);
      auto branch = db.fork("branch", *before);
      branch.erase_range();
      assert(branch.snapshot().live_count() == 0 && branch.snapshot().signature() == 0);
      compare(db.snapshot());
    }
  }
}
int main() { verify<everett::string_policy>(); verify<everett::storage_policy<>>(); }
