#pragma once

#include <everett/select15.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace everett {
  // Keys are byte strings ordered lexicographically by unsigned byte value.
  inline int compare_keys(std::string_view a, std::string_view b) noexcept {
    auto n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i != n; ++i) {
      auto x = static_cast<unsigned char>(a[i]);
      auto y = static_cast<unsigned char>(b[i]);
      if (x != y) return x < y ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
  }

  inline std::size_t common_prefix(std::string_view a, std::string_view b) noexcept {
    std::size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
    return n;
  }

  // prefix contains the leading min(full_size, requested_limit) bytes.
  struct front_anchor {
    std::string_view prefix;
    std::uint64_t full_size = 0;

    static front_anchor complete(std::string_view key) noexcept {
      return {key, key.size()};
    }
  };

  inline int compare_prefix(front_anchor key, std::string_view query) {
    auto needed = std::min<std::uint64_t>(key.full_size, query.size());
    if (key.prefix.size() < needed) {
      throw std::invalid_argument("front-coded comparison has an incomplete query prefix");
    }
    auto prefix_order = compare_keys(key.prefix.substr(0, static_cast<std::size_t>(needed)),
                                    query.substr(0, static_cast<std::size_t>(needed)));
    if (prefix_order) return prefix_order;
    return key.full_size < query.size() ? -1 : key.full_size > query.size() ? 1 : 0;
  }

  namespace front_detail {
    inline void append_varint(std::vector<std::byte> & bytes, std::uint64_t value) {
      while (value >= 128) {
        bytes.push_back(static_cast<std::byte>((value & 127) | 128));
        value >>= 7;
      }
      bytes.push_back(static_cast<std::byte>(value));
    }

    inline std::uint64_t read_varint(std::span<std::byte const> bytes, std::size_t & at) {
      std::uint64_t value = 0;
      for (unsigned shift = 0; shift <= 63; shift += 7) {
        if (at == bytes.size()) throw std::invalid_argument("truncated front-coded header");
        auto part = std::to_integer<unsigned>(bytes[at++]);
        if (shift == 63 && part > 1) throw std::invalid_argument("overflowing front-coded header");
        value |= std::uint64_t(part & 127) << shift;
        if (!(part & 128)) return value;
      }
      throw std::invalid_argument("overflowing front-coded header");
    }
  }

  template <std::size_t N>
  struct front_record {
    std::string key;
    std::array<std::byte, N> value{};
  };

  template <std::size_t N>
  struct front_decoded_record {
    std::string prefix;
    std::uint64_t full_size = 0;
    std::array<std::byte, N> value{};

    front_anchor anchor() const noexcept { return {prefix, full_size}; }
  };

  template <std::size_t N>
  struct front_item {
    std::uint64_t ordinal = 0;
    front_anchor key;
    std::span<std::byte const, N> value;
  };

  template <std::size_t N>
  struct front_encoded_record {
    std::uint64_t shared = 0;
    std::span<std::byte const> suffix;
    std::span<std::byte const, N> value;
    std::uint64_t next_offset = 0;

    std::uint64_t key_size() const {
      if (shared > std::numeric_limits<std::uint64_t>::max() - suffix.size()) {
        throw std::invalid_argument("front-coded key length overflows");
      }
      return shared + suffix.size();
    }
  };

  template <std::size_t N>
  struct front_view {
    using offsets_view = decltype(std::declval<select15_index const &>().view());

    front_view(std::span<std::byte const> bytes, offsets_view offsets, std::uint64_t count)
      : bytes_(bytes), offsets_(offsets), count_(count) {}

    std::uint64_t size() const noexcept { return count_; }
    std::span<std::byte const> bytes() const noexcept { return bytes_; }
    offsets_view group_offsets() const noexcept { return offsets_; }

    // Navigation reads headers and skips suffix payloads without decoding keys.
    front_encoded_record<N> encoded_at(std::uint64_t ordinal) const {
      if (ordinal >= count_) throw std::out_of_range("front-coded record ordinal");
      auto group = ordinal / 15;
      auto at = offsets_.offset(group, N);
      for (auto i = group * 15; i < ordinal; ++i) at = parse(at).next_offset;
      return parse(at);
    }

    // Random reconstruction works for all streams but may traverse their whole
    // prefix chain. A stream built with LPFC bounds its backwards byte span by
    // the full target key's length, not by this requested prefix_limit.
    front_decoded_record<N> reconstruct_at(std::uint64_t ordinal,
        std::uint64_t prefix_limit = std::numeric_limits<std::uint64_t>::max()) const {
      auto encoded = encoded_at(ordinal);
      front_decoded_record<N> result;
      result.full_size = encoded.key_size();
      std::copy(encoded.value.begin(), encoded.value.end(), result.value.begin());
      auto need = std::min(result.full_size, prefix_limit);
      if (need > result.prefix.max_size()) throw std::length_error("front-coded key too large");
      result.prefix.resize(static_cast<std::size_t>(need));
      while (need) {
        if (encoded.key_size() < need) {
          throw std::invalid_argument("front-coded predecessor is shorter than copied prefix");
        }
        if (encoded.shared < need) {
          auto count = need - encoded.shared;
          std::copy_n(reinterpret_cast<char const *>(encoded.suffix.data()),
                      static_cast<std::size_t>(count), result.prefix.data() + encoded.shared);
          need = encoded.shared;
        }
        if (!need) break;
        if (!ordinal) throw std::invalid_argument("front-coded first key is not literal");
        encoded = encoded_at(--ordinal);
      }
      return result;
    }

    // The first record's copied bytes must match the supplied anchor. A
    // sufficient condition is actual predecessor <= anchor <= first key.
    // Redundant coding can also permit an anchor above the first key. Only
    // bytes through min(prefix_limit, stored_shared) must be supplied.
    // Stored shared is a COPY count, not an exact LCP with the supplied anchor.
    // callback's string view refers to scratch and expires on its next call.
    template <class F>
    void visit_window(std::uint64_t first, std::uint64_t last, front_anchor anchor,
                      std::uint64_t prefix_limit, F && callback) const {
      if (first > last || last > count_ || last - first > 15) {
        throw std::out_of_range("front-coded window must contain at most fifteen records");
      }
      if (first == last) return;
      std::string scratch(anchor.prefix.substr(0, static_cast<std::size_t>(
        std::min<std::uint64_t>(prefix_limit, anchor.prefix.size()))));
      auto context_size = anchor.full_size;
      auto encoded = encoded_at(first);
      for (auto i = first; i != last; ++i) {
        decode_into(encoded, prefix_limit, scratch, context_size);
        front_item<N> item{i, {scratch, context_size}, encoded.value};
        if (!callback(item)) break;
        if (i + 1 != last) encoded = parse(encoded.next_offset);
      }
    }

    // Sequential construction, merge input and explicit reference scans start
    // at the literal first key. No ordinal-to-key random access is promised.
    template <class F>
    void visit_all(F && callback) const {
      std::string scratch;
      std::uint64_t context_size = 0;
      std::uint64_t at = 0;
      for (std::uint64_t i = 0; i != count_; ++i) {
        auto encoded = parse(at);
        decode_into(encoded, std::numeric_limits<std::uint64_t>::max(), scratch, context_size);
        front_item<N> item{i, {scratch, context_size}, encoded.value};
        if (!callback(item)) break;
        at = encoded.next_offset;
      }
    }

  private:
    std::span<std::byte const> bytes_;
    offsets_view offsets_;
    std::uint64_t count_;

    front_encoded_record<N> parse(std::uint64_t offset) const {
      if (offset > bytes_.size()) throw std::invalid_argument("front-coded offset outside stream");
      auto at = static_cast<std::size_t>(offset);
      auto shared = front_detail::read_varint(bytes_, at);
      auto suffix_size = front_detail::read_varint(bytes_, at);
      if (bytes_.size() - at < N || suffix_size > bytes_.size() - at - N) {
        throw std::invalid_argument("truncated front-coded record");
      }
      auto suffix = bytes_.subspan(at, static_cast<std::size_t>(suffix_size));
      at += static_cast<std::size_t>(suffix_size);
      std::span<std::byte const, N> value(bytes_.data() + at, N);
      return {shared, suffix, value, at + N};
    }

    static void decode_into(front_encoded_record<N> const & encoded, std::uint64_t limit,
                            std::string & scratch, std::uint64_t & context_size) {
      if (encoded.shared > context_size) {
        throw std::invalid_argument("front-coded copy exceeds anchor length");
      }
      auto keep = std::min(encoded.shared, limit);
      if (keep > scratch.size()) {
        throw std::invalid_argument("front-coded anchor prefix is too short");
      }
      auto full_size = encoded.key_size();
      auto decoded_size = std::min(full_size, limit);
      if (decoded_size > scratch.max_size()) throw std::length_error("front-coded key too large");
      scratch.resize(static_cast<std::size_t>(keep));
      auto copy = decoded_size - keep;
      if (copy) {
        scratch.append(reinterpret_cast<char const *>(encoded.suffix.data()),
                       static_cast<std::size_t>(copy));
      }
      context_size = full_size;
    }
  };

  template <std::size_t N>
  struct front_array {
    static front_array build(std::span<front_record<N> const> records,
                             std::span<std::uint64_t const> prefix_ceilings = {},
                             std::uint64_t restart_factor = 0) {
      if (!prefix_ceilings.empty() && prefix_ceilings.size() != records.size()) {
        throw std::invalid_argument("one front-code prefix ceiling is required per record");
      }
      if (restart_factor && restart_factor < 3) {
        throw std::invalid_argument("LPFC restart factor must be at least three");
      }
      front_array result;
      std::vector<std::uint64_t> offsets;
      std::string_view previous;
      std::uint64_t fixed = 0;
      std::uint64_t anchor = 0;
      for (std::size_t i = 0; i != records.size(); ++i) {
        auto const & record = records[i];
        if (i && compare_keys(previous, record.key) > 0) {
          throw std::invalid_argument("front-coded input must be sorted");
        }
        if (i % 15 == 0) offsets.push_back(result.bytes_.size() - fixed);
        auto shared = common_prefix(previous, record.key);
        if (!prefix_ceilings.empty()) shared = std::min<std::uint64_t>(shared, prefix_ceilings[i]);
        // Count actual encoded bytes, including framing and fixed slots, to
        // bound traversal span. The paper's key-only compression-space theorem
        // is not claimed verbatim for this pragmatic framing convention.
        auto start = std::uint64_t(result.bytes_.size());
        if (shared && restart_factor &&
            record.key.size() <= std::numeric_limits<std::uint64_t>::max() / restart_factor &&
            start - anchor > restart_factor * record.key.size()) {
          shared = 0;
        }
        if (!shared) anchor = start;
        front_detail::append_varint(result.bytes_, shared);
        front_detail::append_varint(result.bytes_, record.key.size() - shared);
        auto suffix = std::as_bytes(std::span(record.key.data() + shared, record.key.size() - shared));
        result.bytes_.insert(result.bytes_.end(), suffix.begin(), suffix.end());
        result.bytes_.insert(result.bytes_.end(), record.value.begin(), record.value.end());
        fixed += N;
        previous = record.key;
      }
      offsets.push_back(result.bytes_.size() - fixed);
      result.offsets_ = select15_index::build(offsets, records.size());
      result.count_ = records.size();
      return result;
    }

    std::uint64_t size() const noexcept { return count_; }
    std::span<std::byte const> bytes() const noexcept { return bytes_; }
    select15_index const & group_offsets() const noexcept { return offsets_; }
    front_view<N> view() const & noexcept { return {bytes_, offsets_.view(), count_}; }
    front_view<N> view() const && = delete;

  private:
    std::vector<std::byte> bytes_;
    select15_index offsets_ = select15_index::build(std::array<std::uint64_t, 1>{0}, 0);
    std::uint64_t count_ = 0;
  };
}

/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
 * \endlicense
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Everett's front support.
 */
