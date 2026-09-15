/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks bounded owner-cache retirement, alias identity and moved sweep cursors.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/runtime_store.h>

#include <cassert>
#include <array>
#include <iostream>

namespace {
  using cache = diet::runtime_store_detail::owner_cache<int, unsigned>;
  void retention() {
    cache values;
    std::vector<std::shared_ptr<int const>> retained;
    for (unsigned i = 0; i != 8192; ++i) {
      retained.push_back(std::make_shared<int const>(int(i)));
      values.insert_or_assign(retained.back(), i);
    }
    assert(values.size() == retained.size());
    auto examined = values.prune(7);
    assert(examined > 0 && examined <= 7 && values.size() == retained.size());
    for (unsigned i = 0; i != retained.size(); ++i) assert(values.at(retained[i]) == i);
    for (unsigned i = 0; i != 65536; ++i) {
      auto transient = std::make_shared<int const>(-1);
      values.insert_or_assign(transient, i);
      assert(values.at(transient) == i);
    }
    // Cleanup accompanies insertion; retained owners remain reusable without
    // making any one sweep walk their complete set.
    assert(values.size() < 3 * retained.size());
    auto bound = values.size();
    for (std::size_t i = 0; i != bound + 1; ++i) assert(values.prune(1) <= 1);
    assert(values.size() == retained.size());
    retained.clear();
    bound = values.size();
    for (std::size_t i = 0; i != bound + 1; ++i) assert(values.prune(1) <= 1);
    assert(values.size() == 0 && values.prune(1000) == 0);
  }
  void aliases() {
    cache values;
    auto owner = std::make_shared<std::array<int, 2>>();
    std::shared_ptr<int const> a(owner, &(*owner)[0]), b(owner, &(*owner)[1]);
    values.insert_or_assign(a, 1);
    assert(values.at(b) == 1);
    values.insert_or_assign(b, 2);
    assert(values.size() == 1 && values.at(a) == 2);
    auto other = std::make_shared<int const>(0);
    values.insert_or_assign(other, 3);
    assert(values.size() == 2 && values.at(other) == 3);
    assert(values.prune(0) == 0 && values.size() == 2);
  }
  void moved_cursors() {
    for (unsigned position = 0; position != 5; ++position) {
      cache source;
      std::vector<std::shared_ptr<int const>> owners;
      for (unsigned i = 0; i != 4; ++i) {
        owners.push_back(std::make_shared<int const>(int(i)));
        source.insert_or_assign(owners.back(), i);
      }
      (void)source.prune(position);
      cache moved(std::move(source));
      assert(source.size() == 0 && source.prune() == 0);
      for (unsigned i = 0; i != owners.size(); ++i) assert(moved.at(owners[i]) == i);
      auto fresh = std::make_shared<int const>(9);
      source.insert_or_assign(fresh, 9);
      source = std::move(moved);
      assert(moved.size() == 0 && moved.prune() == 0);
      assert(source.find(fresh) == source.end());
      owners.clear();
      for (unsigned i = 0; i != 8; ++i) assert(source.prune(1) <= 1);
      assert(source.size() == 0);
      cache empty;
      source.swap(empty);
      assert(source.prune() == 0 && empty.prune() == 0);
    }
  }
}
int main() {
  try { retention(); aliases(); moved_cursors(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
