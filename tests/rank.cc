/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/rank.h>
#include <everett/rank15.h>
#include <everett/rank_groups.h>
#include <everett/select15.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <random>
#include <stdexcept>
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
    require(threw, "invalid input was accepted");
  }

  void test_popcount512() {
    std::array<std::uint64_t, 8> words{};
    for (unsigned count = 0; count <= 512; ++count) {
      require(everett::rank_detail::popcount512(words.data()) == count, "512-bit population");
      require(everett::rank_detail::popcount512_portable(words.data()) == count, "portable 512-bit population");
      if (count != 512) words[count / 64] |= std::uint64_t{1} << (count % 64);
    }
    // Alignment is only that of uint64_t, not a SIMD register or cache line.
    alignas(64) std::array<std::uint64_t, 15> shifted{};
    std::mt19937_64 random(0x512);
    for (auto & word : shifted) word = random();
    for (unsigned offset = 0; offset < 8; ++offset) {
      unsigned expected = 0;
      for (unsigned bit = 0; bit < 512; ++bit)
        expected += unsigned((shifted[offset + bit / 64] >> (bit % 64)) & 1);
      require(everett::rank_detail::popcount512(shifted.data() + offset) == expected,
              "unaligned 512-bit population");
      require(everett::rank_detail::popcount512_portable(shifted.data() + offset) == expected,
              "unaligned portable 512-bit population");
    }
  }

  void test_prefix512() {
    alignas(64) std::array<std::uint64_t, 15> words{};
    std::mt19937_64 random(0x512f00d);
    for (unsigned pattern = 0; pattern < 66; ++pattern) {
      for (auto & word : words) word = pattern == 0 ? 0 : pattern == 1 ? ~std::uint64_t{0} : random();
      for (unsigned offset = 0; offset < 8; ++offset) {
        unsigned expected = 0;
        for (unsigned bit = 0; bit <= 512; ++bit) {
          require(everett::rank_detail::prefix512_portable(words.data() + offset, bit) == expected,
                  "portable 512-bit prefix oracle");
#if defined(__aarch64__) && defined(__ARM_NEON)
          require(everett::rank_detail::prefix512_neon(words.data() + offset, bit) == expected,
                  "NEON 512-bit prefix oracle");
#endif
          if (bit != 512) expected += unsigned((words[offset + bit / 64] >> (bit % 64)) & 1);
        }
      }
    }
  }

  template <unsigned K> void check_group_prefixes(std::span<std::uint64_t const> words,
      std::span<std::uint64_t const> populations, std::uint64_t virtual_count) {
    constexpr unsigned bits = everett::rank_groups<K>::class_bits;
    std::vector<std::uint64_t> oracle(populations.size() + 1), checkpoints;
    for (std::size_t i = 0; i < populations.size(); ++i) {
      if (i % 128 == 0) checkpoints.push_back(oracle[i]);
      oracle[i + 1] = oracle[i] + populations[i];
    }
    everett::rank_groups_view<K> view(words, checkpoints, virtual_count, oracle.back());
    for (std::size_t i = 0; i <= populations.size(); ++i) {
      require(view.rank(i) == oracle[i], "group prefix oracle");
      if (i < populations.size()) {
        require(view.class_at(i) == populations[i], "group class oracle");
        if constexpr (K == 3 || K == 7 || K == 31) {
          auto begin = (i / 128) * (2 * bits);
          // count=0 reads no words, including an empty span's protected address.
          auto expected = unsigned(oracle[i] - oracle[(i / 128) * 128]);
          require(everett::rank_groups_detail::prefix_portable<bits>(words.data() + begin, unsigned(i % 128)) == expected,
                  "portable grouped prefix oracle");
#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
          if (words.size() - begin >= 2 * bits)
            require(everett::rank_groups_detail::prefix_neon<bits>(words.data() + begin, unsigned(i % 128)) == expected,
                    "NEON grouped prefix oracle");
#endif
        }
      }
    }
  }

  template <unsigned K> void test_group_prefixes() {
    std::mt19937_64 random(0x345);
    constexpr unsigned bits = everett::rank_groups<K>::class_bits;
    // Short final virtual groups, short checkpoints, crossings of packed word
    // boundaries, and every word alignment within a cache line.
    for (unsigned groups = 0; groups <= 260; ++groups) {
      for (unsigned pattern = 0; pattern < 3; ++pattern) {
        auto count = groups ? (groups - 1) * K + 1 + groups % K : 0;
        std::vector<std::uint64_t> populations(groups);
        for (unsigned i = 0; i < groups; ++i) {
          auto limit = i + 1 == groups ? count - i * K : K;
          populations[i] = pattern == 0 ? 0 : pattern == 1 ? limit : random() % (limit + 1);
        }
        auto index = everett::rank_groups<K>::build(populations, count);
        // Padding is not a population and must not contribute to a prefix.
        if (groups * bits % 64) index.classes.back() |= ~std::uint64_t{0} << (groups * bits % 64);
        for (unsigned offset = 0; offset < 8; ++offset) {
          std::vector<std::uint64_t> shifted(index.classes.size() + offset);
          std::copy(index.classes.begin(), index.classes.end(), shifted.begin() + offset);
          check_group_prefixes<K>(std::span(shifted).subspan(offset), populations, count);
        }
      }
    }
    // Isolate every class bit and its complement to catch the rotating weights
    // where three- and five-bit fields cross a word or vector boundary.
    for (unsigned lane = 0; lane < 128; ++lane)
      for (unsigned bit = 0; bit < bits; ++bit)
        for (bool complement : {false, true}) {
          std::array<std::uint64_t, 128> populations{};
          populations.fill(complement ? K : 0);
          populations[lane] ^= std::uint64_t{1} << bit;
          auto index = everett::rank_groups<K>::build(populations, 128 * K);
          check_group_prefixes<K>(index.classes, populations, 128 * K);
        }
  }

  void test_rank_tails() {
    std::mt19937_64 random(0x2048512);
    for (std::uint64_t tail = 0; tail < 2048; ++tail) {
      auto bits = 3 * 2048 + tail;
      std::vector<std::uint64_t> source(bits / 64 + (bits % 64 != 0));
      for (auto & word : source) word = random();
      if (bits % 64) source.back() |= ~std::uint64_t{0} << (bits % 64);
      std::vector<std::uint64_t> oracle(bits + 1);
      for (std::uint64_t bit = 0; bit < bits; ++bit)
        oracle[bit + 1] = oracle[bit] + ((source[bit / 64] >> (bit % 64)) & 1);
      auto index = everett::rank_index::build(source, bits);
      require(index.total == oracle.back(), "partial block total");
      require(index.supers == std::vector<std::uint64_t>{0}, "partial block epoch directory");
      for (std::uint64_t block = 0; block < index.blocks.size(); ++block) {
        require(index.blocks[block].before == oracle[block * 2048], "partial block prefix");
        for (unsigned run = 0; run < 3; ++run) {
          auto first = std::min(bits, block * 2048 + run * 512);
          auto last = std::min(bits, block * 2048 + (run + 1) * 512);
          require(((index.blocks[block].runs >> (run * 10)) & 1023u) == oracle[last] - oracle[first],
                  "partial block run population");
        }
      }
      require(index.view().rank(bits) == oracle.back(), "partial block endpoint");
      if (bits % 64) require(index.words.back() >> (bits % 64) == 0, "partial word masking");
    }
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

  void check_rank15_word(std::uint64_t value) {
    std::array<std::uint64_t, 2> words{value, 0};
    std::array<std::uint64_t, 1> checkpoints{0};
    std::array<unsigned, 17> oracle{};
    for (unsigned i = 0; i < 16; ++i)
      oracle[i + 1] = oracle[i] + unsigned((value >> (4 * i)) & 15);
    // The extra group keeps rank(16) on the packed-word path, rather than
    // returning the endpoint's stored total. Earlier queries mask every tail.
    everett::rank15_view view(words, checkpoints, 17 * 15, oracle.back());
    for (unsigned i = 0; i <= 16; ++i)
      require(view.rank(i) == oracle[i], "rank15 packed-word sum");
  }

  void test_rank15_words() {
    auto maximum = std::numeric_limits<std::uint64_t>::max();
    check_rank15_word(0);
    check_rank15_word(maximum); // Sixteen classes of 15 must sum to 240.
    check_rank15_word(0x0f0f0f0f0f0f0f0full);
    check_rank15_word(0xf0f0f0f0f0f0f0f0ull);
    for (unsigned lane = 0; lane < 16; ++lane)
      for (std::uint64_t value = 0; value < 16; ++value) {
        auto mask = std::uint64_t{15} << (4 * lane);
        check_rank15_word(value << (4 * lane));
        check_rank15_word((maximum & ~mask) | (value << (4 * lane)));
      }
    std::mt19937_64 random(0x15f015);
    for (unsigned i = 0; i < 8192; ++i) check_rank15_word(random());
    // Exercise accumulation through an entire checkpoint, including maximum
    // byte-lane totals and uniformly random populations instead of bit flips.
    for (unsigned pattern = 0; pattern < 258; ++pattern) {
      std::array<std::uint64_t, 9> words{};
      for (unsigned i = 0; i < 8; ++i)
        words[i] = pattern == 0 ? 0 : pattern == 1 ? maximum : random();
      std::array<std::uint64_t, 130> oracle{};
      for (unsigned i = 0; i < 129; ++i)
        oracle[i + 1] = oracle[i] + ((words[i / 16] >> (4 * (i % 16))) & 15);
      std::array<std::uint64_t, 2> checkpoints{0, oracle[128]};
      everett::rank15_view view(words, checkpoints, 129 * 15, oracle.back());
      for (unsigned i = 0; i <= 129; ++i)
        require(view.rank(i) == oracle[i], "rank15 checkpoint accumulation");
    }
    // Every short final checkpoint must remain within the allocated words.
    // The first checkpoint of the larger inputs exercises the SIMD path.
    for (unsigned groups = 1; groups <= 256; ++groups) {
      std::vector<std::uint8_t> classes(groups);
      std::vector<std::uint64_t> oracle(groups + 1);
      for (unsigned i = 0; i < groups; ++i) {
        classes[i] = random() & 15;
        oracle[i + 1] = oracle[i] + classes[i];
      }
      auto index = everett::rank15_index::build(classes, groups * 15);
      auto view = index.view();
      for (unsigned i = 0; i <= groups; ++i)
        require(view.rank(i) == oracle[i], "rank15 short checkpoint");
    }
    // Borrowed classes need only uint64_t alignment, even for vector loads.
    alignas(64) std::array<std::uint64_t, 16> shifted{};
    for (auto & word : shifted) word = random();
    for (unsigned offset = 0; offset < 8; ++offset) {
      std::array<std::uint64_t, 129> oracle{};
      for (unsigned i = 0; i < 128; ++i)
        oracle[i + 1] = oracle[i] + ((shifted[offset + i / 16] >> (4 * (i % 16))) & 15);
      std::array<std::uint64_t, 1> checkpoints{0};
      everett::rank15_view view(std::span(shifted).subspan(offset, 8), checkpoints, 128 * 15, oracle.back());
      for (unsigned i = 0; i <= 128; ++i)
        require(view.rank(i) == oracle[i], "rank15 unaligned checkpoint");
    }
  }

  void check_rank15_prefixes(std::span<std::uint64_t const> words,
                            std::span<std::uint8_t const> populations) {
    // Build the oracle from the original population array, independently of
    // packed-word extraction, vector masks and horizontal reductions.
    std::vector<std::uint64_t> oracle(populations.size() + 1);
    std::vector<std::uint64_t> checkpoints;
    for (std::size_t i = 0; i < populations.size(); ++i) {
      if (i % 128 == 0) checkpoints.push_back(oracle[i]);
      oracle[i + 1] = oracle[i] + populations[i];
    }
    everett::rank15_view view(words, checkpoints, populations.size() * 15, oracle.back());
    for (std::size_t i = 0; i <= populations.size(); ++i)
      require(view.rank(i) == oracle[i], "rank15 targeted prefix oracle");
    for (std::size_t i = 0; i < populations.size(); ++i)
      require(view.class_at(i) == populations[i], "rank15 targeted class oracle");
  }

  void test_rank15_lanes() {
    // Isolate both nybbles of every byte in all four 128-bit vectors, including
    // the transitions at 32, 64 and 96 classes. Complements also exercise the
    // maximum prefix populations already covered by the all-ones fixtures.
    for (unsigned lane = 0; lane < 128; ++lane)
      for (unsigned active : {1u, 15u})
        for (bool complement : {false, true}) {
          std::array<std::uint8_t, 128> populations;
          populations.fill(complement ? 15 : 0);
          populations[lane] = std::uint8_t(complement ? 15 - active : active);
          std::array<std::uint64_t, 8> words{};
          for (unsigned i = 0; i < populations.size(); ++i)
            words[i / 16] |= std::uint64_t(populations[i]) << (4 * (i % 16));
          check_rank15_prefixes(words, populations);
        }
  }

#if defined(__unix__) || defined(__APPLE__)
  struct guarded_rank15_page {
    std::byte * address = nullptr;
    std::size_t page = 0;
    guarded_rank15_page() {
      auto size = ::sysconf(_SC_PAGESIZE);
      require(size >= 128 && size % 64 == 0, "rank15 page size unavailable");
      page = static_cast<std::size_t>(size);
      auto mapping = ::mmap(nullptr, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
      require(mapping != MAP_FAILED, "rank15 guard mapping failed");
      address = static_cast<std::byte *>(mapping);
    }
    guarded_rank15_page(guarded_rank15_page const &) = delete;
    ~guarded_rank15_page() { ::munmap(address, 3 * page); }
    void protect(int protection) {
      require(::mprotect(address + page, page, protection) == 0, "rank15 guard protection failed");
    }
    std::byte * end() const { return address + 2 * page; }
  };

  template <unsigned K> void test_groups_guarded_tails() {
    guarded_rank15_page memory;
    for (unsigned groups = 0; groups <= 260; ++groups) {
      auto count = groups ? (groups - 1) * K + 1 + groups % K : 0;
      std::vector<std::uint64_t> populations(groups);
      for (unsigned i = 0; i < groups; ++i)
        populations[i] = i + 1 == groups ? count - i * K : K;
      auto index = everett::rank_groups<K>::build(populations, count);
      constexpr auto width = everett::rank_groups<K>::class_bits;
      if (groups * width % 64) index.classes.back() |= ~std::uint64_t{0} << (groups * width % 64);
      auto bytes = index.classes.size() * 8;
      auto destination = memory.end() - bytes;
      memory.protect(PROT_READ | PROT_WRITE);
      if (bytes) std::memcpy(destination, index.classes.data(), bytes);
      memory.protect(PROT_READ);
      check_group_prefixes<K>({reinterpret_cast<std::uint64_t const *>(destination), index.classes.size()}, populations, count);
    }
  }

  void test_bitmap_guarded_tails() {
    guarded_rank15_page memory;
    for (unsigned bits = 0; bits <= 1088; ++bits) {
      std::vector<std::uint64_t> source((bits + 63) / 64, ~std::uint64_t{0});
      auto index = everett::rank_index::build(source, bits);
      // Keep all-one padding in the borrowed final word.
      auto bytes = source.size() * 8;
      auto destination = memory.end() - bytes;
      memory.protect(PROT_READ | PROT_WRITE);
      if (bytes) std::memcpy(destination, source.data(), bytes);
      memory.protect(PROT_READ);
      std::span words{reinterpret_cast<std::uint64_t const *>(destination), source.size()};
      everett::rank_view view(words, index.blocks, index.supers, bits, bits);
      for (unsigned bit = 0; bit <= bits; ++bit)
        require(view.rank(bit) == bit, "guarded bitmap prefix");
    }
    // A run boundary needs only the directory, even when the payload page is
    // not resident/readable. This catches accidental speculative full-run loads.
    std::array<std::uint64_t, 32> source;
    source.fill(~std::uint64_t{0});
    auto index = everett::rank_index::build(source, 2048);
    auto destination = memory.end() - sizeof(source);
    memory.protect(PROT_READ | PROT_WRITE);
    std::memcpy(destination, source.data(), sizeof(source));
    memory.protect(PROT_NONE);
    everett::rank_view view({reinterpret_cast<std::uint64_t const *>(destination), source.size()},
                           index.blocks, index.supers, 2048, 2048);
    for (unsigned bit = 0; bit <= 2048; bit += 512)
      require(view.rank(bit) == bit, "directory-only guarded boundary");
  }

  void test_rank15_guarded_tails() {
    guarded_rank15_page memory;
    for (unsigned groups = 0; groups <= 256; ++groups)
      for (bool maximum : {false, true}) {
        auto word_count = (groups + 15) / 16;
        std::array<std::uint64_t, 16> packed;
        // Nonzero padding catches a mask that includes an unused final nybble.
        packed.fill(std::numeric_limits<std::uint64_t>::max());
        std::vector<std::uint8_t> populations(groups);
        for (unsigned i = 0; i < groups; ++i) {
          populations[i] = std::uint8_t(maximum ? 15 : (11 * i + 3 * groups) % 16);
          auto shift = 4 * (i % 16);
          packed[i / 16] = (packed[i / 16] & ~(std::uint64_t{15} << shift)) |
                          (std::uint64_t(populations[i]) << shift);
        }
        auto bytes = word_count * sizeof(std::uint64_t);
        auto destination = memory.end() - bytes;
        memory.protect(PROT_READ | PROT_WRITE);
        if (bytes) std::memcpy(destination, packed.data(), bytes);
        memory.protect(PROT_READ);
        // Every span ends immediately before PROT_NONE. Eight-word checkpoints
        // test full 64-byte loads; shorter spans test fallback bounds. Nine to
        // fifteen words put a full checkpoint at only uint64_t alignment.
        // The empty span points at the protected page and must read nothing.
        auto words = std::span(reinterpret_cast<std::uint64_t const *>(destination), word_count);
        check_rank15_prefixes(words, populations);
      }
  }
#endif

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
    test_popcount512();
    test_prefix512();
    test_group_prefixes<3>();
    test_group_prefixes<7>();
    test_group_prefixes<31>();
    test_group_prefixes<63>();
    test_rank_directory();
    test_rank();
    test_rank_tails();
    test_rank15_words();
    test_rank15_lanes();
#if defined(__unix__) || defined(__APPLE__)
    test_rank15_guarded_tails();
    test_groups_guarded_tails<3>();
    test_groups_guarded_tails<7>();
    test_groups_guarded_tails<31>();
    test_bitmap_guarded_tails();
#endif
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
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's storage rank behavior.
 */
