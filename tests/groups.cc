/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Diet's groups behavior.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/rank_groups.h>
#include <diet/elias_fano.h>

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

  std::vector<std::byte> little_words(std::span<std::uint64_t const> words, unsigned offset = 0) {
    std::vector<std::byte> result(offset + words.size() * 8, std::byte{0x9d});
    for (std::size_t i = 0; i < words.size(); ++i)
      for (unsigned byte = 0; byte < 8; ++byte)
        result[offset + i * 8 + byte] = std::byte((words[i] >> (byte * 8)) & 255);
    return result;
  }

  template <class Sample> std::vector<std::byte> little_samples(std::span<Sample const> samples, unsigned offset) {
    std::vector<std::uint64_t> words;
    for (auto sample : samples) { words.push_back(sample.first); words.push_back(sample.sparse); }
    return little_words(words, offset);
  }

  void test_byte_views() {
    std::array<std::uint64_t, 5> source{0, 1, 0x0123456789abcdefull, 0xfedcba9876543210ull, ~std::uint64_t{0}};
    diet::word_view native(source);
    for (unsigned offset = 0; offset < 8; ++offset) {
      auto storage = little_words(source, offset);
      auto bytes = std::span(storage).subspan(offset);
      auto view = diet::word_view::little_endian(bytes);
      require(view.bytes().data() == bytes.data() && view.size() == source.size(), "word bytes retained");
      for (std::size_t i = 0; i < source.size(); ++i)
        require(view[i] == source[i] && native[i] == source[i], "word LE/native load");
      require(view.subspan(2, 1)[0] == source[2] && view.subspan(5).empty(), "word subspan");
      rejects([&] { (void)view[5]; });
      rejects([&] { (void)view.subspan(6); });
      rejects([&] { (void)view.subspan(4, 2); });
      rejects([&] { (void)diet::word_view::little_endian(bytes.first(7)); });
      std::array<diet::elias_fano_sample, 2> samples{{{source[2], source[3]}, {0, ~std::uint64_t{0}}}};
      auto encoded = little_samples(std::span<diet::elias_fano_sample const>(samples), offset);
      auto sample_bytes = std::span(encoded).subspan(offset);
      auto sample = diet::sample_view::little_endian(sample_bytes);
      diet::sample_view native_sample{std::span<diet::elias_fano_sample const>(samples)};
      require(sample.bytes().data() == sample_bytes.data() && sample.words().size() == 4, "sample bytes retained");
      for (std::size_t i = 0; i < samples.size(); ++i)
        require(sample[i].first == samples[i].first && sample[i].sparse == samples[i].sparse &&
                native_sample[i].first == samples[i].first && native_sample[i].sparse == samples[i].sparse,
                "sample LE/native load");
      rejects([&] { (void)sample[2]; });
      rejects([&] { (void)diet::sample_view::little_endian(sample_bytes.first(24)); });
    }
  }

  template <std::uint64_t K> void test_bad_rank_prefix() {
    auto index = diet::rank_groups<K>::build(std::array<std::uint64_t, 2>{1, 1}, 2 * K);
    index.checkpoints[0] = ~std::uint64_t{0};
    auto view = index.view(); // Shape checks must not read the checkpoint.
    rejects([&] { (void)view.rank(0); });
    rejects([&] { (void)view.rank(1); });
    index.checkpoints[0] = index.virtual_count;
    rejects([&] { (void)index.view().rank(1); });
    // The final physical group contains one entry, regardless of field width.
    std::array<std::uint64_t, 1> classes{K}, checkpoints{0};
    diet::rank_groups_view<K> partial(classes, checkpoints, 1);
    require(partial.rank(0) == 0, "partial first boundary");
    rejects([&] { (void)partial.count(); });
    rejects([&] { (void)partial.rank(1); });

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
        auto index = diet::rank_groups<K>::build(classes, count);
        auto view = index.view();
        require(view.size() == count && view.group_count() == groups && view.count() == oracle.back(), "rank groups dimensions");
        for (std::uint64_t i = 0; i < groups; ++i)
          require(view.rank(i) == oracle[i], "rank groups prefix oracle");
        for (std::uint64_t i = 0; i < groups; ++i)
          require(view.class_at(i) == classes[i], "rank groups class oracle");
        for (unsigned offset = 0; offset < 8; ++offset) {
          auto packed = little_words(index.classes, offset), checkpoints = little_words(index.checkpoints, offset);
          auto pc = diet::word_view::little_endian(std::span(packed).subspan(offset));
          auto cp = diet::word_view::little_endian(std::span(checkpoints).subspan(offset));
          diet::rank_groups_view<K> mapped(pc, cp, count);
          require(mapped.count() == oracle.back(), "mapped derived count");
          rejects([&] { (void)mapped.rank(groups); });
          require(mapped.class_words().bytes().data() == packed.data() + offset &&
                  mapped.checkpoint_words().bytes().data() == checkpoints.data() + offset, "rank sections retained");
          for (std::uint64_t i = 0; i < groups; ++i)
            require(mapped.rank(i) == oracle[i], "unaligned mapped rank oracle");
          for (std::uint64_t i = 0; i < groups; ++i)
            require(mapped.class_at(i) == classes[i], "unaligned mapped class oracle");
        }
        // Independent bit-at-a-time oracle checks packed fields crossing words.
        constexpr unsigned width = diet::rank_groups<K>::class_bits;
        for (std::uint64_t i = 0; i < groups; ++i)
          for (unsigned bit = 0; bit < width; ++bit) {
            auto position = i * width + bit;
            require(((index.classes[position / 64] >> (position % 64)) & 1) == ((classes[i] >> bit) & 1),
                    "rank class packed bit mismatch");
          }
        rejects([&] { view.rank(groups); });
        rejects([&] { view.class_at(groups); });
      }
    }
    rejects([] { diet::rank_groups<K>::build(std::array<std::uint64_t, 1>{K + 1}, K); });
    rejects([] { diet::rank_groups<K>::build(std::array<std::uint64_t, 1>{2}, 1); });
    rejects([] { diet::rank_groups<K>::build({}, K); });
    diet::rank_groups<K> empty;
    require(empty.view().count() == 0, "default rank groups");
    rejects([&] { (void)empty.view().rank(0); });
  }

  void test_rank15_agreement() {
    static_assert(diet::rank_groups_view<15>::group_size == 15);
    static_assert(diet::rank_groups_view<15>::class_bits == 4);
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
        auto policy = diet::rank_groups<15>::build(classes, count);
        auto fixed = diet::rank15_index::build(fixed_classes, count);
        require(policy.classes == fixed.classes && policy.checkpoints == fixed.checkpoints &&
                policy.view().count() == fixed.view().count(), "rank15 policy encoding agreement");
        auto view = policy.view();
        auto fixed_view = fixed.view();
        static_assert(std::is_same_v<decltype(view.class_at(0)), std::uint64_t>);
        require(view.size() == count && view.group_count() == groups && view.count() == oracle.back(),
                "rank15 policy view dimensions");
        for (std::uint64_t i = 0; i < groups; ++i) {
          require(view.rank(i) == oracle[i] && fixed_view.rank(i) == oracle[i],
                  "rank15 policy prefix agreement");
          if (i < groups) require(view.class_at(i) == classes[i], "rank15 policy class agreement");
        }
      }
    }
    rejects([] { diet::rank_groups_view<15>({}, {}, 15); });
    rejects([] {
      std::array<std::uint64_t, 1> classes{0};
      diet::rank_groups_view<15>(classes, {}, 15);
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
          diet::elias_fano_detail::pack_low(source, actual, width);
          require(actual == expected, "width-specialized packing changed wire words");
        }
    rejects([] { diet::elias_fano_detail::pack_low({}, {}, 64); });
    rejects([] { diet::elias_fano_detail::pack_low(std::array<std::uint64_t, 1>{1}, {}, 1); });
  }

  void check_encoding(diet::elias_fano const & index,
                      std::span<std::uint64_t const> source) {
    for (unsigned offset = 0; offset < 8; ++offset) {
      auto low = little_words(index.low, offset), high = little_words(index.high, offset);
      auto sparse = little_words(index.sparse, offset);
      auto samples = little_samples(std::span<diet::elias_fano_sample const>(index.samples), offset);
      auto lo = diet::word_view::little_endian(std::span(low).subspan(offset));
      auto hi = diet::word_view::little_endian(std::span(high).subspan(offset));
      auto sp = diet::word_view::little_endian(std::span(sparse).subspan(offset));
      auto sm = diet::sample_view::little_endian(std::span(samples).subspan(offset));
      diet::elias_fano_view mapped(lo, hi, sm, sp, index.entry_count, index.universe, index.low_width);
      require(mapped.low_words().bytes().data() == low.data() + offset &&
              mapped.high_words().bytes().data() == high.data() + offset &&
              mapped.samples().bytes().data() == samples.data() + offset &&
              mapped.sparse_words().bytes().data() == sparse.data() + offset,
              "select sections retained");
      require(mapped.universe() == index.universe && mapped.low_width() == index.low_width, "select scalar metadata");
      require(mapped.size() == source.size(), "mapped EF entry count");
      for (std::size_t i = 0; i < source.size(); ++i)
        require(mapped.select(i) == source[i], "unaligned mapped select oracle");
      rejects([&] { mapped.select(source.size()); });
    }
    if (source.empty()) {
      require(index.entry_count == 0 && index.universe == 0 && index.low_width == 0 &&
              index.low.empty() && index.high.empty() && index.samples.empty() && index.sparse.empty(),
              "canonical empty EF encoding");
      return;
    }
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

  diet::elias_fano check_select(std::vector<std::uint64_t> const & source) {
    auto index = diet::elias_fano::build(source);
    check_encoding(index, source);
    auto view = index.view();
    require(view.size() == source.size(), "EF entry count");
    for (std::uint64_t i = 0; i < source.size(); ++i)
      require(view.select(i) == source[i], "EF value mismatch");
    rejects([&] { view.select(source.size()); });
    return index;
  }

  void test_select() {
    std::mt19937_64 random(0xef);
    for (std::uint64_t count : {0, 1, 2, 3, 14, 15, 16, 17, 31, 32, 33, 255, 256, 257, 258, 10002}) {
      for (unsigned pattern = 0; pattern < 4; ++pattern) {
        std::vector<std::uint64_t> source(count);
        for (std::size_t i = 1; i < source.size(); ++i)
          source[i] = source[i - 1] + (pattern == 0 ? 0 : pattern == 1 ? 1 :
                          pattern == 2 ? random() % 32 : (std::uint64_t{1} << 40) + random() % 32);
        check_select(source);
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
        auto index = check_select(residuals);
        require(index.low_width == width, "valid full-codec width fixture");
      }
    }
    // A decrease in every lane and every short vector remainder is rejected,
    // including unsigned values on either side of the sign bit.
    for (unsigned size = 2; size != 34; ++size)
      for (unsigned at = 1; at != size; ++at) {
        std::vector<std::uint64_t> source(size, std::uint64_t{1} << 63);
        source[at] -= 1;
        rejects([&] { diet::elias_fano::build(source); });
      }
    std::vector<std::uint64_t> skewed(10001);
    for (std::size_t i = 17; i < skewed.size(); ++i) skewed[i] = 20000;
    auto sparse = check_select(skewed);
    require(!sparse.sparse.empty(), "generic sparse select not exercised");
    check_select({maximum});
    check_select({0, maximum});
    check_select({0, 7});
    rejects([] { diet::elias_fano::build(std::array<std::uint64_t, 2>{1, 0}); });
    diet::elias_fano empty;
    require(empty.view().size() == 0, "default EF has no sentinel");
    rejects([&] { empty.view().select(0); });
  }

  void check_high_words(std::span<std::uint64_t const> high, std::uint64_t bit_count) {
    std::vector<std::uint64_t> positions;
    for (std::uint64_t bit = 0; bit != bit_count; ++bit)
      if ((high[bit / 64] >> (bit % 64)) & 1) positions.push_back(bit);
    require(!positions.empty() && positions.size() <= 256, "high-word fixture shape");
    std::array<diet::elias_fano_sample, 1> samples{{{positions.front(), ~std::uint64_t{0}}}};
    auto universe = bit_count - positions.size();
    diet::elias_fano_view view({}, high, samples, {}, positions.size(), universe, 0);
    for (std::size_t i = 0; i != positions.size(); ++i) {
      auto expected = positions[i] - i;
      require(view.select(i) == expected,
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
        diet::elias_fano_detail::pack_low(input, output, width);
        require(std::equal(output.begin(), output.end(), expected.begin(), expected.end()), "guarded low packing");
      }
    for (unsigned entries = 1; entries != 34; ++entries) {
      std::vector<std::uint64_t> source(entries);
      for (unsigned i = 0; i != entries; ++i) source[i] = i * 256;
      auto bounded = low_guard.copy(source);
      auto index = diet::elias_fano::build(bounded);
      check_encoding(index, source);
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
      auto index = diet::elias_fano::build(source);
      auto low = low_guard.copy(index.low), high = high_guard.copy(index.high);
      diet::elias_fano_view view(low, high, index.samples, index.sparse, entries, universe, width);
      for (std::size_t i = 0; i != source.size(); ++i)
        require(view.select(i) == source[i], "guarded EF low/high tail");
    }
#endif
  }

  template <std::uint64_t K> void test_mapped_rank_guards() {
#if defined(__unix__) || defined(__APPLE__)
    auto page = std::size_t(sysconf(_SC_PAGESIZE));
    auto raw = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(raw != MAP_FAILED, "mapped rank guard allocation");
    struct cleanup {
      void * data;
      std::size_t bytes;
      ~cleanup() { munmap(data, bytes); }
    } guard{raw, page * 2};
    auto end = static_cast<std::byte *>(raw) + page;
    require(mprotect(end, page, PROT_NONE) == 0, "mapped rank tail guard");
    std::mt19937_64 random(0xfeed + K);
    for (std::uint64_t groups = 0; groups <= 260; ++groups) {
      auto count = groups ? (groups - 1) * K + 1 : 0;
      std::vector<std::uint64_t> source(groups), oracle(groups + 1);
      for (std::uint64_t i = 0; i < groups; ++i) {
        source[i] = i + 1 == groups ? 1 : random() % (K + 1);
        oracle[i + 1] = oracle[i] + source[i];
      }
      auto index = diet::rank_groups<K>::build(source, count);
      auto encoded = little_words(index.classes);
      require(encoded.size() <= page, "mapped rank fits guard page");
      auto start = end - encoded.size();
      std::copy(encoded.begin(), encoded.end(), start);
      std::span<std::byte const> bytes(start, encoded.size());
      auto words = diet::word_view::little_endian(bytes);
      auto checkpoints = diet::word_view(index.checkpoints);
      require(mprotect(raw, page, PROT_NONE) == 0, "mapped rank shape guard");
      diet::rank_groups_view<K> view(words, checkpoints, count);
      require(view.class_words().bytes().data() == start, "mapped rank shape retains pointer");
      rejects([&] { (void)view.rank(groups); });
      require(mprotect(raw, page, PROT_READ | PROT_WRITE) == 0, "mapped rank shape unprotect");
      require(view.count() == oracle.back(), "guarded mapped derived count");
      for (std::uint64_t i = 0; i < groups; ++i) {
        require(view.rank(i) == oracle[i], "guarded mapped rank oracle");
        if (i < groups) require(view.class_at(i) == source[i], "guarded mapped class oracle");
      }
    }
#endif
  }

  void test_wide_policy() {
    constexpr std::uint64_t k = (std::uint64_t{1} << 63) - 1;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto index = diet::rank_groups<k>::build(std::array<std::uint64_t, 3>{k, k, 1}, maximum);
    require(index.view().rank(2) == maximum - 1 && index.view().count() == maximum,
            "wide group counter overflow");
    require(index.view().class_at(1) == k && index.view().class_at(2) == 1, "63-bit packed classes");
  }
}

int main() {
  try {
    rejects([] {
      diet::elias_fano_view({}, {}, {}, {}, ~std::uint64_t{0}, 0, 0);
    });
    test_byte_views();
    test_bad_rank_prefix<3>(); test_bad_rank_prefix<7>(); test_bad_rank_prefix<15>();
    test_bad_rank_prefix<31>(); test_bad_rank_prefix<63>();
    test_low_packing();
    test_word_select();
    test_select_guard_pages();
    test_rank<3>(); test_rank<7>(); test_rank<15>(); test_rank<31>();
    test_rank15_agreement();
    test_select();
    test_mapped_rank_guards<3>(); test_mapped_rank_guards<7>();
    test_mapped_rank_guards<15>(); test_mapped_rank_guards<31>();
    test_wide_policy();
    std::cout << "Policy groups 3/7/15/31, packed classes, and generic Elias-Fano checks passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
