/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/rank_groups.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
  thread_local long fail_after = -1;
  thread_local bool count_allocations = false;
  thread_local std::size_t allocations = 0;
  void * allocate(std::size_t bytes) {
    if (count_allocations) ++allocations;
    if (fail_after >= 0 && fail_after-- == 0) { fail_after = -1; throw std::bad_alloc(); }
    if (auto result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
  }
}
void * operator new(std::size_t n) { return allocate(n); }
void * operator new[](std::size_t n) { return allocate(n); }
void operator delete(void * p) noexcept { std::free(p); }
void operator delete[](void * p) noexcept { std::free(p); }
void operator delete(void * p, std::size_t) noexcept { std::free(p); }
void operator delete[](void * p, std::size_t) noexcept { std::free(p); }

namespace {
  using namespace everett;
  void require(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && f) {
    bool caught = false;
    try { f(); } catch (std::exception const &) { caught = true; }
    require(caught, "invalid rank builder operation accepted");
  }
  template <std::uint64_t K> void check(rank_groups<K> const & actual,
      std::span<std::uint64_t const> classes, std::uint64_t count) {
    auto batch = rank_groups<K>::build(classes, count);
    require(actual.classes == batch.classes && actual.checkpoints == batch.checkpoints &&
      actual.virtual_count == count, "incremental rank wire differs from batch");
    auto view = actual.view();
    std::uint64_t prefix = 0;
    for (std::size_t i = 0; i != classes.size(); ++i) {
      require(view.rank(i) == prefix && view.class_at(i) == classes[i], "incremental rank independent oracle");
      prefix += classes[i];
    }
    require(view.count() == prefix, "incremental rank final population");
    if (classes.empty()) require(actual.classes.empty() && actual.checkpoints.empty(), "empty rank directory");
  }
  template <std::uint64_t K> void matrix() {
    std::mt19937_64 rng(0x75643821 + K);
    for (std::size_t n : {0u, 1u, 2u, 15u, 16u, 17u, 21u, 22u, 63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 1025u}) {
      for (std::uint64_t tail : {std::uint64_t{1}, K - 1, K}) {
        rank_groups_builder<K> builder;
        std::vector<std::uint64_t> classes;
        std::uint64_t count = 0;
        for (std::size_t i = 0; i != n; ++i) {
          auto width = i + 1 == n ? tail : K;
          auto value = i % 3 == 0 ? width : i % 3 == 1 ? 0 : rng() % (width + 1);
          builder.append(value, width); count += width; classes.push_back(value);
          require(builder.size() == count && builder.group_count() == i + 1, "incremental rank progress");
        }
        auto result = builder.finish();
        require(builder.finished(), "rank finish state");
        check(result, classes, count);
        rejects([&] { builder.append(0); });
        rejects([&] { builder.finish(); });
      }
    }
  }
  void invalid_and_moves() {
    rank_groups_builder<15> source;
    rejects([&] { source.append(16); });
    rejects([&] { source.append(0, 0); });
    rejects([&] { source.append(0, 16); });
    rejects([&] { source.append(4, 3); });
    require(!source.size() && !source.group_count(), "rejected rank group changed empty state");
    source.append(7);
    auto moved = std::move(source);
    rejects([&] { source.append(0); });
    rejects([&] { source.finish(); });
    rank_groups_builder<15> assigned;
    assigned.append(12);
    assigned = std::move(moved);
    rejects([&] { moved.finish(); });
    auto * self = &assigned;
    assigned = std::move(*self);
    assigned.append(2, 3);
    rejects([&] { assigned.append(0); });
    check(assigned.finish(), std::array<std::uint64_t, 2>{7, 2}, 18);

    constexpr auto k = (std::uint64_t{1} << 63) - 1;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    rank_groups_builder<k> wide;
    wide.append(k); wide.append(k);
    rejects([&] { wide.append(0); });
    require(wide.size() == maximum - 1, "overflow rejection changed rank builder");
    wide.append(1, 1);
    check(wide.finish(), std::array<std::uint64_t, 3>{k, k, 1}, maximum);
  }
  void allocation_failures() {
    std::vector<std::uint64_t> classes(32769);
    for (std::size_t i = 0; i != classes.size(); ++i) classes[i] = (i * 13) & 15;
    classes.back() = 1;
    auto run = [&](long failure) {
      rank_groups_builder<15> builder;
      fail_after = failure; count_allocations = true; allocations = 0;
      bool caught = false;
      for (std::size_t i = 0; i != classes.size(); ++i) {
        auto width = i + 1 == classes.size() ? 1 : 15;
        try { builder.append(classes[i], width); }
        catch (std::bad_alloc const &) {
          caught = true;
          require(builder.size() == i * 15 && builder.group_count() == i,
            "allocation failure changed accepted rank groups");
          builder.append(classes[i], width);
        }
      }
      fail_after = -1; count_allocations = false;
      auto count = allocations;
      require(caught == (failure >= 0), "rank allocation cut not reached");
      check(builder.finish(), classes, (classes.size() - 1) * 15 + 1);
      return count;
    };
    auto count = run(-1);
    require(count <= 32, "rank buffers did not grow geometrically");
    for (std::size_t cut = 0; cut != count; ++cut) (void)run(static_cast<long>(cut));
  }
}

int main() try {
  matrix<3>(); matrix<7>(); matrix<15>(); matrix<31>(); matrix<63>(); matrix<127>();
  invalid_and_moves(); allocation_failures();
  std::cout << "Incremental packed rank groups, boundary oracles and allocation retry passed\n";
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n'; return 1;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks incremental packed rank construction and retry without expanded classes.
 */
