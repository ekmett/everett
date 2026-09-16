/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks persistent nursery branches, private edit reuse and failure isolation.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/nursery_map.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
  void check(bool condition, char const * message) {
    if (!condition) { std::cerr << message << '\n'; std::abort(); }
  }
  template <class E, class F> void rejects(F && operation, char const * message) {
    bool caught = false;
    try { operation(); }
    catch (E const &) { caught = true; }
    catch (...) { check(false, "unexpected exception type"); }
    check(caught, message);
  }

  using map_type = everett::nursery_map<int, std::string>;
  using oracle_type = std::map<int, std::string>;
  static_assert(!std::is_copy_constructible_v<map_type>);
  static_assert(std::is_nothrow_move_constructible_v<map_type>);
  static_assert(std::is_nothrow_copy_constructible_v<map_type::snapshot_type>);

  template <class Map> void compare(Map const & map, oracle_type const & expected) {
    check(map.size() == expected.size(), "nursery size mismatch");
    check(map.empty() == expected.empty(), "nursery empty mismatch");
    auto next = expected.begin();
    map.for_each([&](int const & key, std::string const & value) {
      check(next != expected.end() && next->first == key && next->second == value, "nursery sorted traversal mismatch");
      auto found = map.find(key);
      check(found && *found == value, "nursery find mismatch");
      ++next;
    });
    check(next == expected.end(), "nursery traversal omitted records");
    for (int key = -4; key != 260; ++key) {
      auto actual = map.find(key);
      auto found = expected.find(key);
      check(bool(actual) == (found != expected.end()), "nursery missing-key lookup mismatch");
      if (actual) check(*actual == found->second, "nursery lookup value mismatch");
    }
  }

  void empty_and_moving() {
    map_type map;
    compare(map, {});
    check(!map.erase(3), "erased an absent key");
    auto empty = map.freeze();
    compare(empty, {});
    map_type::snapshot_type default_empty;
    compare(default_empty, {});
    auto branch = map_type::thaw(empty);
    branch.insert_or_assign(7, "seven");
    compare(empty, {});
    auto moved = std::move(branch);
    compare(branch, {});
    compare(moved, {{7, "seven"}});
    branch.insert_or_assign(9, "nine");
    moved = std::move(branch);
    compare(moved, {{9, "nine"}});
    compare(branch, {});
    auto saved = moved.freeze();
    auto moved_snapshot = std::move(saved);
    compare(saved, {});
    compare(moved_snapshot, {{9, "nine"}});
    default_empty = std::move(moved_snapshot);
    compare(moved_snapshot, {});
    compare(default_empty, {{9, "nine"}});
  }

  void reuse_and_old_branches() {
    map_type map;
    oracle_type before;
    for (int key = 0; key != 256; ++key) {
      auto text = std::to_string(key);
      check(map.insert_or_assign(key, text), "insert reported replacement");
      before.emplace(key, text);
    }
    check(map.work().nodes_created == 256 && map.work().path_copies == 0, "private insertion copied paths");
    check(map.work().rotations != 0, "ordered insertion did not exercise AVL rotations");
    auto work = map.work();
    for (int repeat = 0; repeat != 100; ++repeat)
      check(!map.insert_or_assign(97, "latest"), "replacement reported insertion");
    check(map.work().nodes_created == work.nodes_created, "private replacements allocated tree nodes");
    before[97] = "latest";
    auto original = map.freeze();
    auto copied = original;
    auto frozen_again = map.freeze();
    check(map.work().nodes_created == work.nodes_created, "freeze copied tree nodes");
    auto left = map_type::thaw(original);
    auto right = map_type::thaw(copied);
    check(left.work().nodes_created == 0 && right.work().nodes_created == 0, "thaw copied nodes");
    auto left_oracle = before, right_oracle = before;
    for (int key = 0; key != 256; key += 2) {
      check(left.erase(key), "left erase missing"); left_oracle.erase(key);
    }
    for (int key = 1; key < 256; key += 2) {
      right.insert_or_assign(key, "right"); right_oracle[key] = "right";
    }
    map.insert_or_assign(1000, "future");
    compare(original, before); compare(copied, before); compare(frozen_again, before);
    compare(left, left_oracle); compare(right, right_oracle);
    // Fork the oldest version after descendants have edited and rotated paths.
    auto late = map_type::thaw(original);
    late.insert_or_assign(-1, "old branch");
    auto late_oracle = before; late_oracle[-1] = "old branch";
    compare(late, late_oracle); compare(original, before);
    check(left.work().path_copies != 0 && right.work().path_copies != 0, "snapshot edits did not copy paths");
  }

  void randomized_versions() {
    std::mt19937 random(0x19570916u);
    std::vector<std::pair<map_type::snapshot_type, oracle_type>> saved;
    map_type map;
    oracle_type expected;
    saved.emplace_back(map.freeze(), expected);
    for (unsigned step = 0; step != 12000; ++step) {
      if (step % 113 == 0) {
        auto const & old = saved[random() % saved.size()];
        map = map_type::thaw(old.first); expected = old.second;
      }
      int key = int(random() % 256);
      if ((random() & 3u) == 0) {
        bool removed = expected.erase(key) != 0;
        check(map.erase(key) == removed, "random erase result mismatch");
      } else {
        auto text = std::to_string(random());
        auto inserted = expected.insert_or_assign(key, text).second;
        check(map.insert_or_assign(key, std::move(text)) == inserted, "random insertion result mismatch");
      }
      if (step % 61 == 0) {
        compare(map, expected);
        saved.emplace_back(map.freeze(), expected);
      }
    }
    compare(map, expected);
    for (auto const & [snapshot, oracle] : saved) compare(snapshot, oracle);
  }

  struct counted_compare {
    std::shared_ptr<std::size_t> count;
    bool operator()(int a, int b) const { ++*count; return a < b; }
  };
  void deletion_and_balance() {
    auto comparisons = std::make_shared<std::size_t>(0);
    everett::nursery_map<int, int, counted_compare> map(counted_compare{comparisons});
    constexpr int count = 8192;
    for (int key = 0; key != count; ++key) map.insert_or_assign(key, key);
    auto full = map.freeze();
    for (int key = 0; key != count; ++key) {
      *comparisons = 0;
      check(map.find(key) && *map.find(key) == key, "large AVL key missing");
      auto bound = 8u * unsigned(std::ceil(std::log2(double(count - key + 1)))) + 8;
      check(*comparisons <= bound, "AVL search path exceeds height bound");
      check(map.erase(key), "large AVL deletion missing");
    }
    check(map.empty() && full.size() == std::size_t(count), "deletion changed retained snapshot");
    for (int key = count - 1; key >= 0; --key) check(full.find(key) && *full.find(key) == key, "old rotated path corrupted");
  }

  struct length_compare {
    bool operator()(std::string const & a, std::string const & b) const { return a.size() < b.size(); }
  };
  void comparator_and_move_only_values() {
    everett::nursery_map<std::string, std::unique_ptr<int>, length_compare> map;
    map.insert_or_assign("abc", std::make_unique<int>(1));
    auto old = map.freeze();
    check(!map.insert_or_assign("xyz", std::make_unique<int>(2)), "equivalent key inserted twice");
    check(**old.find("xyz") == 1 && **map.find("abc") == 2, "move-only value isolation failed");
    map.for_each([](std::string const & key, auto const & value) {
      check(key == "abc" && *value == 2, "equivalent key representative changed");
    });
    auto fork = decltype(map)::thaw(old);
    check(fork.erase("uvw"), "equivalent-key erase failed");
    check(old.size() == 1 && map.size() == 1 && fork.empty(), "move-only branch deletion leaked");
  }

  struct throw_control { int remaining = -1; };
  struct throwing_compare {
    std::shared_ptr<throw_control> control;
    bool operator()(int a, int b) const {
      if (!control->remaining) throw std::runtime_error("injected comparator failure");
      if (control->remaining > 0) --control->remaining;
      return a < b;
    }
  };
  struct throwing_value {
    static inline int live = 0;
    static inline bool fail_copy = false;
    static inline bool fail_move = false;
    int value;
    explicit throwing_value(int next) : value(next) { ++live; }
    throwing_value(throwing_value const & other) : value(other.value) {
      if (fail_copy) throw std::runtime_error("injected argument copy failure");
      ++live;
    }
    throwing_value(throwing_value && other) : value(other.value) {
      if (fail_move) throw std::runtime_error("injected value move failure");
      ++live;
    }
    ~throwing_value() { --live; }
  };
  void failures_preserve_snapshots() {
    auto control = std::make_shared<throw_control>();
    everett::nursery_map<int, std::string, throwing_compare> map(throwing_compare{control});
    oracle_type expected;
    for (int key = 0; key != 128; ++key) { map.insert_or_assign(key, "old"); expected[key] = "old"; }
    auto snapshot = map.freeze();
    control->remaining = 4;
    rejects<std::runtime_error>([&] { map.insert_or_assign(1000, "new"); }, "comparison did not fail");
    check(map.failed() && map.work().path_copies != 0, "partly edited transient was not poisoned");
    control->remaining = -1;
    compare(snapshot, expected);
    rejects<std::logic_error>([&] { (void)map.find(0); }, "failed transient allowed lookup");
    rejects<std::logic_error>([&] { (void)map.freeze(); }, "failed transient published snapshot");
    rejects<std::logic_error>([&] { map.erase(0); }, "failed transient allowed erase");
    auto recovered = decltype(map)::thaw(snapshot);
    recovered.insert_or_assign(1000, "recovered");
    compare(snapshot, expected);

    {
      everett::nursery_map<int, throwing_value> values;
      values.insert_or_assign(1, throwing_value(7));
      auto retained = values.freeze();
      throwing_value replacement(8);
      throwing_value::fail_copy = true;
      rejects<std::runtime_error>([&] { values.insert_or_assign(1, replacement); }, "argument construction did not fail");
      throwing_value::fail_copy = false;
      check(!values.failed() && values.find(1)->value == 7, "argument rejection poisoned unchanged transient");
      throwing_value::fail_move = true;
      rejects<std::runtime_error>([&] { values.insert_or_assign(1, replacement); }, "value construction did not fail");
      throwing_value::fail_move = false;
      check(values.failed() && retained.find(1)->value == 7, "failed value replacement changed snapshot");
      auto resumed = decltype(values)::thaw(retained);
      resumed.insert_or_assign(1, throwing_value(9));
      check(retained.find(1)->value == 7 && resumed.find(1)->value == 9, "value failure prevented independent branch");
    }
    check(throwing_value::live == 0, "nursery leaked value ownership");
  }
}

int main() {
  empty_and_moving();
  reuse_and_old_branches();
  randomized_versions();
  deletion_and_balance();
  comparator_and_move_only_values();
  failures_preserve_snapshots();
  std::cout << "persistent nursery map checks passed\n";
}
