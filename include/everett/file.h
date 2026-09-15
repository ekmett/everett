#pragma once

#include <everett/mapped_file.h>
#include <everett/object_path.h>
#include <everett/policy.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace everett {
  // CRC32C (Castagnoli), reflected polynomial, initial/final complement. This
  // byte-integrity check is independent of the algebraic logical-state hash.
  inline std::uint32_t crc32c(std::span<std::byte const> bytes) noexcept {
    std::uint32_t crc = ~std::uint32_t{0};
    for (auto byte : bytes) {
      crc ^= std::to_integer<std::uint32_t>(byte);
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0x82f63b78u);
    }
    return ~crc;
  }

  template <class P> struct file_header {
    using policy_type = P;
    file_kind kind = file_kind::native_blob;
    std::uint64_t extent = 0; // Body address units, exactly P::unit.
    std::uint64_t record_count = 0;
    std::optional<std::uint64_t> common_value_width;
    bool operator==(file_header const &) const = default;
  };

  namespace file_detail {
    inline constexpr std::size_t header_bytes = 96;
    inline constexpr std::uint16_t version = 1;

    // Version 1, explicit little-endian integers; never a raw C++ struct:
    //  0 magic[8]        8 version:u16       10 header_bytes:u16
    // 12 flags:u32      16 unit:u8          17 checksum_kind:u8 (=1)
    // 18 reserved[6]    24 group_K:u64      32 policy_value_width:u64
    // 40 common_width:u64                  48 body_extent:u64
    // 56 record_count:u64                  64 body_crc32c:u32
    // 68 header_crc32c:u32                 72 body_offset:u64 (=96)
    // 80 total_file_bytes:u64              88 reserved[8]
    // Flags: bit0 fixed policy values; bit1 common width present. Header CRC
    // covers all 96 bytes with bytes68..71 zero. Padding and reserved bits zero.
    inline std::uint64_t get(std::span<std::byte const> bytes, std::size_t at, unsigned width) noexcept {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[at + i])) << (8 * i);
      return value;
    }
    inline void put(std::span<std::byte> bytes, std::size_t at, unsigned width, std::uint64_t value) noexcept {
      for (unsigned i = 0; i < width; ++i) bytes[at + i] = std::byte((value >> (8 * i)) & 255u);
    }
    inline std::string_view magic(file_kind kind) {
      switch (kind) {
        case file_kind::native_blob: return {"EVRT.KV\0", 8};
        case file_kind::fractional_index: return {"EVRT.IX\0", 8};
      }
      throw std::invalid_argument("unsupported Everett file kind");
    }
    template <class P> std::uint64_t body_bytes(std::uint64_t extent) noexcept {
      if constexpr (P::unit == profile_unit::byte) return extent;
      else return extent / 8 + (extent % 8 != 0);
    }
    template <class P> std::uint64_t total_bytes(std::uint64_t extent) {
      auto bytes = body_bytes<P>(extent);
      if (bytes > std::numeric_limits<std::uint64_t>::max() - header_bytes)
        throw std::overflow_error("Everett file extent overflow");
      return bytes + header_bytes;
    }
    template <class P> void validate_metadata(file_header<P> const & header) {
      (void)magic(header.kind);
      if (header.kind == file_kind::fractional_index) {
        if (header.common_value_width != std::optional<std::uint64_t>{0})
          throw std::invalid_argument("fractional index values must have width zero");
      } else if (header.kind == file_kind::native_blob) {
        if constexpr (P::fixed_width)
          if (header.common_value_width && header.common_value_width != P::value_width)
            throw std::invalid_argument("native common width disagrees with fixed policy");
        auto width = header.common_value_width;
        if constexpr (P::fixed_width) width = P::value_width;
        if (width && *width && header.record_count > header.extent / *width)
          throw std::invalid_argument("fixed value slots exceed body extent");
      }
      (void)total_bytes<P>(header.extent);
    }
    template <class P> void validate_body(file_header<P> const & header, std::span<std::byte const> body) {
      if (body_bytes<P>(header.extent) != body.size())
        throw std::invalid_argument("Everett body extent mismatch");
      if constexpr (P::unit == profile_unit::bit) {
        if (header.extent % 8) {
          unsigned unused = 8 - unsigned(header.extent % 8);
          if (std::to_integer<unsigned>(body.back()) & ((1u << unused) - 1))
            throw std::invalid_argument("noncanonical bit-profile tail padding");
        }
      }
    }
    inline std::uint32_t header_checksum(std::span<std::byte const> bytes) {
      std::array<std::byte, header_bytes> copy{};
      for (std::size_t i = 0; i < copy.size(); ++i) copy[i] = bytes[i];
      put(copy, 68, 4, 0);
      return crc32c(copy);
    }
  }

  template <class P> file_header<P> decode_file_header(std::span<std::byte const> bytes) {
    if (bytes.size() < file_detail::header_bytes) throw std::invalid_argument("truncated Everett header");
    file_header<P> header;
    bool recognized = false;
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      auto magic = file_detail::magic(kind);
      bool match = true;
      for (std::size_t i = 0; i < 8; ++i)
        if (std::to_integer<unsigned char>(bytes[i]) != static_cast<unsigned char>(magic[i])) match = false;
      if (match) { header.kind = kind; recognized = true; break; }
    }
    if (!recognized) throw std::invalid_argument("unrecognized Everett magic");
    if (file_detail::get(bytes, 8, 2) != file_detail::version ||
        file_detail::get(bytes, 10, 2) != file_detail::header_bytes)
      throw std::invalid_argument("unsupported Everett file version or header length");
    if (file_detail::get(bytes, 68, 4) != file_detail::header_checksum(bytes))
      throw std::invalid_argument("Everett header CRC32C mismatch");
    auto flags = file_detail::get(bytes, 12, 4);
    if (flags & ~std::uint64_t{3}) throw std::invalid_argument("unsupported Everett header flags");
    if (file_detail::get(bytes, 16, 1) > 1 ||
        file_detail::get(bytes, 16, 1) != static_cast<unsigned>(P::unit))
      throw std::invalid_argument("Everett address unit disagrees with policy");
    if (file_detail::get(bytes, 17, 1) != 1) throw std::invalid_argument("unsupported Everett checksum kind");
    if (file_detail::get(bytes, 18, 6) || file_detail::get(bytes, 88, 8))
      throw std::invalid_argument("nonzero Everett reserved header bytes");
    if (file_detail::get(bytes, 24, 8) != P::group_size ||
        bool(flags & 1) != P::fixed_width || file_detail::get(bytes, 32, 8) != P::value_width.value_or(0))
      throw std::invalid_argument("Everett stored policy descriptor mismatch");
    if (flags & 2) header.common_value_width = file_detail::get(bytes, 40, 8);
    else if (file_detail::get(bytes, 40, 8)) throw std::invalid_argument("absent common width must encode zero");
    header.extent = file_detail::get(bytes, 48, 8);
    header.record_count = file_detail::get(bytes, 56, 8);
    file_detail::validate_metadata(header);
    if (file_detail::get(bytes, 72, 8) != file_detail::header_bytes ||
        file_detail::get(bytes, 80, 8) != file_detail::total_bytes<P>(header.extent))
      throw std::invalid_argument("noncanonical Everett body offset or physical extent");
    return header;
  }

  template <class P> file_header<P> validate_file(std::span<std::byte const> bytes) {
    auto header = decode_file_header<P>(bytes);
    if (file_detail::total_bytes<P>(header.extent) != bytes.size())
      throw std::invalid_argument("truncated or trailing Everett object bytes");
    auto body = bytes.subspan(file_detail::header_bytes);
    file_detail::validate_body(header, body);
    if (file_detail::get(bytes, 64, 4) != crc32c(body))
      throw std::invalid_argument("Everett body CRC32C mismatch");
    return header;
  }

  // Pure serialization of an already encoded body; no file writes or durability.
  template <class P> std::vector<std::byte> encode_file(file_header<P> const & header,
                                                      std::span<std::byte const> body) {
    file_detail::validate_metadata(header);
    file_detail::validate_body(header, body);
    auto total = file_detail::total_bytes<P>(header.extent);
    if (total > std::numeric_limits<std::size_t>::max()) throw std::length_error("Everett file is too large");
    std::vector<std::byte> result(static_cast<std::size_t>(total));
    auto magic = file_detail::magic(header.kind);
    for (std::size_t i = 0; i < 8; ++i) result[i] = std::byte(static_cast<unsigned char>(magic[i]));
    file_detail::put(result, 8, 2, file_detail::version);
    file_detail::put(result, 10, 2, file_detail::header_bytes);
    file_detail::put(result, 12, 4, (P::fixed_width ? 1u : 0u) | (header.common_value_width ? 2u : 0u));
    file_detail::put(result, 16, 1, static_cast<unsigned>(P::unit));
    file_detail::put(result, 17, 1, 1);
    file_detail::put(result, 24, 8, P::group_size);
    file_detail::put(result, 32, 8, P::value_width.value_or(0));
    file_detail::put(result, 40, 8, header.common_value_width.value_or(0));
    file_detail::put(result, 48, 8, header.extent);
    file_detail::put(result, 56, 8, header.record_count);
    file_detail::put(result, 64, 4, crc32c(body));
    file_detail::put(result, 72, 8, file_detail::header_bytes);
    file_detail::put(result, 80, 8, total);
    file_detail::put(result, 68, 4, file_detail::header_checksum(result));
    for (std::size_t i = 0; i < body.size(); ++i) result[file_detail::header_bytes + i] = body[i];
    return result;
  }

  // Validated typed single-object reader. The retained body slice keeps the
  // underlying read-only mapping alive independently of this file object.
  template <class P> struct file {
    using policy_type = P;
    static file from_slice(mapped_slice bytes) {
      auto header = validate_file<P>(bytes.bytes());
      return {std::move(bytes), std::move(header)};
    }
    static file open(std::filesystem::path const & path) {
      auto mapping = mapped_file::open(path);
      auto result = from_slice(mapping.slice(0, mapping.size()));
      if (path.extension().generic_string() != file_extension(result.header_.kind))
        throw std::invalid_argument("Everett extension disagrees with file magic");
      return result;
    }
    file_header<P> const & header() const & noexcept { return header_; }
    file_header<P> const & header() const && = delete;
    mapped_slice body() const { return bytes_.slice(file_detail::header_bytes, file_detail::body_bytes<P>(header_.extent)); }
  private:
    file(mapped_slice bytes, file_header<P> header) : bytes_(std::move(bytes)), header_(std::move(header)) {}
    mapped_slice bytes_;
    file_header<P> header_;
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
 * \brief Declares Everett's file support.
 */
