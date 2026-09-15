/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests generic Elias-Fano independently of record sampling and stride.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/elias_fano.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif
namespace {
  using namespace diet;
  void require(bool condition, char const * text) { if (!condition) throw std::runtime_error(text); }
  template <class F> void rejects(F action) {
    bool failed = false;
    try { action(); } catch (std::exception const &) { failed = true; }
    require(failed, "invalid Elias-Fano operation accepted");
  }
  std::vector<std::byte> bytes(std::span<std::uint64_t const> words, unsigned offset = 0) {
    std::vector<std::byte> result(offset + 8 * words.size(), std::byte{0xa5});
    for (std::size_t i = 0; i < words.size(); ++i)
      for (unsigned j = 0; j < 8; ++j) result[offset + 8 * i + j] = std::byte(words[i] >> (8 * j));
    return result;
  }
  std::vector<std::byte> sample_bytes(std::span<elias_fano_sample const> samples, unsigned offset = 0) {
    std::vector<std::uint64_t> words;
    for (auto sample : samples) { words.push_back(sample.first); words.push_back(sample.sparse); }
    return bytes(words, offset);
  }
  struct encoding {
    std::vector<std::uint64_t> low, high, sparse;
    std::vector<elias_fano_sample> samples;
  };
  // Independent one-bit-at-a-time encoding: no production packing, word
  // selection, rank, or directory construction helpers are used by the oracle.
  encoding oracle(std::span<std::uint64_t const> values, unsigned width) {
    encoding out;
    if (values.empty()) return out;
    auto low_bits = values.size() * width;
    auto high_bits = (values.back() >> width) + values.size();
    out.low.resize(low_bits / 64 + (low_bits % 64 != 0));
    out.high.resize(high_bits / 64 + (high_bits % 64 != 0));
    std::vector<std::uint64_t> positions;
    for (std::uint64_t i = 0; i < values.size(); ++i) {
      for (unsigned bit = 0; bit < width; ++bit) if ((values[i] >> bit) & 1) {
        auto at = i * width + bit;
        out.low[at / 64] |= std::uint64_t{1} << (at % 64);
      }
      auto at = (values[i] >> width) + i;
      out.high[at / 64] |= std::uint64_t{1} << (at % 64);
      positions.push_back(at);
    }
    for (std::size_t first = 0; first < positions.size(); first += 256) {
      auto end = std::min(positions.size(), first + 256);
      auto sparse = std::numeric_limits<std::uint64_t>::max();
      if (positions[end - 1] - positions[first] >= 4096) {
        sparse = out.sparse.size();
        out.sparse.insert(out.sparse.end(), positions.begin() + first, positions.begin() + end);
      }
      out.samples.push_back({positions[first], sparse});
    }
    return out;
  }
  void query_oracle(elias_fano_view view, std::span<std::uint64_t const> values) {
    require(view.size() == values.size(), "EF encoded entry count");
    for (std::size_t i = 0; i < values.size(); ++i)
      require(view.select(i) == values[i], "EF select differs from original integer");
    rejects([&] { (void)view.select(values.size()); });
    rejects([&] { (void)view.select(std::numeric_limits<std::uint64_t>::max()); });
  }
  void check(std::vector<std::uint64_t> const & values) {
    auto actual = elias_fano::build(values);
    auto width = 0u;
    if (!values.empty()) {
      auto quotient = values.back() / values.size();
      while (quotient > 1) { ++width; quotient >>= 1; }
    }
    require(actual.entry_count == values.size() && actual.low_width == width &&
      actual.universe == (values.empty() ? 0 : values.back()), "EF scalar metadata");
    auto expected = oracle(values, width);
    require(actual.low == expected.low && actual.high == expected.high && actual.sparse == expected.sparse,
            "EF wire words differ from independent encoding");
    require(actual.samples.size() == expected.samples.size(), "EF sample count");
    for (std::size_t i = 0; i < actual.samples.size(); ++i)
      require(actual.samples[i].first == expected.samples[i].first && actual.samples[i].sparse == expected.samples[i].sparse,
              "EF sample wire pair");
    query_oracle(actual.view(), values);
    for (unsigned offset = 0; offset < 8; ++offset) {
      auto low = bytes(expected.low, offset), high = bytes(expected.high, offset), sparse = bytes(expected.sparse, offset);
      auto samples = sample_bytes(expected.samples, offset);
      auto word = [&](auto const & data) { return word_view::little_endian(std::span(data).subspan(offset)); };
      elias_fano_view mapped(word(low), word(high), sample_view::little_endian(std::span(samples).subspan(offset)), word(sparse),
                            values.size(), actual.universe, width);
      require(mapped.low_words().bytes().data() == low.data() + offset &&
              mapped.high_words().bytes().data() == high.data() + offset, "EF copied mapped words");
      query_oracle(mapped, values);
    }
  }
  void patterns() {
    std::mt19937_64 random(0xe11a5fa0);
    for (std::size_t n : {0u, 1u, 2u, 15u, 16u, 63u, 64u, 65u, 127u, 128u, 255u, 256u, 257u, 511u, 512u, 513u}) {
      check(std::vector<std::uint64_t>(n));
      check(std::vector<std::uint64_t>(n, 7));
      std::vector<std::uint64_t> values(n);
      for (std::size_t i = 0; i < n; ++i) values[i] = random() % 100000;
      std::sort(values.begin(), values.end()); check(values);
    }
    // Cover every low width, including the optimized eight/sixteen-bit packers.
    for (unsigned width = 0; width < 64; ++width) {
      auto n = std::min<std::uint64_t>(257, std::uint64_t{1} << (63 - width));
      std::vector<std::uint64_t> values(n);
      auto mask = (std::uint64_t{1} << width) - 1;
      for (std::uint64_t i = 0; i < n; ++i) values[i] = (i << width) | (random() & mask);
      values.back() = n << width;
      check(values);
    }
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    check({maximum}); check({0, maximum}); check({maximum - 1, maximum});
    std::vector<std::uint64_t> sparse(10001, 20000);
    std::fill(sparse.begin(), sparse.begin() + 17, 0);
    auto built = elias_fano::build(sparse);
    require(!built.sparse.empty(), "EF sparse fixture did not select exceptions");
    check(sparse);
  }
#if defined(__unix__) || defined(__APPLE__)
  struct guarded_section {
    void * base;
    std::size_t page;
    std::span<std::byte const> data;
    explicit guarded_section(std::span<std::byte const> source) {
      page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
      require(source.size() <= page, "EF guard fixture fits page");
      base = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      require(base != MAP_FAILED, "map EF guard fixture");
      auto end = static_cast<std::byte *>(base) + page;
      if (!source.empty()) std::memcpy(end - source.size(), source.data(), source.size());
      data = {end - source.size(), source.size()};
      require(mprotect(end, page, PROT_NONE) == 0, "protect EF tail");
      protect(PROT_NONE);
    }
    void protect(int permissions) { require(mprotect(base, page, permissions) == 0, "EF payload protection"); }
    ~guarded_section() { munmap(base, 2 * page); }
    guarded_section(guarded_section const &) = delete;
    guarded_section & operator=(guarded_section const &) = delete;
  };
  void guarded(std::vector<std::uint64_t> const & values) {
    auto index = elias_fano::build(values);
    guarded_section low(bytes(index.low)), high(bytes(index.high)), samples(sample_bytes(index.samples)), sparse(bytes(index.sparse));
    // Every directory and payload page is inaccessible during shape opening.
    elias_fano_view view(word_view::little_endian(low.data), word_view::little_endian(high.data),
      sample_view::little_endian(samples.data), word_view::little_endian(sparse.data), values.size(), index.universe, index.low_width);
    require(view.size() == values.size(), "protected EF shape query");
    rejects([&] { (void)view.select(values.size()); });
    low.protect(PROT_READ); high.protect(PROT_READ); samples.protect(PROT_READ); sparse.protect(PROT_READ);
    query_oracle(view, values);
  }
  void guard_pages() {
    for (unsigned n = 0; n <= 260; ++n) {
      std::vector<std::uint64_t> values(n);
      for (unsigned i = 0; i < n; ++i) values[i] = (i / 3) * 127;
      guarded(values);
    }
    std::vector<std::uint64_t> sparse(10001, 20000);
    std::fill(sparse.begin(), sparse.begin() + 17, 0);
    guarded(sparse);
  }
#endif
  void invalid() {
    static_assert(noexcept(elias_fano_detail::add(1, 2)));
    static_assert(noexcept(elias_fano_detail::multiply(1, 2)));
    elias_fano empty;
    query_oracle(empty.view(), {});
    query_oracle(elias_fano_view{}, {});
    rejects([] { elias_fano::build(std::array<std::uint64_t, 2>{2, 1}); });
    rejects([] { elias_fano_view({}, {}, {}, {}, 0, 1, 0); });
    rejects([] { elias_fano_view({}, {}, {}, {}, 0, 0, 1); });
    rejects([] { elias_fano_view({}, {}, {}, {}, 0, 0, 64); });
    rejects([] { elias_fano_view({}, {}, {}, {}, 1, ~std::uint64_t{0}, 0); });
    rejects([] { elias_fano_view({}, {}, {}, {}, ~std::uint64_t{0}, 0, 63); });
    rejects([] { elias_fano_view({}, {}, {}, {}, 1, 0, 0); });
    std::array<std::uint64_t, 1> word{0};
    rejects([&] { elias_fano_view(word, {}, {}, {}, 0, 0, 0); });
    rejects([&] { elias_fano_view({}, {}, {}, word, 0, 0, 0); });
    auto missing = elias_fano::build(std::array<std::uint64_t, 2>{0, 1});
    missing.high[0] = 0;
    rejects([&] { (void)missing.view().select(0); });
    auto bad_sample = elias_fano::build(std::array<std::uint64_t, 1>{0});
    bad_sample.samples[0].first = ~std::uint64_t{0};
    rejects([&] { (void)bad_sample.view().select(0); });
  }
}
int main() {
  try {
    patterns(); invalid();
#if defined(__unix__) || defined(__APPLE__)
    guard_pages();
#endif
    std::cout << "Generic Elias-Fano encoding, empty sequences, and guarded selection checks passed\n";
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
