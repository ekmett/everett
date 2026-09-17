/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Encodes and selects arbitrary nondecreasing integer sequences.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <algorithm>
#include <array>
#include <everett/word_view.h>
#include <everett/error_detail.h>
#include <everett/backend.h>
#include <simd/integer.h>
#include <simd/packing.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace everett {
  namespace elias_fano_detail {
    constexpr std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept { return a + b; }
    constexpr std::uint64_t multiply(std::uint64_t a, std::uint64_t b) noexcept { return a * b; }
    inline std::uint64_t words(std::uint64_t bits) noexcept {
      return (bits + 63) >> 6;
    }

    template <simd::architecture Arch = simd::scalar>
    inline bool monotone(std::span<std::uint64_t const> source) noexcept {
      if (source.size() < 2) return true;
      std::size_t i = 1;
      if constexpr (!std::is_same_v<Arch, simd::scalar>) {
        using V = simd::vec<std::uint64_t, backend_detail::register_bytes<Arch> / 8, Arch>;
        for (; source.size() - i >= V::lanes; i += V::lanes)
          if (any(V::load(source.data() + i) < V::load(source.data() + i - 1))) return false;
      }
      for (; i < source.size(); ++i) if (source[i] < source[i - 1]) return false;
      return true;
    }

    // Zero-based selection in a nonzero word, with ordinal < popcount(value).
    // Byte-prefix populations fit in seven bits, so the marked subtraction
    // finds the first byte whose cumulative population exceeds ordinal.
    template <simd::architecture Arch = simd::scalar>
    inline unsigned select_word(std::uint64_t value, unsigned ordinal) noexcept {
      if constexpr (std::is_same_v<Arch, simd::avx2> || std::is_same_v<Arch, simd::avx512>)
        return unsigned(std::countr_zero(deposit_bits(Arch{}, std::uint64_t{1} << ordinal, value)));
      else {
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
      }
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
    template <unsigned W, simd::architecture Arch = simd::scalar>
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
          // Narrowing truncates low fields and concatenates in source order.
          // Only the first eight packed bytes are written, with no tail read.
          if constexpr (!std::is_same_v<Arch, simd::scalar> &&
              std::endian::native == std::endian::little && (W == 8 || W == 16)) {
            using V = simd::vec<std::uint64_t, 2, Arch>;
            auto a = narrow_concat<std::uint32_t>(V::load(source.data() + at),
              V::load(source.data() + at + 2));
            if constexpr (W == 16) {
              auto packed = narrow_concat<std::uint16_t>(a, decltype(a){});
              reinterpret_bits<std::uint64_t>(packed).store_partial(destination, 1);
            } else {
              auto b = narrow_concat<std::uint32_t>(V::load(source.data() + at + 4),
                V::load(source.data() + at + 6));
              auto half = narrow_concat<std::uint16_t>(a, b);
              auto packed = narrow_concat<std::uint8_t>(half, decltype(half){});
              reinterpret_bits<std::uint64_t>(packed).store_partial(destination, 1);
            }
          } else
            low_tile<W>(source.data() + at, destination, std::make_index_sequence<outputs>{});
        }
        auto tail = out.subspan((at / inputs) * outputs);
        std::fill(tail.begin(), tail.end(), 0);
        constexpr auto mask = (std::uint64_t{1} << W) - 1;
        for (std::size_t i = 0; i != source.size() - at; ++i) {
          auto bit = i * W;
          unsigned shift = unsigned(bit & 63);
          auto value = source[at + i] & mask;
          tail[bit >> 6] |= value << shift;
          if (shift + W > 64) tail[(bit >> 6) + 1] |= value >> (64 - shift);
        }
      }
    }

    template <simd::architecture Arch, std::size_t... W>
    constexpr auto low_packers(std::index_sequence<W...>) noexcept {
      using packer = void (*)(std::span<std::uint64_t const>, std::span<std::uint64_t>) noexcept;
      return std::array<packer, sizeof...(W)>{&pack_low_fixed<W, Arch>...};
    }

    // This dispatch occurs once per complete low section, not per value. The
    // destination may contain old data: both complete and tail words are set,
    // including zero tail padding. Source and destination must not overlap.
    template <simd::architecture Arch = simd::scalar>
    inline void pack_low(std::span<std::uint64_t const> source,
                         std::span<std::uint64_t> out, unsigned width) {
      if (width > 63) error_detail::raise<std::invalid_argument>("elias_fano low width");
      if (width && source.size() > (std::numeric_limits<std::uint64_t>::max() - 63) / width)
        error_detail::raise<std::overflow_error>("elias_fano low section extent");
      if (out.size() != words(source.size() * width))
        error_detail::raise<std::invalid_argument>("elias_fano low output size");
      static constexpr auto packers = low_packers<Arch>(std::make_index_sequence<64>{});
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
        auto next = position >> 6;
        if (next != word) {
          out[word] = value;
          word = next;
          value = 0;
        }
        value |= std::uint64_t{1} << (position & 63);
      }
      if (!source.empty()) out[word] = value;
    }
  }

  struct elias_fano_sample {
    std::uint64_t first;
    // UINT64_MAX denotes a dense group. Other values index the exception
    // array, containing every one-bit position of this group of <=256 ones.
    std::uint64_t sparse;
  };

  struct elias_fano_cursor;

  // Elias--Fano over an arbitrary nondecreasing sequence of uint64_t values.
  // Repeated values and the empty sequence are supported. Sampling intervals,
  // record counts, sentinels and fixed strides belong to the caller.
  // Select samples every 256 ones: a dense group spans at most 4096 bits
  // (65 aligned words); a sparse group stores each one-bit position explicitly.
  // Constructors validate section shapes without reading their contents.
  // Borrowed metadata must come from a builder or an independently validated
  // reader. Native words and unaligned little-endian sections share navigation.
  struct elias_fano_view {
    elias_fano_view() = default;

    elias_fano_view(std::span<std::uint64_t const> low,
                       std::span<std::uint64_t const> high,
                       std::span<elias_fano_sample const> samples,
                       std::span<std::uint64_t const> sparse,
                       std::uint64_t entry_count, std::uint64_t universe,
                       unsigned low_width)
      : elias_fano_view(word_view(low), word_view(high), sample_view(samples), word_view(sparse),
                         entry_count, universe, low_width) {}

    template <class Words, class Samples>
      requires (std::is_same_v<Words, word_view> && std::is_same_v<Samples, sample_view>)
    elias_fano_view(Words low, Words high, Samples samples, Words sparse,
                       std::uint64_t entry_count, std::uint64_t universe, unsigned low_width)
      : low_(low), high_(high), samples_(samples), sparse_(sparse),
        entry_count_(entry_count), universe_(universe), low_width_(low_width) {
      if (low_width > 63) error_detail::raise<std::invalid_argument>("elias_fano low width");
      if (!entry_count) {
        if (universe || low_width || !low.empty() || !high.empty() || !samples.empty() || !sparse.empty())
          error_detail::raise<std::invalid_argument>("invalid empty Elias-Fano sections");
        return;
      }
      constexpr auto maximum = std::numeric_limits<std::uint64_t>::max() - 255;
      auto entries = entry_count;
      // Validate section extents once, before borrowing any navigation words.
      if (entries > maximum || (low_width && entries > maximum / low_width) ||
          (universe >> low_width) > maximum - entries)
        error_detail::raise<std::overflow_error>("elias_fano section extent");
      auto low_bits = elias_fano_detail::multiply(entries, low_width);
      high_bits_ = elias_fano_detail::add(universe >> low_width, entries);
      if (low.size() != elias_fano_detail::words(low_bits) ||
          high.size() != elias_fano_detail::words(high_bits_) ||
          samples.size() != ((entries + 255) >> 8))
        error_detail::raise<std::invalid_argument>("invalid elias_fano spans");
    }

    word_view low_words() const noexcept { return low_; }
    word_view high_words() const noexcept { return high_; }
    sample_view samples() const noexcept { return samples_; }
    word_view sparse_words() const noexcept { return sparse_; }
    std::uint64_t universe() const noexcept { return universe_; }
    unsigned low_width() const noexcept { return low_width_; }
    std::uint64_t size() const noexcept { return entry_count_; }

    template <simd::architecture Arch = simd::scalar>
    std::uint64_t select(std::uint64_t ordinal) const {
      if (ordinal >= entry_count_) [[unlikely]]
        error_detail::raise<std::out_of_range>("Elias-Fano ordinal");
      return decode(ordinal, select_high<Arch>(ordinal));
    }

    template <simd::architecture Arch = simd::scalar>
    elias_fano_cursor cursor(std::uint64_t ordinal = 0) const;

  private:
    friend struct elias_fano_cursor;
    std::uint64_t decode(std::uint64_t ordinal, std::uint64_t position) const {
      if (position < ordinal) error_detail::raise<std::invalid_argument>("invalid elias_fano high value");
      auto hi = position - ordinal;
      if (hi > (universe_ >> low_width_)) error_detail::raise<std::invalid_argument>("elias_fano high overflow");
      std::uint64_t lo = 0;
      if (low_width_) {
        auto bit = ordinal * low_width_;
        auto word = bit >> 6;
        unsigned shift = unsigned(bit & 63);
        lo = low_[word] >> shift;
        if (shift + low_width_ > 64) lo |= low_[word + 1] << (64 - shift);
        lo &= (std::uint64_t{1} << low_width_) - 1;
      }
      auto value = (hi << low_width_) | lo;
      if (value > universe_) error_detail::raise<std::invalid_argument>("elias_fano value exceeds universe");
      return value;
    }

    template <simd::architecture Arch>
    std::uint64_t select_high(std::uint64_t ordinal) const {
      auto sample = samples_[ordinal >> 8];
      unsigned remaining = unsigned(ordinal & 255);
      if (sample.sparse != std::numeric_limits<std::uint64_t>::max()) {
        if (sample.sparse > sparse_.size() || remaining >= sparse_.size() - sample.sparse)
          error_detail::raise<std::invalid_argument>("invalid elias_fano exception");
        auto position = sparse_[sample.sparse + remaining];
        if (position >= high_bits_ || !(high_[position >> 6] & (std::uint64_t{1} << (position & 63))))
          error_detail::raise<std::invalid_argument>("invalid elias_fano sparse position");
        return position;
      }
      if (sample.first >= high_bits_) error_detail::raise<std::invalid_argument>("invalid elias_fano sample");
      auto word = sample.first >> 6;
      auto value = high_[word] & (~std::uint64_t{0} << (sample.first & 63));
      for (unsigned scanned = 0; scanned < 65 && word < high_.size(); ++scanned, ++word) {
        if (scanned) value = high_[word];
        auto population = unsigned(std::popcount(value));
        if (remaining < population) {
          auto position = word * 64 + elias_fano_detail::select_word<Arch>(value, remaining);
          if (position >= high_bits_ || position - sample.first >= 4096)
            error_detail::raise<std::invalid_argument>("elias_fano dense span");
          return position;
        }
        remaining -= population;
      }
      error_detail::raise<std::invalid_argument>("elias_fano missing high bit");
    }

    word_view low_;
    word_view high_;
    sample_view samples_;
    word_view sparse_;
    std::uint64_t entry_count_ = 0;
    std::uint64_t universe_ = 0;
    std::uint64_t high_bits_ = 0;
    unsigned low_width_ = 0;
  };

  // Forward selection retains the unused high bits of its current word.
  // Every 256 entries it enters the next directory sample, preserving dense
  // span and sparse-exception checks. An ordinal start performs one selection;
  // starting at zero or a directory boundary reads no payload pages.
  // The source sections must outlive this cursor and any copies of it.
  struct elias_fano_cursor {
    template <simd::architecture Arch = simd::scalar>
    explicit elias_fano_cursor(elias_fano_view source, std::uint64_t ordinal = 0,
        std::type_identity<Arch> = {})
      : source_(source), ordinal_(ordinal) {
      if (ordinal > source.size()) error_detail::raise<std::out_of_range>("Elias-Fano cursor ordinal");
      if (done() || !(ordinal & 255)) return;
      auto sample = source_.samples_[ordinal >> 8];
      sample_ = {sample.first, sample.sparse};
      if (sample_.sparse == std::numeric_limits<std::uint64_t>::max()) {
        auto position = source_.select_high<Arch>(ordinal);
        word_ = position >> 6;
        remaining_ = source_.high_[word_] & (~std::uint64_t{0} << (position & 63));
      }
    }
    bool done() const noexcept { return ordinal_ == source_.size(); }
    std::uint64_t ordinal() const noexcept { return ordinal_; }
    std::uint64_t next() {
      if (done()) [[unlikely]] error_detail::raise<std::out_of_range>("Elias-Fano cursor end");
      if (!(ordinal_ & 255)) {
        auto sample = source_.samples_[ordinal_ >> 8];
        sample_ = {sample.first, sample.sparse};
        if (sample_.sparse == std::numeric_limits<std::uint64_t>::max()) {
          if (sample_.first >= source_.high_bits_)
            error_detail::raise<std::invalid_argument>("invalid elias_fano sample");
          word_ = sample_.first >> 6;
          remaining_ = source_.high_[word_] & (~std::uint64_t{0} << (sample_.first & 63));
        }
      }
      std::uint64_t position;
      if (sample_.sparse != std::numeric_limits<std::uint64_t>::max()) {
        auto lane = ordinal_ & 255;
        if (sample_.sparse > source_.sparse_.size() || lane >= source_.sparse_.size() - sample_.sparse)
          error_detail::raise<std::invalid_argument>("invalid elias_fano exception");
        position = source_.sparse_[sample_.sparse + lane];
        if (position >= source_.high_bits_ || !(source_.high_[position >> 6] & (std::uint64_t{1} << (position & 63))))
          error_detail::raise<std::invalid_argument>("invalid elias_fano sparse position");
      } else {
        while (!remaining_) {
          ++word_;
          if (word_ >= source_.high_.size() || word_ - (sample_.first >> 6) >= 65)
            error_detail::raise<std::invalid_argument>("elias_fano missing high bit");
          remaining_ = source_.high_[word_];
        }
        position = (word_ << 6) + unsigned(std::countr_zero(remaining_));
        if (position >= source_.high_bits_ || position - sample_.first >= 4096)
          error_detail::raise<std::invalid_argument>("elias_fano dense span");
      }
      auto value = source_.decode(ordinal_, position);
      remaining_ &= remaining_ - 1;
      ++ordinal_;
      return value;
    }

  private:
    elias_fano_view source_;
    elias_fano_sample sample_{};
    std::uint64_t ordinal_ = 0, word_ = 0, remaining_ = 0;
  };

  template <simd::architecture Arch>
  inline elias_fano_cursor elias_fano_view::cursor(std::uint64_t ordinal) const {
    return elias_fano_cursor(*this, ordinal, std::type_identity<Arch>{});
  }

  struct elias_fano {
    template <simd::architecture Arch = simd::scalar>
    static elias_fano build(std::span<std::uint64_t const> residuals) {
      if (residuals.empty()) return {};
      if (!elias_fano_detail::monotone<Arch>(residuals))
        error_detail::raise<std::invalid_argument>("elias_fano nonmonotone offsets");
      elias_fano result;
      result.entry_count = residuals.size();
      result.universe = residuals.back();
      auto quotient = result.universe / residuals.size();
      result.low_width = quotient ? unsigned(std::bit_width(quotient) - 1) : 0;
      constexpr auto maximum = std::numeric_limits<std::uint64_t>::max() - 255;
      if (residuals.size() > maximum || (result.low_width && residuals.size() > maximum / result.low_width) ||
          (result.universe >> result.low_width) > maximum - residuals.size())
        error_detail::raise<std::overflow_error>("elias_fano section extent");
      auto low_bits = elias_fano_detail::multiply(residuals.size(), result.low_width);
      auto high_bits = elias_fano_detail::add(result.universe >> result.low_width, residuals.size());
      result.low.assign(elias_fano_detail::words(low_bits), 0);
      result.high.assign(elias_fano_detail::words(high_bits), 0);
      result.samples.clear();
      elias_fano_detail::pack_low<Arch>(residuals, result.low, result.low_width);
      elias_fano_detail::write_high(residuals, result.high, result.low_width);
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

    elias_fano_view view() const & {
      return {low, high, samples, sparse, entry_count, universe, low_width};
    }

    elias_fano_view view() const && = delete;

    // The default is an empty sequence. A sampling owner encodes any terminal
    // sentinel explicitly as another value; the codec imposes no stride.
    std::vector<std::uint64_t> low;
    std::vector<std::uint64_t> high;
    std::vector<elias_fano_sample> samples;
    std::vector<std::uint64_t> sparse;
    std::uint64_t entry_count = 0;
    std::uint64_t universe = 0;
    unsigned low_width = 0;
  };
}
