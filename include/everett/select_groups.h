/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace everett {
  namespace select_groups_detail {
    inline std::uint64_t add(std::uint64_t a, std::uint64_t b) {
      if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("select_groups addition");
      return a + b;
    }
    inline std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
      if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
        throw std::overflow_error("select_groups multiplication");
      return a * b;
    }
    inline std::uint64_t words(std::uint64_t bits) noexcept {
      return bits / 64 + (bits % 64 != 0);
    }

    inline bool monotone(std::span<std::uint64_t const> source) noexcept {
      if (source.size() < 2) return true;
      std::size_t i = 1;
#if defined(__aarch64__) && defined(__ARM_NEON)
      for (; source.size() - i >= 2; i += 2)
        if (vmaxvq_u32(vreinterpretq_u32_u64(vcltq_u64(vld1q_u64(source.data() + i),
                                                      vld1q_u64(source.data() + i - 1)))))
          return false;
#endif
      for (; i < source.size(); ++i) if (source[i] < source[i - 1]) return false;
      return true;
    }

    // Zero-based selection in a nonzero word, with ordinal < popcount(value).
    // Byte-prefix populations fit in seven bits, so the marked subtraction
    // finds the first byte whose cumulative population exceeds ordinal.
    inline unsigned select_word(std::uint64_t value, unsigned ordinal) noexcept {
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
      return unsigned(std::countr_zero(_pdep_u64(std::uint64_t{1} << ordinal, value)));
#else
      auto pairs = value - ((value >> 1) & 0x5555555555555555ull);
      auto nibbles = (pairs & 0x3333333333333333ull) + ((pairs >> 2) & 0x3333333333333333ull);
      auto bytes = (nibbles + (nibbles >> 4)) & 0x0f0f0f0f0f0f0f0full;
      auto prefixes = bytes * 0x0101010101010101ull;
      auto marked = ((prefixes | 0x8080808080808080ull) -
                     (ordinal + 1) * 0x0101010101010101ull) & 0x8080808080808080ull;
      auto shift = unsigned(std::countr_zero(marked)) & ~7u;
      if (shift) ordinal -= unsigned((prefixes >> (shift - 8)) & 255);
      auto lower = unsigned((nibbles >> shift) & 15);
      auto step = unsigned(ordinal >= lower);
      shift += step * 4;
      ordinal -= step * lower;
      auto bits = unsigned(value >> shift) & 15;
      lower = (bits & 1) + ((bits >> 1) & 1);
      step = unsigned(ordinal >= lower);
      shift += step * 2;
      ordinal -= step * lower;
      return shift + ordinal + unsigned(((value >> shift) & 1) == 0);
#endif
    }

    template <unsigned W, std::size_t O, std::size_t I>
    inline std::uint64_t low_component(std::uint64_t const * source) noexcept {
      constexpr auto mask = (std::uint64_t{1} << W) - 1;
      constexpr auto bit = I * W;
      constexpr auto first = O * 64;
      if constexpr (bit < first) return (source[I] & mask) >> (first - bit);
      else return (source[I] & mask) << (bit - first);
    }

    template <unsigned W, std::size_t O, std::size_t... I>
    inline std::uint64_t low_word(std::uint64_t const * source, std::index_sequence<I...>) noexcept {
      constexpr auto first = O * 64 / W;
      return (low_component<W, O, first + I>(source) | ...);
    }

    template <unsigned W, std::size_t... O>
    inline void low_tile(std::uint64_t const * source, std::uint64_t * out,
                         std::index_sequence<O...>) noexcept {
      ((out[O] = low_word<W, O>(source,
          std::make_index_sequence<(O * 64 + 63) / W - O * 64 / W + 1>{})), ...);
    }

    // W-bit fields return to a word boundary after 64/gcd(W,64) values.
    // Each tile assigns its output words once, rather than repeatedly loading
    // and updating them for each input field. All shifts are compile-time
    // constants below 64; the final incomplete tile uses bounded scalar work.
    template <unsigned W>
    inline void pack_low_fixed(std::span<std::uint64_t const> source,
                               std::span<std::uint64_t> out) noexcept {
      static_assert(W <= 63);
      if constexpr (W) {
        constexpr auto divisor = std::gcd(W, 64u);
        constexpr auto inputs = 64 / divisor;
        constexpr auto outputs = W / divisor;
        std::size_t at = 0;
        for (; source.size() - at >= inputs; at += inputs) {
          auto destination = out.data() + (at / inputs) * outputs;
#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
          // These complete tiles consume exactly four/eight input words and
          // emit one word. Narrowing preserves each field's low bits.
          if constexpr (W == 16) {
            auto a = vmovn_u64(vld1q_u64(source.data() + at));
            auto b = vmovn_u64(vld1q_u64(source.data() + at + 2));
            *destination = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(vcombine_u32(a, b))), 0);
          } else if constexpr (W == 8) {
            auto a = vmovn_u64(vld1q_u64(source.data() + at));
            auto b = vmovn_u64(vld1q_u64(source.data() + at + 2));
            auto c = vmovn_u64(vld1q_u64(source.data() + at + 4));
            auto d = vmovn_u64(vld1q_u64(source.data() + at + 6));
            *destination = vget_lane_u64(vreinterpret_u64_u8(vmovn_u16(vcombine_u16(
              vmovn_u32(vcombine_u32(a, b)), vmovn_u32(vcombine_u32(c, d))))), 0);
          } else
#endif
            low_tile<W>(source.data() + at, destination, std::make_index_sequence<outputs>{});
        }
        auto tail = out.subspan((at / inputs) * outputs);
        std::fill(tail.begin(), tail.end(), 0);
        constexpr auto mask = (std::uint64_t{1} << W) - 1;
        for (std::size_t i = 0; i != source.size() - at; ++i) {
          auto bit = i * W;
          unsigned shift = unsigned(bit % 64);
          auto value = source[at + i] & mask;
          tail[bit / 64] |= value << shift;
          if (shift + W > 64) tail[bit / 64 + 1] |= value >> (64 - shift);
        }
      }
    }

    template <std::size_t... W>
    constexpr auto low_packers(std::index_sequence<W...>) noexcept {
      using packer = void (*)(std::span<std::uint64_t const>, std::span<std::uint64_t>) noexcept;
      return std::array<packer, sizeof...(W)>{&pack_low_fixed<W>...};
    }

    // This dispatch occurs once per complete low section, not per value. The
    // destination may contain old data: both complete and tail words are set,
    // including zero tail padding. Source and destination must not overlap.
    inline void pack_low(std::span<std::uint64_t const> source,
                         std::span<std::uint64_t> out, unsigned width) {
      if (width > 63) throw std::invalid_argument("select_groups low width");
      if (out.size() != words(multiply(source.size(), width)))
        throw std::invalid_argument("select_groups low output size");
      static constexpr auto packers = low_packers(std::make_index_sequence<64>{});
      packers[width](source, out);
    }

    // The owner has checked monotonicity, width and extent and zeroed out.
    // (source[i] >> width) + i is strictly increasing, even for equal values.
    // Accumulating a word locally removes repeated dependent memory updates;
    // gaps remain zero. This preserves the exact existing high-bit ordering.
    inline void write_high(std::span<std::uint64_t const> source,
                           std::span<std::uint64_t> out, unsigned width) noexcept {
      std::uint64_t word = 0, value = 0;
      for (std::uint64_t i = 0; i != source.size(); ++i) {
        auto position = (source[i] >> width) + i;
        auto next = position / 64;
        if (next != word) {
          out[word] = value;
          word = next;
          value = 0;
        }
        value |= std::uint64_t{1} << (position % 64);
      }
      if (!source.empty()) out[word] = value;
    }
  }

  struct select_groups_sample {
    std::uint64_t first;
    // UINT64_MAX denotes a dense group. Other values index the exception
    // array, containing every one-bit position of this group of <=256 ones.
    std::uint64_t sparse;
  };

  // Elias--Fano over residual integer addresses at records 0,K,2K,..., then an
  // actual-record-count sentinel. Residuals omit the fixed stride per record.
  // Addresses and fixed stride use the same units (for example bytes or bits).
  // Equal residual offsets are valid, including an entirely zero sequence.
  //
  // Select on the high bitvector is bounded: sample every 256 ones; scan at
  // most 4096 bits (65 aligned words) for a dense group, or read an explicit
  // exception position for a sparse group. There is no binary search. The
  // exception array is O(n): disjoint sparse groups each span >4096 high bits,
  // and Elias--Fano's high vector has O(n) bits. This is a practical baseline,
  // not a tuned succinct select implementation.
  // Views check shapes and guard navigation bounds. They do not establish
  // semantic consistency of borrowed metadata: use a builder or validated
  // reader. These native-endian spans are not a portable file-format parser.
  template <std::uint64_t K> struct select_groups_view {
    static_assert(K >= 1 && K < std::numeric_limits<std::uint64_t>::max(),
                  "physical block size must be positive and below UINT64_MAX");
    static constexpr std::uint64_t group_size = K;

    select_groups_view(std::span<std::uint64_t const> low,
                       std::span<std::uint64_t const> high,
                       std::span<select_groups_sample const> samples,
                       std::span<std::uint64_t const> sparse,
                       std::uint64_t record_count, std::uint64_t universe,
                       unsigned low_width)
      : low_(low), high_(high), samples_(samples), sparse_(sparse),
        record_count_(record_count), universe_(universe), low_width_(low_width) {
      if (low_width > 63) throw std::invalid_argument("select_groups low width");
      auto entries = group_count() + 1;
      auto low_bits = select_groups_detail::multiply(entries, low_width);
      high_bits_ = select_groups_detail::add(universe >> low_width, entries);
      if (low.size() != select_groups_detail::words(low_bits) ||
          high.size() != select_groups_detail::words(high_bits_) ||
          samples.size() != entries / 256 + (entries % 256 != 0))
        throw std::invalid_argument("invalid select_groups spans");
    }

    std::uint64_t size() const noexcept { return record_count_; }
    std::uint64_t group_count() const noexcept {
      return record_count_ / K + (record_count_ % K != 0);
    }

    std::uint64_t residual(std::uint64_t group) const {
      if (group > group_count()) throw std::out_of_range("select_groups group");
      auto position = select_high(group);
      if (position < group) throw std::invalid_argument("invalid select_groups high value");
      auto hi = position - group;
      if (hi > (universe_ >> low_width_)) throw std::invalid_argument("select_groups high overflow");
      std::uint64_t lo = 0;
      if (low_width_) {
        auto bit = group * low_width_;
        auto word = bit / 64;
        unsigned shift = unsigned(bit % 64);
        lo = low_[word] >> shift;
        if (shift + low_width_ > 64) lo |= low_[word + 1] << (64 - shift);
        lo &= (std::uint64_t{1} << low_width_) - 1;
      }
      auto value = (hi << low_width_) | lo;
      if (value > universe_) throw std::invalid_argument("select_groups value exceeds universe");
      return value;
    }

    std::uint64_t offset(std::uint64_t group, std::uint64_t fixed_stride = 0) const {
      auto value = residual(group);
      auto ordinal = group == group_count() ? record_count_ : group * K;
      return select_groups_detail::add(value, select_groups_detail::multiply(ordinal, fixed_stride));
    }

  private:
    std::uint64_t select_high(std::uint64_t ordinal) const {
      auto sample = samples_[ordinal / 256];
      unsigned remaining = unsigned(ordinal % 256);
      if (sample.sparse != std::numeric_limits<std::uint64_t>::max()) {
        if (sample.sparse > sparse_.size() || remaining >= sparse_.size() - sample.sparse)
          throw std::invalid_argument("invalid select_groups exception");
        auto position = sparse_[sample.sparse + remaining];
        if (position >= high_bits_ || !(high_[position / 64] & (std::uint64_t{1} << (position % 64))))
          throw std::invalid_argument("invalid select_groups sparse position");
        return position;
      }
      if (sample.first >= high_bits_) throw std::invalid_argument("invalid select_groups sample");
      auto word = sample.first / 64;
      auto value = high_[word] & (~std::uint64_t{0} << (sample.first % 64));
      for (unsigned scanned = 0; scanned < 65 && word < high_.size(); ++scanned, ++word) {
        if (scanned) value = high_[word];
        auto population = unsigned(std::popcount(value));
        if (remaining < population) {
          auto position = word * 64 + select_groups_detail::select_word(value, remaining);
          if (position >= high_bits_ || position - sample.first >= 4096)
            throw std::invalid_argument("select_groups dense span");
          return position;
        }
        remaining -= population;
      }
      throw std::invalid_argument("select_groups missing high bit");
    }

    std::span<std::uint64_t const> low_;
    std::span<std::uint64_t const> high_;
    std::span<select_groups_sample const> samples_;
    std::span<std::uint64_t const> sparse_;
    std::uint64_t record_count_;
    std::uint64_t universe_;
    std::uint64_t high_bits_ = 0;
    unsigned low_width_;
  };

  template <std::uint64_t K> struct select_groups {
    static_assert(K >= 1 && K < std::numeric_limits<std::uint64_t>::max(),
                  "physical block size must be positive and below UINT64_MAX");
    static constexpr std::uint64_t group_size = K;

    static select_groups build(std::span<std::uint64_t const> residuals,
                               std::uint64_t records) {
      auto groups = records / K + (records % K != 0);
      if (residuals.size() != groups + 1)
        throw std::invalid_argument("select_groups residual count");
      if (!select_groups_detail::monotone(residuals))
        throw std::invalid_argument("select_groups nonmonotone offsets");
      select_groups result;
      result.record_count = records;
      result.universe = residuals.back();
      auto quotient = result.universe / residuals.size();
      result.low_width = quotient ? unsigned(std::bit_width(quotient) - 1) : 0;
      auto low_bits = select_groups_detail::multiply(residuals.size(), result.low_width);
      auto high_bits = select_groups_detail::add(result.universe >> result.low_width, residuals.size());
      result.low.assign(select_groups_detail::words(low_bits), 0);
      result.high.assign(select_groups_detail::words(high_bits), 0);
      result.samples.clear();
      select_groups_detail::pack_low(residuals, result.low, result.low_width);
      select_groups_detail::write_high(residuals, result.high, result.low_width);
      for (std::uint64_t begin = 0; begin < residuals.size(); begin += 256) {
        auto end = residuals.size() - begin < 256 ? residuals.size() : begin + 256;
        auto first = (residuals[begin] >> result.low_width) + begin;
        auto last = (residuals[end - 1] >> result.low_width) + end - 1;
        auto sparse = std::numeric_limits<std::uint64_t>::max();
        if (last - first >= 4096) {
          sparse = result.sparse.size();
          for (auto i = begin; i < end; ++i)
            result.sparse.push_back((residuals[i] >> result.low_width) + i);
        }
        result.samples.push_back({first, sparse});
      }
      return result;
    }

    select_groups_view<K> view() const & {
      return {low, high, samples, sparse, record_count, universe, low_width};
    }

    select_groups_view<K> view() const && = delete;

    // Default represents the empty stream's offset-zero sentinel. Sections
    // are native-endian/aligned arrays, not a finalized portable file format.
    std::vector<std::uint64_t> low;
    std::vector<std::uint64_t> high{1};
    std::vector<select_groups_sample> samples{{0, std::numeric_limits<std::uint64_t>::max()}};
    std::vector<std::uint64_t> sparse;
    std::uint64_t record_count = 0;
    std::uint64_t universe = 0;
    unsigned low_width = 0;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's select groups support.
 */
