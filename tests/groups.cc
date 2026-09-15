/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/rank_groups.h>
#include <everett/select_groups.h>
#include <everett/select15.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <random>
#include <stdexcept>
#include <type_traits>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

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

  void test_rank15_agreement() {
    static_assert(everett::rank_groups_view<15>::group_size == 15);
    static_assert(everett::rank_groups_view<15>::class_bits == 4);
    std::mt19937_64 random(0x151528);
    // Keep a full checkpoint before every possible final checkpoint length,
    // including partial virtual groups. This covers vector reductions and
    // bounded short-span fallbacks through the policy-sized view API.
    for (std::uint64_t tail = 0; tail <= 128 * 15; ++tail) {
      auto count = 128 * 15 + tail;
      auto groups = count / 15 + (count % 15 != 0);
      for (unsigned pattern = 0; pattern < 3; ++pattern) {
        std::vector<std::uint64_t> classes(groups);
        std::vector<std::uint8_t> fixed_classes(groups);
        std::vector<std::uint64_t> oracle(groups + 1);
        for (std::uint64_t i = 0; i < groups; ++i) {
          auto capacity = i + 1 == groups && count % 15 ? count % 15 : 15;
          classes[i] = pattern == 0 ? 0 : pattern == 1 ? capacity : random() % (capacity + 1);
          fixed_classes[i] = std::uint8_t(classes[i]);
          oracle[i + 1] = oracle[i] + classes[i];
        }
        auto policy = everett::rank_groups<15>::build(classes, count);
        auto fixed = everett::rank15_index::build(fixed_classes, count);
        require(policy.classes == fixed.classes && policy.checkpoints == fixed.checkpoints &&
                policy.total == fixed.total, "rank15 policy encoding agreement");
        auto view = policy.view();
        auto fixed_view = fixed.view();
        static_assert(std::is_same_v<decltype(view.class_at(0)), std::uint64_t>);
        require(view.size() == count && view.group_count() == groups && view.count() == oracle.back(),
                "rank15 policy view dimensions");
        for (std::uint64_t i = 0; i <= groups; ++i) {
          require(view.rank(i) == oracle[i] && fixed_view.rank(i) == oracle[i],
                  "rank15 policy prefix agreement");
          if (i < groups) require(view.class_at(i) == classes[i], "rank15 policy class agreement");
        }
      }
    }
    rejects([] { everett::rank_groups_view<15>({}, {}, 15, 0); });
    rejects([] { everett::rank_groups_view<15>({}, {}, 0, 1); });
    rejects([] {
      std::array<std::uint64_t, 1> classes{0};
      everett::rank_groups_view<15>(classes, {}, 15, 0);
    });
  }

  std::vector<std::uint64_t> low_oracle(std::span<std::uint64_t const> source, unsigned width) {
    auto bits = source.size() * width;
    std::vector<std::uint64_t> result(bits / 64 + (bits % 64 != 0));
    // Independent bit-at-a-time oracle: no word packing, tile formula or
    // optimized helper participates in the expected representation.
    for (std::size_t i = 0; i != source.size(); ++i)
      for (unsigned bit = 0; bit != width; ++bit)
        if ((source[i] >> bit) & 1) {
          auto at = i * width + bit;
          result[at / 64] |= std::uint64_t{1} << (at % 64);
        }
    return result;
  }

  void test_low_packing() {
    std::mt19937_64 random(0x10ef);
    for (unsigned width = 0; width != 64; ++width)
      for (unsigned count : {0, 1, 2, 3, 7, 15, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257})
        for (unsigned pattern = 0; pattern != 3; ++pattern) {
          std::vector<std::uint64_t> source(count);
          for (auto & value : source)
            value = pattern == 0 ? 0 : pattern == 1 ? std::numeric_limits<std::uint64_t>::max() : random();
          auto expected = low_oracle(source, width);
          // Nonzero old contents also check that tail words are assigned and
          // padding cleared, rather than being accidentally ORed with old data.
          std::vector<std::uint64_t> actual(expected.size(), ~std::uint64_t{0});
          everett::select_groups_detail::pack_low(source, actual, width);
          require(actual == expected, "width-specialized packing changed wire words");
        }
    rejects([] { everett::select_groups_detail::pack_low({}, {}, 64); });
    rejects([] { everett::select_groups_detail::pack_low(std::array<std::uint64_t, 1>{1}, {}, 1); });
    rejects([] {
      everett::select_groups_detail::multiply(std::numeric_limits<std::uint64_t>::max(), 2);
    });
    rejects([] {
      everett::select_groups_detail::add(std::numeric_limits<std::uint64_t>::max(), 1);
    });
  }

  template <std::uint64_t K> void check_encoding(everett::select_groups<K> const & index,
                                               std::span<std::uint64_t const> source) {
    unsigned width = 0;
    for (auto quotient = source.back() / source.size(); quotient > 1; quotient >>= 1) ++width;
    require(index.low_width == width && index.universe == source.back(), "EF width/universe changed");
    require(index.low == low_oracle(source, width), "EF low word oracle");
    auto bits = (source.back() >> width) + source.size();
    std::vector<std::uint64_t> high(bits / 64 + (bits % 64 != 0));
    std::vector<std::uint64_t> positions;
    for (std::size_t i = 0; i != source.size(); ++i) {
      auto position = (source[i] >> width) + i;
      positions.push_back(position);
      high[position / 64] |= std::uint64_t{1} << (position % 64);
    }
    require(index.high == high, "EF high word oracle including zero gaps/padding");
    std::size_t samples = 0;
    std::vector<std::uint64_t> sparse;
    for (std::size_t first = 0; first < positions.size(); first += 256) {
      auto end = std::min(positions.size(), first + 256);
      auto sparse_at = std::numeric_limits<std::uint64_t>::max();
      if (positions[end - 1] - positions[first] >= 4096) {
        sparse_at = sparse.size();
        sparse.insert(sparse.end(), positions.begin() + first, positions.begin() + end);
      }
      require(samples < index.samples.size() && index.samples[samples].first == positions[first] &&
              index.samples[samples].sparse == sparse_at, "EF sample/exception representation");
      ++samples;
    }
    require(samples == index.samples.size() && sparse == index.sparse, "EF sparse word oracle");
  }

  template <std::uint64_t K> everett::select_groups<K> check_select(
      std::vector<std::uint64_t> const & residuals, std::uint64_t count) {
    auto index = everett::select_groups<K>::build(residuals, count);
    check_encoding(index, residuals);
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
    // A large input cannot have width63 in a uint64 universe. Shrink the
    // fixture for high widths so every case is an actual valid EF instance.
    auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (unsigned width = 0; width != 64; ++width) {
      std::uint64_t samples = 129;
      while (samples > (maximum >> width)) samples = (samples + 1) / 2;
      auto universe = samples << width;
      for (unsigned pattern = 0; pattern != 3; ++pattern) {
        std::vector<std::uint64_t> residuals(samples);
        if (samples == 1) residuals[0] = universe;
        else for (std::uint64_t i = 0; i != samples; ++i) {
          auto at = pattern == 1 && i + 1 != samples ? (i / 8) * 8 : i;
          residuals[i] = pattern == 2 ? (i < samples / 2 ? 0 : universe) :
            (universe / (samples - 1)) * at + ((universe % (samples - 1)) * at) / (samples - 1);
        }
        auto count = samples == 1 ? 0 : (samples - 1) * K - (K - 1);
        auto index = check_select<K>(residuals, count);
        require(index.low_width == width, "valid full-codec width fixture");
      }
    }
    // A decrease in every lane and every short vector remainder is rejected,
    // including unsigned values on either side of the sign bit.
    for (unsigned size = 2; size != 34; ++size)
      for (unsigned at = 1; at != size; ++at) {
        std::vector<std::uint64_t> source(size, std::uint64_t{1} << 63);
        source[at] -= 1;
        rejects([&] { everett::select_groups<K>::build(source, (size - 1) * K); });
        rejects([&] { everett::select15_index::build(source, (size - 1) * 15); });
      }
    std::vector<std::uint64_t> skewed(10001);
    for (std::size_t i = 17; i < skewed.size(); ++i) skewed[i] = 20000;
    auto sparse = check_select<K>(skewed, 10000 * K);
    require(!sparse.sparse.empty(), "generic sparse select not exercised");
    check_select<K>({maximum}, 0);
    check_select<K>({0, maximum}, 1);
    auto tiny = check_select<K>({0, 7}, K - 1);
    rejects([&] { tiny.view().offset(1, maximum); });
    rejects([] { everett::select_groups<K>::build({}, 0); });
    rejects([] { everett::select_groups<K>::build(std::array<std::uint64_t, 2>{1, 0}, 1); });
    everett::select_groups<K> empty;
    require(empty.view().offset(0, 13) == 0, "default select groups");
  }

  void check_high_words(std::span<std::uint64_t const> high, std::uint64_t bit_count) {
    std::vector<std::uint64_t> positions;
    for (std::uint64_t bit = 0; bit != bit_count; ++bit)
      if ((high[bit / 64] >> (bit % 64)) & 1) positions.push_back(bit);
    require(!positions.empty() && positions.size() <= 256, "high-word fixture shape");
    std::array<everett::select_groups_sample, 1> samples{{{positions.front(), ~std::uint64_t{0}}}};
    std::array<everett::select15_sample, 1> fixed_samples{{{positions.front(), ~std::uint64_t{0}}}};
    auto records = (positions.size() - 1) * 15;
    auto universe = bit_count - positions.size();
    everett::select_groups_view<15> view({}, high, samples, {}, records, universe, 0);
    everett::select15_view fixed({}, high, fixed_samples, {}, records, universe, 0);
    for (std::size_t i = 0; i != positions.size(); ++i) {
      auto expected = positions[i] - i;
      require(view.residual(i) == expected && fixed.residual(i) == expected,
              "independent within-word select oracle");
    }
  }

  void test_word_select() {
    // Every 16-bit bitmap in each quarter, all 64-bit selection ordinals,
    // and nonuniform full-word populations use the public EF views.
    for (unsigned pattern = 1; pattern != 65536; ++pattern)
      for (unsigned shift : {0, 16, 32, 48}) {
        std::array<std::uint64_t, 1> high{std::uint64_t(pattern) << shift};
        check_high_words(high, 64);
      }
    std::mt19937_64 random(0x5e1ec7);
    for (unsigned i = 0; i != 1024; ++i) {
      std::array<std::uint64_t, 1> high{i < 64 ? ~(std::uint64_t{1} << i) : random() | 1};
      check_high_words(high, 64);
    }
    std::array<std::uint64_t, 1> full{~std::uint64_t{0}};
    check_high_words(full, 64);
  }

  void test_select15_encoding() {
    std::mt19937_64 random(0x15ef);
    for (unsigned count : {0, 1, 14, 15, 16, 255 * 15, 256 * 15, 10000 * 15})
      for (unsigned pattern = 0; pattern != 4; ++pattern) {
        std::vector<std::uint64_t> source(count / 15 + (count % 15 != 0) + 1);
        for (std::size_t i = 1; i != source.size(); ++i)
          source[i] = source[i - 1] + (pattern == 0 ? 0 : pattern == 1 ? 1 :
            pattern == 2 ? random() % 10000 : (i == 17 ? 20000 : 0));
        auto general = check_select<15>(source, count);
        auto fixed = everett::select15_index::build(source, count);
        require(fixed.low == general.low && fixed.high == general.high && fixed.sparse == general.sparse &&
                fixed.universe == general.universe && fixed.low_width == general.low_width &&
                fixed.samples.size() == general.samples.size(), "select15 exact encoded layout");
        for (std::size_t i = 0; i != fixed.samples.size(); ++i)
          require(fixed.samples[i].first == general.samples[i].first &&
                  fixed.samples[i].sparse == general.samples[i].sparse, "select15 sample layout");
        auto view = fixed.view();
        for (std::size_t i = 0; i != source.size(); ++i)
          require(view.residual(i) == source[i], "select15 residual construction oracle");
      }
  }

  void test_select_guard_pages() {
#if defined(__unix__) || defined(__APPLE__)
    struct guarded_words {
      std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
      void * mapping = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      guarded_words() {
        require(mapping != MAP_FAILED, "select guard mapping failed");
        require(mprotect(static_cast<char *>(mapping) + page, page, PROT_NONE) == 0, "select guard protection failed");
      }
      ~guarded_words() { munmap(mapping, 2 * page); }
      std::span<std::uint64_t> copy(std::span<std::uint64_t const> words) {
        require(words.size_bytes() <= page, "select guard fixture too large");
        auto end = reinterpret_cast<std::uint64_t *>(static_cast<char *>(mapping) + page);
        std::span<std::uint64_t> result(end - words.size(), words.size());
        std::copy(words.begin(), words.end(), result.begin());
        return result;
      }
    } low_guard, high_guard;
    // Complete SIMD packing tiles and every short remainder end at guards
    // on both sides; old destination contents must be fully overwritten.
    for (unsigned count = 0; count != 34; ++count)
      for (unsigned width : {0, 1, 7, 8, 16, 32, 63}) {
        std::vector<std::uint64_t> source(count);
        for (unsigned i = 0; i != count; ++i) source[i] = ~std::uint64_t{0} - i * 0x102030405ull;
        auto expected = low_oracle(source, width);
        std::vector<std::uint64_t> old(expected.size(), ~std::uint64_t{0});
        auto input = low_guard.copy(source), output = high_guard.copy(old);
        everett::select_groups_detail::pack_low(input, output, width);
        require(std::equal(output.begin(), output.end(), expected.begin(), expected.end()), "guarded low packing");
      }
    for (unsigned entries = 1; entries != 34; ++entries) {
      std::vector<std::uint64_t> source(entries);
      for (unsigned i = 0; i != entries; ++i) source[i] = i * 256;
      auto bounded = low_guard.copy(source);
      auto records = (entries - 1) * 15;
      auto general = everett::select_groups<15>::build(bounded, records);
      auto fixed = everett::select15_index::build(bounded, records);
      require(general.low == fixed.low && general.high == fixed.high, "guarded constructor source");
    }
    // Exact 1..65-word spans end at an inaccessible page. The 65-word
    // fixture starts at bit63 and ends4095 bits later, the dense limit.
    for (unsigned words = 1; words <= 65; ++words) {
      std::vector<std::uint64_t> high(words, std::uint64_t{1} << 63);
      if (words == 65) high.back() = std::uint64_t{1} << 62;
      check_high_words(high_guard.copy(high), words * 64 - unsigned(words == 65));
    }
    auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (unsigned width = 0; width != 64; ++width) {
      std::uint64_t entries = 129;
      while (entries > (maximum >> width)) entries = (entries + 1) / 2;
      auto universe = entries << width;
      std::vector<std::uint64_t> source(entries);
      if (entries == 1) source[0] = universe;
      else for (std::uint64_t i = 0; i != entries; ++i)
        source[i] = (universe / (entries - 1)) * i + ((universe % (entries - 1)) * i) / (entries - 1);
      auto records = entries == 1 ? 0 : (entries - 1) * 15 - 14;
      auto index = everett::select_groups<15>::build(source, records);
      auto low = low_guard.copy(index.low), high = high_guard.copy(index.high);
      everett::select_groups_view<15> view(low, high, index.samples, index.sparse, records, universe, width);
      std::vector<everett::select15_sample> fixed_samples;
      for (auto sample : index.samples) fixed_samples.push_back({sample.first, sample.sparse});
      everett::select15_view fixed(low, high, fixed_samples, index.sparse, records, universe, width);
      for (std::size_t i = 0; i != source.size(); ++i)
        require(view.residual(i) == source[i] && fixed.residual(i) == source[i], "guarded EF low/high tail");
    }
#endif
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
    test_low_packing();
    test_word_select();
    test_select15_encoding();
    test_select_guard_pages();
    test_rank<3>(); test_rank<7>(); test_rank<15>(); test_rank<31>();
    test_rank15_agreement();
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
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's groups behavior.
 */
