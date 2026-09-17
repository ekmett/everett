/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Defines optional offset-selection representations on identical integer sequences.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once
#include "../host_backend.h"
#include <everett/elias_fano.h>
#include <memory>
#include <string>
// External, independently licensed Sux headers are supplied by the build.
#include <sux/bits/SimpleSelect.hpp>
#include <sux/bits/SimpleSelectHalf.hpp>
#include <everett_optional_sux_half_fixed.hpp>
#undef likely
#undef unlikely

namespace select_compare {
  using u64 = std::uint64_t;
  struct space {
    u64 payload = 0, auxiliary = 0, allocated = 0, object = 0;
  };
  inline void require(bool value, char const *message) {
    if (!value) throw std::runtime_error(message);
  }
  inline u64 words(u64 bits) { return (bits + 63) >> 6; }
  inline u64 decode_value(std::span<u64 const> low, u64 universe, unsigned width, u64 ordinal, u64 position) {
    require(position >= ordinal && (position - ordinal) <= (universe >> width), "bad high position");
    u64 lower = 0;
    if (width) {
      auto bit = ordinal * width; auto word = bit >> 6; auto shift = unsigned(bit & 63);
      lower = low[word] >> shift;
      if (shift + width > 64) lower |= low[word + 1] << (64 - shift);
      lower &= (u64{1} << width) - 1;
    }
    auto value = ((position - ordinal) << width) | lower;
    require(value <= universe, "bad offset");
    return value;
  }
  struct payload {
    std::vector<u64> low, high;
    u64 count = 0, universe = 0;
    unsigned width = 0;
    explicit payload(std::span<u64 const> values) : count(values.size()), universe(values.empty() ? 0 : values.back()) {
      require(everett::elias_fano_detail::monotone<everett_experiment::architecture>(values), "unordered offsets");
      auto quotient = count ? universe / count : 0;
      width = quotient ? unsigned(std::bit_width(quotient) - 1) : 0;
      low.assign(words(count * width), 0); high.assign(words(high_bits()), 0);
      everett::elias_fano_detail::pack_low<everett_experiment::architecture>(values, low, width);
      everett::elias_fano_detail::write_high(values, high, width);
    }
    u64 high_bits() const { return (universe >> width) + count; }
    u64 bytes() const { return (low.size() + high.size()) * 8; }
    u64 allocated() const { return (low.capacity() + high.capacity()) * 8; }
    u64 decode(u64 ordinal, u64 position) const {
      return decode_value(low, universe, width, ordinal, position);
    }
  };
  struct current {
    everett::elias_fano data;
    everett::elias_fano_view view;
    explicit current(std::span<u64 const> values) : data(everett::elias_fano::build<everett_experiment::architecture>(values)), view(data.view()) {}
    u64 select(u64 ordinal) const { return view.select<everett_experiment::architecture>(ordinal); }
    space sizes() const { return {(data.low.size() + data.high.size()) * 8,
      data.samples.size() * 16 + data.sparse.size() * 8,
      (data.low.capacity() + data.high.capacity() + data.sparse.capacity()) * 8 + data.samples.capacity() * 16,
      sizeof(*this)}; }
  };
  // Same production directory and word select, on builder-validated arrays;
  // using direct owning arrays instead of mapped/native views and skipping
  // per-call high-directory/span validation.
  struct trusted_current : current {
    using current::current;
    u64 select(u64 ordinal) const {
      if (ordinal >= data.entry_count) throw std::out_of_range("select ordinal");
      auto sample = data.samples[ordinal >> 8];
      unsigned residual = unsigned(ordinal & 255);
      u64 position;
      if (sample.sparse != std::numeric_limits<u64>::max()) position = data.sparse[sample.sparse + residual];
      else {
        auto word = sample.first >> 6;
        auto bits = data.high[word] & (~u64{} << (sample.first & 63));
        for (;;) {
          auto count = unsigned(std::popcount(bits));
          if (residual < count) { position = (word << 6) + everett::elias_fano_detail::select_word(bits, residual); break; }
          residual -= count; bits = data.high[++word];
        }
      }
      return decode_value(data.low, data.universe, data.low_width, ordinal, position);
    }
  };
  template <class T> struct direct {
    std::vector<T> values;
    explicit direct(std::span<u64 const> source) {
      if (!source.empty()) require(source.back() <= std::numeric_limits<T>::max(), "direct width exceeded");
      values.assign(source.begin(), source.end());
    }
    u64 select(u64 ordinal) const { return values[ordinal]; }
    space sizes() const { return {values.size() * sizeof(T), 0, values.capacity() * sizeof(T), sizeof(*this)}; }
  };
  struct packed {
    std::vector<u64> data;
    unsigned width;
    explicit packed(std::span<u64 const> source) : width(source.empty() ? 0 : unsigned(std::bit_width(source.back()))) {
      data.assign(words(source.size() * width), 0);
      if (width == 64) std::copy(source.begin(), source.end(), data.begin());
      else everett::elias_fano_detail::pack_low<everett_experiment::architecture>(source, data, width);
    }
    u64 select(u64 ordinal) const {
      if (!width) return 0;
      auto at = ordinal * width; auto word = at >> 6; auto shift = unsigned(at & 63);
      auto value = data[word] >> shift;
      if (shift + width > 64) value |= data[word + 1] << (64 - shift);
      return width == 64 ? value : value & ((u64{1} << width) - 1);
    }
    space sizes() const { return {data.size() * 8, 0, data.capacity() * 8, sizeof(*this)}; }
  };
  struct high_direct {
    std::vector<u64> positions;
    explicit high_direct(payload const &p) {
      positions.reserve(p.count);
      for (u64 w = 0; w < p.high.size(); ++w) {
        auto bits = p.high[w];
        while (bits) { positions.push_back((w << 6) + std::countr_zero(bits)); bits &= bits - 1; }
      }
      require(positions.size() == p.count, "high population mismatch");
    }
    u64 select(u64 ordinal) const { return positions[ordinal]; }
    u64 bytes() const { return positions.size() * 8; }
    u64 allocated() const { return positions.capacity() * 8; }
  };
  // Keep the production 256-one/sparse directory and add a uint16 offset
  // every32 ones in each dense group. Sparse groups retain exact positions.
  struct sub32 {
    std::vector<everett::elias_fano_sample> samples;
    std::vector<u64> sparse;
    std::vector<std::array<std::uint16_t, 8>> sub;
    payload const *data;
    explicit sub32(payload const &p) : data(&p) {
      std::array<u64, 256> group{}; unsigned used = 0;
      auto flush = [&] {
        if (!used) return;
        auto start = group[0], last = group[used - 1];
        auto exceptional = std::numeric_limits<u64>::max();
        std::array<std::uint16_t, 8> offsets{};
        if (last - start >= 4096) {
          exceptional = sparse.size(); sparse.insert(sparse.end(), group.begin(), group.begin() + used);
        } else for (unsigned i = 0; i < used; i += 32) offsets[i >> 5] = std::uint16_t(group[i] - start);
        samples.push_back({start, exceptional}); sub.push_back(offsets); used = 0;
      };
      for (u64 w = 0; w < p.high.size(); ++w) {
        auto bits = p.high[w];
        while (bits) {
          group[used++] = (w << 6) + std::countr_zero(bits); bits &= bits - 1;
          if (used == 256) flush();
        }
      }
      flush();
    }
    u64 select(u64 ordinal) const {
      auto g = ordinal >> 8; auto lane = unsigned(ordinal & 255); auto sample = samples[g];
      if (sample.sparse != std::numeric_limits<u64>::max()) return sparse[sample.sparse + lane];
      auto start = sample.first + sub[g][lane >> 5];
      auto word = start >> 6, bits = data->high[word] & (~u64{} << (start & 63));
      auto residual = lane & 31;
      for (;;) {
        auto count = unsigned(std::popcount(bits));
        if (residual < count) return (word << 6) + everett::elias_fano_detail::select_word(bits, residual);
        residual -= count; bits = data->high[++word];
      }
    }
    u64 bytes() const { return samples.size() * 16 + sparse.size() * 8 + sub.size() * 16; }
    u64 allocated() const { return samples.capacity() * 16 + sparse.capacity() * 8 + sub.capacity() * 16; }
  };
  template <int Parameter> struct simple {
    using implementation = std::conditional_t<(Parameter == -2), sux::bits::SimpleSelectHalfFixed<>,
      std::conditional_t<(Parameter == -1), sux::bits::SimpleSelectHalf<>, sux::bits::SimpleSelect<>>>;
    std::unique_ptr<implementation> index;
    explicit simple(payload const &p) {
      if constexpr (Parameter < 0) index = std::make_unique<implementation>(p.high.data(), p.high_bits());
      else index = std::make_unique<implementation>(p.high.data(), p.high_bits(), Parameter);
    }
    u64 select(u64 ordinal) const { return index->select(ordinal); }
    // Both external arrays use Vector::size(), which trims MALLOC capacity
    // to their logical word counts. Exclude object fields, count sentinel/spill.
    u64 bytes() const { return index->bitCount() / 8 - sizeof(implementation); }
    u64 allocated() const { return bytes(); }
    u64 object() const { return sizeof(implementation); }
  };
  template <class Index> struct alternative {
    using index_type = Index;
    payload data;
    Index index;
    explicit alternative(std::span<u64 const> values) : data(values), index(data) {}
    u64 select(u64 ordinal) const {
      if (ordinal >= data.count) throw std::out_of_range("select ordinal");
      return data.decode(ordinal, index.select(ordinal));
    }
    space sizes() const {
      u64 extra = 0;
      if constexpr (requires { index.object(); }) extra = index.object();
      return {data.bytes(), index.bytes(), data.allocated() + index.allocated(), sizeof(*this) + extra};
    }
  };
}
