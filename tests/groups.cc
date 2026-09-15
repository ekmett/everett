#include <everett/rank_groups.h>
#include <everett/select_groups.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F action) {
    bool threw = false;
    try { action(); } catch (std::exception const &) { threw = true; }
    require(threw, "invalid group operation accepted");
  }

  template <std::uint64_t K> void test_rank() {
    std::mt19937_64 random(0x6715 + K);
    for (std::uint64_t count : std::array<std::uint64_t, 12>{0, 1, K - 1, K, K + 1, 31 * K,
                                                           63 * K + 1, 127 * K, 128 * K,
                                                           128 * K + 1, 129 * K, 1000 * K - 1}) {
      auto groups = count / K + (count % K != 0);
      for (unsigned pattern = 0; pattern < 3; ++pattern) {
        std::vector<std::uint64_t> classes(groups);
        std::vector<std::uint64_t> oracle(groups + 1);
        for (std::uint64_t i = 0; i < groups; ++i) {
          auto capacity = i + 1 == groups && count % K ? count % K : K;
          classes[i] = pattern == 0 ? 0 : pattern == 1 ? capacity : random() % (capacity + 1);
          oracle[i + 1] = oracle[i] + classes[i];
        }
        auto index = everett::rank_groups<K>::build(classes, count);
        auto view = index.view();
        require(view.size() == count && view.group_count() == groups, "rank groups dimensions");
        for (std::uint64_t i = 0; i <= groups; ++i)
          require(view.rank(i) == oracle[i], "rank groups prefix oracle");
        for (std::uint64_t i = 0; i < groups; ++i)
          require(view.class_at(i) == classes[i], "rank groups class oracle");
        // Independent bit-at-a-time oracle checks packed fields crossing words.
        constexpr unsigned width = everett::rank_groups<K>::class_bits;
        for (std::uint64_t i = 0; i < groups; ++i)
          for (unsigned bit = 0; bit < width; ++bit) {
            auto position = i * width + bit;
            require(((index.classes[position / 64] >> (position % 64)) & 1) == ((classes[i] >> bit) & 1),
                    "rank class packed bit mismatch");
          }
        rejects([&] { view.rank(groups + 1); });
        rejects([&] { view.class_at(groups); });
      }
    }
    rejects([] { everett::rank_groups<K>::build(std::array<std::uint64_t, 1>{K + 1}, K); });
    rejects([] { everett::rank_groups<K>::build(std::array<std::uint64_t, 1>{2}, 1); });
    rejects([] { everett::rank_groups<K>::build({}, K); });
    everett::rank_groups<K> empty;
    require(empty.view().rank(0) == 0, "default rank groups");
  }

  template <std::uint64_t K> everett::select_groups<K> check_select(
      std::vector<std::uint64_t> const & residuals, std::uint64_t count) {
    auto index = everett::select_groups<K>::build(residuals, count);
    auto view = index.view();
    for (std::uint64_t i = 0; i < residuals.size(); ++i) {
      auto ordinal = i + 1 == residuals.size() ? count : i * K;
      require(view.residual(i) == residuals[i], "group residual mismatch");
      // Same API supports byte and bit addresses: fixed stride is in the
      // same unit as the residual sequence, with no hidden eight-bit factor.
      for (std::uint64_t stride : {0u, 7u, 8u, 13u}) {
        if (!stride || ordinal <= (std::numeric_limits<std::uint64_t>::max() - residuals[i]) / stride)
          require(view.offset(i, stride) == residuals[i] + stride * ordinal, "group fixed stride mismatch");
        else rejects([&] { view.offset(i, stride); });
      }
    }
    rejects([&] { view.residual(residuals.size()); });
    return index;
  }

  template <std::uint64_t K> void test_select() {
    std::mt19937_64 random(0xef + K);
    for (std::uint64_t count : std::array<std::uint64_t, 10>{0, 1, K - 1, K, K + 1, 255 * K,
                                                           256 * K, 256 * K + 1, 257 * K, 10001 * K - 1}) {
      auto groups = count / K + (count % K != 0);
      for (unsigned pattern = 0; pattern < 4; ++pattern) {
        std::vector<std::uint64_t> residuals(groups + 1);
        for (std::size_t i = 1; i < residuals.size(); ++i)
          residuals[i] = residuals[i - 1] + (pattern == 0 ? 0 : pattern == 1 ? 1 :
                          pattern == 2 ? random() % 32 : (std::uint64_t{1} << 40) + random() % 32);
        check_select<K>(residuals, count);
      }
    }
    std::vector<std::uint64_t> skewed(10001);
    for (std::size_t i = 17; i < skewed.size(); ++i) skewed[i] = 20000;
    auto sparse = check_select<K>(skewed, 10000 * K);
    require(!sparse.sparse.empty(), "generic sparse select not exercised");
    auto maximum = std::numeric_limits<std::uint64_t>::max();
    check_select<K>({maximum}, 0);
    check_select<K>({0, maximum}, 1);
    auto tiny = check_select<K>({0, 7}, K - 1);
    rejects([&] { tiny.view().offset(1, maximum); });
    rejects([] { everett::select_groups<K>::build({}, 0); });
    rejects([] { everett::select_groups<K>::build(std::array<std::uint64_t, 2>{1, 0}, 1); });
    everett::select_groups<K> empty;
    require(empty.view().offset(0, 13) == 0, "default select groups");
  }

  void test_wide_policy() {
    constexpr std::uint64_t k = (std::uint64_t{1} << 63) - 1;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto index = everett::rank_groups<k>::build(std::array<std::uint64_t, 3>{k, k, 1}, maximum);
    require(index.view().rank(2) == maximum - 1 && index.view().rank(3) == maximum,
            "wide group counter overflow");
    require(index.view().class_at(1) == k && index.view().class_at(2) == 1, "63-bit packed classes");
    check_select<k>({0, 0, 0, 0}, maximum);
  }
}

int main() {
  try {
    test_rank<3>(); test_rank<7>(); test_rank<15>(); test_rank<31>();
    test_select<3>(); test_select<7>(); test_select<15>(); test_select<31>();
    test_wide_policy();
    std::cout << "Policy groups 3/7/15/31, packed classes, and residual-address select checks passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
 * \endlicense
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's groups behavior.
 */
