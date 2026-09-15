#include <everett/rank.h>
#include <everett/rank15.h>
#include <everett/select15.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <initializer_list>
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
    require(threw, "invalid input was accepted");
  }

  void test_rank_directory() {
    // Exercise every possible population in each position against a scalar
    // oracle, mixing maximum and zero neighboring lanes to expose carries.
    for (unsigned value = 0; value <= 512; ++value) {
      for (unsigned lane = 0; lane < 3; ++lane) {
        for (unsigned other : {0u, 511u, 512u}) {
          std::array<unsigned, 3> counts{other, other, other};
          counts[lane] = value;
          auto packed = counts[0] | (counts[1] << 10) | (counts[2] << 20);
          unsigned expected = 0;
          for (unsigned run = 0; run < 4; ++run) {
            require(everett::rank_detail::run_prefix(packed, run) == expected,
                    "SWAR ten-bit prefix mismatch");
            if (run < 3) expected += counts[run];
          }
        }
      }
    }

    // Synthetic counts for an all-one source at real block positions. This
    // checks the actual builder's epoch state without allocating a fake span
    // or a 512 MiB payload merely to reach the first 64-bit absolute count.
    everett::rank_detail::directory_cursor cursor;
    constexpr std::uint64_t epoch_blocks = std::uint64_t{1} << 21;
    constexpr std::uint64_t epoch_bits = std::uint64_t{1} << 32;
    require(cursor.before(0, 0) == 0, "initial rank epoch");
    require(cursor.before(epoch_blocks - 1, epoch_bits - 2048) == epoch_bits - 2048,
            "last 32-bit relative rank");
    require(cursor.before(epoch_blocks, epoch_bits) == 0 && cursor.epoch_base == epoch_bits,
            "rank epoch transition");
    require(cursor.before(epoch_blocks + 1, epoch_bits + 2048) == 2048,
            "rank after epoch transition");
    require(cursor.before(2 * epoch_blocks, 2 * epoch_bits) == 0 && cursor.epoch_base == 2 * epoch_bits,
            "second rank epoch transition");
    rejects([&] { cursor.before(2 * epoch_blocks + 1, cursor.epoch_base - 1); });
    rejects([&] { cursor.before(2 * epoch_blocks + 1, cursor.epoch_base + epoch_bits); });
  }

  void test_rank() {
    std::mt19937_64 random(0x915);
    for (std::uint64_t bits : std::initializer_list<std::uint64_t>{0, 1, 63, 64, 65, 511, 512, 513, 1024,
                              1536, 2047, 2048, 2049, 8193, 100003}) {
      for (unsigned pattern = 0; pattern < 5; ++pattern) {
        std::vector<std::uint64_t> source(bits / 64 + (bits % 64 != 0));
        std::vector<std::uint64_t> oracle(bits + 1);
        for (std::uint64_t i = 0; i < bits; ++i) {
          bool bit = pattern == 1 || (pattern == 2 && i % 512 == 0) ||
                     (pattern == 3 && i % 2048 >= 1536) || (pattern == 4 && (random() & 1));
          oracle[i + 1] = oracle[i] + bit;
          if (bit) source[i / 64] |= std::uint64_t{1} << (i % 64);
        }
        // Garbage padding must not contribute to the last block or total.
        if (bits % 64) source.back() |= ~std::uint64_t{0} << (bits % 64);
        auto index = everett::rank_index::build(source, bits);
        auto view = index.view();
        require(view.count() == oracle.back(), "rank total");
        for (std::uint64_t i = 0; i <= bits; ++i)
          require(view.rank(i) == oracle[i], "rank prefix mismatch");
        rejects([&] { view.rank(bits + 1); });
        if (bits >= 2048 && pattern == 1)
          require(index.blocks[0].runs == (512u | (512u << 10) | (512u << 20)),
                  "packed runs must hold independent populations of 512");
      }
    }
    rejects([] { everett::rank_index::build({}, 1); });
    everett::rank_index empty;
    require(empty.view().rank(0) == 0, "default rank");
  }

  void test_rank15() {
    std::mt19937_64 random(0x1515);
    for (std::uint64_t bits : std::initializer_list<std::uint64_t>{0, 1, 14, 15, 16, 239, 240, 241, 1919, 1920,
                              1921, 2048, 300001}) {
      for (unsigned pattern = 0; pattern < 3; ++pattern) {
        auto groups = bits / 15 + (bits % 15 != 0);
        std::vector<std::uint8_t> classes(groups);
        std::vector<std::uint64_t> oracle(groups + 1);
        for (std::uint64_t i = 0; i < bits; ++i)
          if (pattern == 1 || (pattern == 2 && (random() & 1))) ++classes[i / 15];
        for (std::uint64_t i = 0; i < groups; ++i) oracle[i + 1] = oracle[i] + classes[i];
        auto index = everett::rank15_index::build(classes, bits);
        auto view = index.view();
        require(view.group_count() == groups && view.count() == oracle.back(), "rank15 shape");
        for (std::uint64_t i = 0; i <= groups; ++i)
          require(view.rank(i) == oracle[i], "rank15 prefix mismatch");
        for (std::uint64_t i = 0; i < groups; ++i)
          require(view.class_at(i) == classes[i], "rank15 packed class");
        rejects([&] { view.rank(groups + 1); });
        rejects([&] { view.class_at(groups); });
      }
    }
    rejects([] { everett::rank15_index::build({}, 15); });
    rejects([] { everett::rank15_index::build(std::array<std::uint8_t, 1>{16}, 15); });
    rejects([] { everett::rank15_index::build(std::array<std::uint8_t, 1>{2}, 1); });
    everett::rank15_index empty;
    require(empty.view().rank(0) == 0, "default rank15");
  }

  everett::select15_index check_select(std::vector<std::uint64_t> const & offsets,
                                            std::uint64_t records, std::uint64_t fixed = 8) {
    auto index = everett::select15_index::build(offsets, records);
    auto view = index.view();
    for (std::uint64_t i = 0; i < offsets.size(); ++i) {
      auto ordinal = i + 1 == offsets.size() ? records : i * 15;
      require(view.residual(i) == offsets[i], "Elias-Fano residual mismatch");
      if (!fixed || ordinal <= (std::numeric_limits<std::uint64_t>::max() - offsets[i]) / fixed)
        require(view.offset(i, fixed) == offsets[i] + ordinal * fixed, "fixed stride restoration");
      else rejects([&] { view.offset(i, fixed); });
    }
    rejects([&] { view.residual(offsets.size()); });
    return index;
  }

  void test_select15() {
    std::mt19937_64 random(0xef15);
    for (std::uint64_t records : std::initializer_list<std::uint64_t>{0, 1, 14, 15, 16, 239, 240, 241, 3824,
                                 3825, 3840, 3841, 131073}) {
      auto groups = records / 15 + (records % 15 != 0);
      for (unsigned pattern = 0; pattern < 5; ++pattern) {
        std::vector<std::uint64_t> offsets(groups + 1);
        for (std::size_t i = 1; i < offsets.size(); ++i) {
          std::uint64_t step = pattern == 0 ? 0 : pattern == 1 ? 1 :
                               pattern == 2 ? random() % 32 : pattern == 3 ? random() % 100000 :
                               (std::uint64_t{1} << 40) + random() % 32;
          offsets[i] = offsets[i - 1] + step;
        }
        check_select(offsets, records);
      }
    }
    // Concentrate a large gap inside a sampled group: bounded select must use
    // its sparse exception path instead of traversing thousands of zero bits.
    std::vector<std::uint64_t> skewed(10001);
    for (std::size_t i = 17; i < skewed.size(); ++i) skewed[i] = 20000;
    auto sparse = check_select(skewed, 150000);
    require(!sparse.sparse.empty(), "sparse select exception was not exercised");

    auto maximum = std::numeric_limits<std::uint64_t>::max();
    check_select({maximum}, 0, 0); // low width 63, no shift by 64.
    check_select({0, maximum}, 1, 0);
    check_select({maximum - 1, maximum}, 1, 8); // restored-offset overflow.
    check_select({0, 7}, 14, maximum); // stride multiplication overflow.
    everett::select15_index empty_index;
    auto empty = empty_index.view();
    require(empty.size() == 0 && empty.offset(0) == 0, "default select15");
    rejects([] { everett::select15_index::build({}, 0); });
    rejects([] { everett::select15_index::build(std::array<std::uint64_t, 2>{10, 0}, 1); });

    auto broken = everett::select15_index::build(std::array<std::uint64_t, 2>{0, 1}, 1);
    broken.high[0] = 0;
    rejects([&] { broken.view().residual(0); });
    broken = everett::select15_index::build(std::array<std::uint64_t, 2>{0, 1}, 1);
    broken.samples[0].sparse = 0;
    rejects([&] { broken.view().residual(0); });
    broken.samples[0].sparse = maximum;
    broken.samples[0].first = maximum;
    rejects([&] { broken.view().residual(0); });
    broken.high.clear();
    rejects([&] { broken.view(); });
  }
}

int main() {
  try {
    test_rank_directory();
    test_rank();
    test_rank15();
    test_select15();
    std::cout << "Storage rank, packed rank15, and Elias-Fano select15 oracle checks passed\n";
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
 * \brief Tests Everett's storage rank behavior.
 */
