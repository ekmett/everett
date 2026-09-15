/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/error_detail.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace everett {
  struct sample_view;

  // Borrows native words or portable little-endian bytes. Construction reads
  // no elements; loads use memcpy and need neither aligned addresses nor live
  // uint64_t objects in the backing mapping. The owner must outlive this view.
  struct word_view {
    word_view() = default;
    explicit word_view(std::span<std::uint64_t const> words) noexcept
      : bytes_(std::as_bytes(words)), little_(std::endian::native == std::endian::little) {}

    static word_view little_endian(std::span<std::byte const> bytes) {
      if (bytes.size() % 8) error_detail::raise<std::invalid_argument>("word section length is not a multiple of eight");
      return {bytes, true};
    }

    std::size_t size() const noexcept { return bytes_.size() / 8; }
    bool empty() const noexcept { return bytes_.empty(); }
    std::span<std::byte const> bytes() const noexcept { return bytes_; }
    bool is_little_endian() const noexcept { return little_; }
    word_view subspan(std::size_t offset, std::size_t count = std::dynamic_extent) const {
      if (offset > size()) error_detail::raise<std::out_of_range>("word section offset");
      if (count == std::dynamic_extent) count = size() - offset;
      if (count > size() - offset) error_detail::raise<std::out_of_range>("word section length");
      return {bytes_.subspan(offset * 8, count * 8), little_};
    }

    std::uint64_t operator[](std::size_t index) const {
      if (index >= size()) error_detail::raise<std::out_of_range>("word section index");
      std::uint64_t value;
      std::memcpy(&value, bytes_.data() + index * 8, 8);
      if constexpr (std::endian::native == std::endian::big) {
        if (little_) {
          value = ((value & 0x00ff00ff00ff00ffull) << 8) | ((value >> 8) & 0x00ff00ff00ff00ffull);
          value = ((value & 0x0000ffff0000ffffull) << 16) | ((value >> 16) & 0x0000ffff0000ffffull);
          value = (value << 32) | (value >> 32);
        }
      }
      return value;
    }

  private:
    friend struct sample_view;
    static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
                  "directory words require a byte-ordered host");
    word_view(std::span<std::byte const> bytes, bool little) noexcept : bytes_(bytes), little_(little) {}
    std::span<std::byte const> bytes_;
    bool little_ = true;
  };

  struct sample_value {
    std::uint64_t first;
    std::uint64_t sparse;
  };

  // Select samples are explicit pairs of words. Existing native sample structs
  // keep their public names; their layout is checked before borrowing bytes.
  struct sample_view {
    sample_view() = default;
    template <class Sample> explicit sample_view(std::span<Sample const> samples) noexcept
      : words_(std::as_bytes(samples), std::endian::native == std::endian::little) {
      static_assert(std::is_standard_layout_v<Sample> && std::is_trivially_copyable_v<Sample>);
      static_assert(std::is_same_v<decltype(Sample::first), std::uint64_t> &&
                    std::is_same_v<decltype(Sample::sparse), std::uint64_t>);
      static_assert(sizeof(Sample) == 16 && offsetof(Sample, first) == 0 && offsetof(Sample, sparse) == 8);
    }

    static sample_view little_endian(std::span<std::byte const> bytes) {
      if (bytes.size() % 16) error_detail::raise<std::invalid_argument>("sample section length is not a multiple of sixteen");
      return sample_view(word_view::little_endian(bytes));
    }

    std::size_t size() const noexcept { return words_.size() / 2; }
    bool empty() const noexcept { return !size(); }
    word_view words() const noexcept { return words_; }
    std::span<std::byte const> bytes() const noexcept { return words_.bytes(); }
    sample_value operator[](std::size_t index) const {
      if (index >= size()) error_detail::raise<std::out_of_range>("sample section index");
      return {words_[2 * index], words_[2 * index + 1]};
    }

  private:
    explicit sample_view(word_view words) noexcept : words_(words) {}
    word_view words_;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Borrows native or little-endian directory words and select samples.
 */
