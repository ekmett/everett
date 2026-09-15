/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/object_writer.h>
#include <everett/profile_blob.h>
#include <everett/word_view.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace everett {
  struct blob_identity {
    object_id native;
    object_id index;
    bool operator==(blob_identity const &) const = default;
  };

  namespace section_detail {
    inline constexpr std::uint16_t version = 2;
    inline constexpr std::size_t native_directory_bytes = 128;
    inline constexpr std::size_t index_directory_bytes = 256;
    inline constexpr std::size_t native_descriptor_offset = 48;
    inline constexpr std::size_t index_descriptor_offset = 112;
    inline constexpr std::size_t descriptor_bytes = 16;
    inline constexpr std::size_t fc = 0, low = 1, high = 2, samples = 3, sparse = 4;
    inline constexpr std::size_t classes = 5, checkpoints = 6, false_borrows = 7, cut_lcps = 8;

    struct descriptor {
      std::uint64_t offset = 0;
      std::uint64_t length = 0;
      bool operator==(descriptor const &) const = default;
    };
    struct directory {
      std::array<descriptor, 9> sections{};
      std::size_t count = 0;
      std::size_t bytes = 0;
      std::uint64_t extent = 0;
      std::uint64_t terminal_key_units = 0;
      std::uint64_t universe = 0;
      unsigned low_width = 0;
      std::uint64_t virtual_count = 0;
      std::optional<object_id> native_id;
      std::optional<blob_identity> target_id;
    };

    inline std::uint64_t align(std::uint64_t value) noexcept {
      return (value + 7) & ~std::uint64_t{7};
    }
    inline void zero(std::span<std::byte const> bytes, char const * message) {
      for (auto byte : bytes) if (byte != std::byte{0}) throw std::invalid_argument(message);
    }
    inline unsigned hex_value(char c) noexcept { return unsigned(c <= '9' ? c - '0' : c - 'a' + 10); }
    inline void put_id(std::span<std::byte> bytes, std::size_t at, object_id const & id) {
      auto const & text = id.hex();
      if (text.size() != 32) throw std::invalid_argument("invalid Everett section object identity");
      if (at > bytes.size() || 16 > bytes.size() - at)
        throw std::out_of_range("Everett section identity position");
      for (std::size_t i = 0; i != 16; ++i)
        bytes[at + i] = std::byte((hex_value(text[2 * i]) << 4) | hex_value(text[2 * i + 1]));
    }
    inline object_id get_id(std::span<std::byte const> bytes, std::size_t at) {
      constexpr char hex[] = "0123456789abcdef";
      std::string text(32, '0');
      for (std::size_t i = 0; i != 16; ++i) {
        auto byte = std::to_integer<unsigned>(bytes[at + i]);
        text[2 * i] = hex[byte >> 4];
        text[2 * i + 1] = hex[byte & 15];
      }
      return object_id(std::move(text));
    }
    inline std::span<std::byte const> section(std::span<std::byte const> body,
        directory const & layout, std::size_t slot) {
      if (slot >= layout.count) throw std::out_of_range("Everett section slot");
      auto const & part = layout.sections[slot];
      if (part.offset > body.size() || part.length > body.size() - part.offset)
        throw std::invalid_argument("Everett section exceeds body");
      return body.subspan(static_cast<std::size_t>(part.offset), static_cast<std::size_t>(part.length));
    }

    template <class P> directory parse(file_header<P> const & header, std::span<std::byte const> body) {
      directory result;
      bool index = header.kind == file_kind::fractional_index;
      result.count = index ? 9 : 5;
      result.bytes = index ? index_directory_bytes : native_directory_bytes;
      auto descriptors = index ? index_descriptor_offset : native_descriptor_offset;
      if (body.size() > std::numeric_limits<std::uint64_t>::max() - 7)
        throw std::invalid_argument("Everett section container extent overflows");
      if (body.size() < result.bytes) throw std::invalid_argument("truncated Everett section directory");
      if (header.extent != profile_detail::multiply(body.size(), 1u << (3 - P::unit_shift)))
        throw std::invalid_argument("Everett section container must occupy complete bytes");
      auto magic = index ? "IX02" : "KV02";
      for (unsigned i = 0; i != 4; ++i)
        if (std::to_integer<unsigned>(body[i]) != unsigned(magic[i]))
          throw std::invalid_argument("unexpected Everett section magic");
      if (file_detail::get(body, 4, 2) != version || file_detail::get(body, 6, 2) != result.count)
        throw std::invalid_argument("unsupported Everett section directory");
      result.extent = file_detail::get(body, 8, 8);
      result.terminal_key_units = file_detail::get(body, 16, 8);
      result.universe = file_detail::get(body, 24, 8);
      result.low_width = unsigned(file_detail::get(body, 32, 1));
      if (index) {
        auto target = file_detail::get(body, 33, 1);
        if (target > 1) throw std::invalid_argument("invalid Everett target flag");
        zero(body.subspan(34, 6), "nonzero Everett directory reserved bytes");
        zero(body.subspan(96, 16), "nonzero Everett directory reserved bytes");
        result.virtual_count = file_detail::get(body, 40, 8);
        if (result.virtual_count < header.record_count)
          throw std::invalid_argument("borrowed records exceed virtual count");
        result.native_id = get_id(body, 48);
        if (target) result.target_id = blob_identity{get_id(body, 64), get_id(body, 80)};
        else zero(body.subspan(64, 32), "absent Everett target must encode zero");
      } else zero(body.subspan(33, 15), "nonzero Everett directory reserved bytes");
      std::uint64_t end = result.bytes;
      for (std::size_t i = 0; i != result.count; ++i) {
        auto at = descriptors + i * descriptor_bytes;
        descriptor part{file_detail::get(body, at, 8), file_detail::get(body, at + 8, 8)};
        if (part.offset != align(end) || part.offset > body.size() || part.length > body.size() - part.offset)
          throw std::invalid_argument("noncanonical Everett section range");
        end = part.offset + part.length;
        result.sections[i] = part;
      }
      if (end != body.size()) throw std::invalid_argument("trailing Everett section bytes");
      return result;
    }
    inline void validate_gaps(std::span<std::byte const> body, directory const & layout) {
      std::size_t end = layout.bytes;
      for (std::size_t i = 0; i != layout.count; ++i) {
        auto const & part = layout.sections[i];
        zero(body.subspan(end, static_cast<std::size_t>(part.offset) - end), "nonzero Everett section alignment padding");
        end = static_cast<std::size_t>(part.offset + part.length);
      }
    }
    template <class P, stream_role Role> profile_view<P, Role> profile(
        file_header<P> const & header, std::span<std::byte const> body, directory const & layout) {
      auto metadata = profile_detail::initial_metadata<P, Role>();
      metadata.record_count = header.record_count;
      metadata.extent = layout.extent;
      metadata.terminal_key_units = layout.terminal_key_units;
      metadata.common_value_width = header.common_value_width;
      auto part = [&](std::size_t slot) { return section(body, layout, slot); };
      elias_fano_view offsets{
        word_view::little_endian(part(low)), word_view::little_endian(part(high)),
        sample_view::little_endian(part(samples)), word_view::little_endian(part(sparse)),
        profile_detail::add(header.record_count / P::codec_block_size +
          (header.record_count % P::codec_block_size != 0), 1), layout.universe, layout.low_width};
      auto result = profile_view<P, Role>::from_sections(part(fc), offsets, metadata);
      result.validate_offset_metadata();
      return result;
    }
    inline void equal_words(word_view actual, std::span<std::uint64_t const> expected, char const * message) {
      if (actual.size() != expected.size()) throw std::invalid_argument(message);
      for (std::size_t i = 0; i != expected.size(); ++i)
        if (actual[i] != expected[i]) throw std::invalid_argument(message);
    }
    template <class P, stream_role Role> void scan_profile(profile_view<P, Role> const & view) {
      view.validate_contents();
      auto const & metadata = view.metadata();
      auto offsets = view.group_offsets();
      std::vector<std::uint64_t> residuals;
      auto groups = view.block_count();
      if (groups == std::numeric_limits<std::uint64_t>::max() || groups + 1 > residuals.max_size())
        throw std::length_error("Everett scan offset count");
      residuals.reserve(static_cast<std::size_t>(groups + 1));
      bit_view data(view.bytes(), profile_detail::multiply(metadata.extent, P::bits_per_unit));
      std::uint64_t at = 0, previous_units = 0;
      bit_string previous;
      for (std::uint64_t ordinal = 0; ordinal != metadata.record_count; ++ordinal) {
        std::uint64_t retained;
        if (ordinal % P::codec_block_size == 0) {
          auto stride = profile_detail::multiply(ordinal, metadata.common_value_width.value_or(0));
          if (stride > at) throw std::invalid_argument("Everett fixed values exceed physical position");
          residuals.push_back(at - stride);
          retained = profile_detail::read_count<P>(data, at);
          if (retained > previous_units)
            throw std::invalid_argument("Everett profile absolute prefix exceeds predecessor");
        } else {
          auto backspace = profile_detail::read_backspace<P>(data, at);
          if (backspace > previous_units) throw std::invalid_argument("Everett profile backspace exceeds predecessor");
          retained = previous_units - backspace;
        }
        auto suffix = profile_detail::read_count<P>(data, at);
        auto values = metadata.common_value_width ? *metadata.common_value_width : profile_detail::read_count<P>(data, at);
        auto length = profile_detail::add(retained, suffix);
        auto start_bits = profile_detail::multiply(at, P::bits_per_unit);
        auto suffix_bits = profile_detail::multiply(suffix, P::bits_per_unit);
        auto value_bits = profile_detail::multiply(values, P::bits_per_unit);
        if (start_bits > data.size() || suffix_bits > data.size() - start_bits ||
            value_bits > data.size() - start_bits - suffix_bits)
          throw std::invalid_argument("truncated Everett profile record");
        auto retained_bits = profile_detail::multiply(retained, P::bits_per_unit);
        // The retained prefix is already equal. Ordinary FC must differ at
        // the first new unit, or extend the entire predecessor; no inherited
        // prefix needs copying or comparison to establish maximality/order.
        if (retained < previous_units) {
          if (!suffix) throw std::invalid_argument("Everett profile key precedes its predecessor");
          auto before = profile_detail::load_bits(previous.view(), retained_bits, P::bits_per_unit);
          auto after = profile_detail::load_bits(data, start_bits, P::bits_per_unit);
          if (after <= before)
            throw std::invalid_argument("Everett profile is not sorted ordinary front coding");
        } else if (ordinal && !suffix && Role == stream_role::native)
          throw std::invalid_argument("Everett native profile keys must be unique");
        profile_detail::resize(previous, retained_bits);
        profile_detail::append(previous, data.subview(start_bits, suffix_bits));
        at = profile_detail::add(at, profile_detail::add(suffix, values));
        previous_units = length;
      }
      if (at != metadata.extent || previous_units != metadata.terminal_key_units)
        throw std::invalid_argument("Everett terminal profile extent or length mismatch");
      auto stride = profile_detail::multiply(metadata.record_count, metadata.common_value_width.value_or(0));
      if (stride > at) throw std::invalid_argument("Everett fixed payload exceeds extent");
      residuals.push_back(at - stride);
      auto expected = elias_fano::build(residuals);
      if (expected.universe != offsets.universe() || expected.low_width != offsets.low_width())
        throw std::invalid_argument("noncanonical Everett Elias-Fano parameters");
      equal_words(offsets.low_words(), expected.low, "Everett Elias-Fano low words mismatch");
      equal_words(offsets.high_words(), expected.high, "Everett Elias-Fano high words mismatch");
      equal_words(offsets.sparse_words(), expected.sparse, "Everett Elias-Fano sparse words mismatch");
      auto actual_samples = offsets.samples();
      if (actual_samples.size() != expected.samples.size()) throw std::invalid_argument("Everett Elias-Fano sample count mismatch");
      for (std::size_t i = 0; i != expected.samples.size(); ++i) {
        auto a = actual_samples[i];
        auto b = expected.samples[i];
        if (a.first != b.first || a.sparse != b.sparse) throw std::invalid_argument("Everett Elias-Fano sample mismatch");
      }
    }
  }

  // Owns only the small directory and any endian-conversion buffers. Source
  // ordinary-FC arrays are borrowed and must remain immutable and alive through
  // every use. Encoding does not scan them to certify that precondition.
  // Move-only because copied section spans could refer to conversion buffers.
  template <class P> struct encoded_sections {
    using policy_type = P;
    encoded_sections(encoded_sections const &) = delete;
    encoded_sections & operator=(encoded_sections const &) = delete;
    encoded_sections(encoded_sections && other) noexcept
      : header_(std::move(other.header_)), directory_(other.directory_), zero_{},
        directory_size_(std::exchange(other.directory_size_, 0)), sections_(std::move(other.sections_)),
        converted_(std::move(other.converted_)) {}
    encoded_sections & operator=(encoded_sections && other) noexcept {
      if (this != &other) {
        header_ = std::move(other.header_); directory_ = other.directory_;
        directory_size_ = std::exchange(other.directory_size_, 0);
        sections_ = std::move(other.sections_); converted_ = std::move(other.converted_);
      }
      return *this;
    }
    file_header<P> const & header() const & { require_active(); return header_; }
    file_header<P> const & header() const && = delete;
    // Returned spans borrow this encoder as well as its source arrays, and
    // expire on encoder movement or destruction.
    std::vector<std::span<std::byte const>> chunks() const & {
      require_active();
      std::vector<std::span<std::byte const>> result;
      result.reserve(2 * sections_.size() + 1);
      result.push_back(std::span(directory_).first(directory_size_));
      std::uint64_t end = directory_size_;
      for (auto section : sections_) {
        auto gap = section_detail::align(end) - end;
        if (gap) result.push_back(std::span(zero_).first(static_cast<std::size_t>(gap)));
        result.push_back(section);
        end += gap + section.size();
      }
      return result;
    }
    std::vector<std::span<std::byte const>> chunks() const && = delete;
    std::vector<std::byte> materialize() const {
      require_active();
      std::vector<std::byte> body;
      auto size = file_detail::body_bytes<P>(header_.extent);
      if (size > body.max_size()) throw std::length_error("Everett section body too large");
      body.reserve(static_cast<std::size_t>(size));
      for (auto chunk : chunks()) body.insert(body.end(), chunk.begin(), chunk.end());
      return encode_file(header_, body);
    }
    object_seal_receipt seal(std::filesystem::path const & root, object_id const & id,
        object_attempt_id const & attempt) const {
      auto parts = chunks();
      return object_writer<P>::seal(root, id, attempt, header_, parts);
    }

  private:
    template <class Q> friend encoded_sections<Q> encode_native_sections(profile_array<Q, stream_role::native> const &);
    template <class Q> friend encoded_sections<Q> encode_index_sections(profile_blob<Q> const &, object_id const &,
                                                                      std::optional<blob_identity>);
    encoded_sections() = default;
    void require_active() const {
      if (!directory_size_) throw std::logic_error("Everett section encoding has no source");
    }
    file_header<P> header_;
    std::array<std::byte, section_detail::index_directory_bytes> directory_{};
    std::array<std::byte, 8> zero_{};
    std::size_t directory_size_ = 0;
    std::vector<std::span<std::byte const>> sections_;
    std::vector<std::vector<std::byte>> converted_;

    void words(std::span<std::uint64_t const> values) {
      if constexpr (std::endian::native == std::endian::little) sections_.push_back(std::as_bytes(values));
      else {
        auto & bytes = converted_.emplace_back(values.size() << 3);
        for (std::size_t i = 0; i != values.size(); ++i) file_detail::put(bytes, (i << 3), 8, values[i]);
        sections_.push_back(bytes);
      }
    }
    void samples(std::span<elias_fano_sample const> values) {
      static_assert(sizeof(elias_fano_sample) == 16 && offsetof(elias_fano_sample, first) == 0 &&
                    offsetof(elias_fano_sample, sparse) == 8);
      if constexpr (std::endian::native == std::endian::little) sections_.push_back(std::as_bytes(values));
      else {
        auto & bytes = converted_.emplace_back(values.size() << 4);
        for (std::size_t i = 0; i != values.size(); ++i) {
          file_detail::put(bytes, (i << 4), 8, values[i].first);
          file_detail::put(bytes, (i << 4) + 8, 8, values[i].sparse);
        }
        sections_.push_back(bytes);
      }
    }
    template <stream_role Role> void profile(profile_array<P, Role> const & array) {
      sections_.push_back(array.bytes());
      auto const & offsets = array.group_offsets();
      words(offsets.low); words(offsets.high); samples(offsets.samples); words(offsets.sparse);
      auto const & metadata = array.metadata();
      header_.record_count = metadata.record_count;
      header_.common_value_width = metadata.common_value_width;
      file_detail::put(directory_, 8, 8, metadata.extent);
      file_detail::put(directory_, 16, 8, metadata.terminal_key_units);
      file_detail::put(directory_, 24, 8, offsets.universe);
      file_detail::put(directory_, 32, 1, offsets.low_width);
    }
    void finish(file_kind kind) {
      header_.kind = kind;
      bool index = kind == file_kind::fractional_index;
      directory_size_ = index ? section_detail::index_directory_bytes : section_detail::native_directory_bytes;
      auto descriptors = index ? section_detail::index_descriptor_offset : section_detail::native_descriptor_offset;
      auto magic = index ? "IX02" : "KV02";
      for (unsigned i = 0; i != 4; ++i) directory_[i] = std::byte(magic[i]);
      file_detail::put(directory_, 4, 2, section_detail::version);
      file_detail::put(directory_, 6, 2, sections_.size());
      std::uint64_t end = directory_size_;
      for (std::size_t i = 0; i != sections_.size(); ++i) {
        if (end > std::numeric_limits<std::uint64_t>::max() - 7)
          throw std::overflow_error("Everett section alignment overflows");
        auto start = section_detail::align(end);
        end = profile_detail::add(start, sections_[i].size());
        file_detail::put(directory_, descriptors + (i << 4), 8, start);
        file_detail::put(directory_, descriptors + (i << 4) + 8, 8, sections_[i].size());
      }
      header_.extent = profile_detail::multiply(end, 1u << (3 - P::unit_shift));
      file_detail::validate_metadata(header_);
    }
  };

  template <class P> encoded_sections<P> encode_native_sections(profile_array<P, stream_role::native> const & native) {
    encoded_sections<P> result;
    result.profile(native);
    result.finish(file_kind::native_blob);
    return result;
  }
  template <class P> encoded_sections<P> encode_native_sections(profile_array<P, stream_role::native> const &&) = delete;

  template <class P> encoded_sections<P> encode_index_sections(profile_blob<P> const & pair,
      object_id const & native_id, std::optional<blob_identity> exact_target = std::nullopt) {
    if ((!exact_target && (pair.target() || pair.borrowed().size())) ||
        (pair.target() && pair.borrowed().size() != pair.target()->group_count()))
      throw std::invalid_argument("Everett index encoding needs its exact bound target");
    encoded_sections<P> result;
    result.profile(pair.borrowed());
    result.words(pair.interleave().classes);
    result.words(pair.interleave().checkpoints);
    result.sections_.push_back(pair.false_borrow_bits());
    result.words(pair.cut_lcps());
    file_detail::put(result.directory_, 40, 8, pair.virtual_size());
    section_detail::put_id(result.directory_, 48, native_id);
    if (exact_target) {
      file_detail::put(result.directory_, 33, 1, 1);
      section_detail::put_id(result.directory_, 64, exact_target->native);
      section_detail::put_id(result.directory_, 80, exact_target->index);
    }
    result.finish(file_kind::fractional_index);
    return result;
  }
  template <class P> encoded_sections<P> encode_index_sections(profile_blob<P> const &&, object_id const &,
                                                              std::optional<blob_identity> = std::nullopt) = delete;

  // Typed construction reads the envelope and fixed section directory only.
  // It retains the mapping; view/section spans borrow that owner's lifetime.
  // Semantic validation and the whole-body CRC are explicit scan operations.
  template <class P, stream_role Role> struct mapped_profile {
    using policy_type = P;
    static mapped_profile open(file<P> source) {
      auto header = source.header();
      constexpr auto kind = Role == stream_role::native ? file_kind::native_blob : file_kind::fractional_index;
      if (header.kind != kind) throw std::invalid_argument("Everett mapped profile kind mismatch");
      auto body = source.body();
      auto layout = section_detail::parse(header, body.bytes());
      auto view = section_detail::profile<P, Role>(header, body.bytes(), layout);
      return {std::move(source), std::move(body), std::move(layout), std::move(view)};
    }
    static mapped_profile open(std::filesystem::path const & path) { return open(file<P>::open(path)); }
    static mapped_profile from_slice(mapped_slice source, file_open_mode mode = file_open_mode::checked) {
      return open(file<P>::from_slice(std::move(source), mode));
    }
    bool active() const noexcept { return !body_.empty(); }
    profile_view<P, Role> view() const & { require_active(); return view_; }
    profile_view<P, Role> view() const && = delete;
    std::uint64_t size() const { require_active(); return view_.size(); }
    section_detail::directory const & layout() const & { require_active(); return layout_; }
    section_detail::directory const & layout() const && = delete;
    std::span<std::byte const> section(std::size_t slot) const & {
      require_active(); return section_detail::section(body_.bytes(), layout_, slot);
    }
    std::span<std::byte const> section(std::size_t) const && = delete;
    void scan() const {
      require_active();
      source_.scan();
      section_detail::validate_gaps(body_.bytes(), layout_);
      section_detail::scan_profile(view_);
    }
  private:
    void require_active() const {
      if (!active()) throw std::logic_error("Everett mapped profile has no mapping");
    }
    mapped_profile(file<P> source, mapped_slice body, section_detail::directory layout, profile_view<P, Role> view)
      : source_(std::move(source)), body_(std::move(body)), layout_(std::move(layout)), view_(std::move(view)) {}
    file<P> source_;
    mapped_slice body_;
    section_detail::directory layout_;
    profile_view<P, Role> view_;
  };
  template <class P> using mapped_native = mapped_profile<P, stream_role::native>;

  template <class P> struct mapped_index {
    using policy_type = P;
    static mapped_index open(file<P> source) {
      auto profile = mapped_profile<P, stream_role::borrowed>::open(std::move(source));
      auto words = [&](std::size_t slot) { return word_view::little_endian(profile.section(slot)); };
      auto const & layout = profile.layout();
      rank_groups_view<P::group_size> ranks(words(section_detail::classes), words(section_detail::checkpoints),
                                           layout.virtual_count);
      auto cuts = words(section_detail::cut_lcps);
      auto groups = layout.virtual_count / P::group_size + (layout.virtual_count % P::group_size != 0);
      if (profile.size() > std::numeric_limits<std::uint64_t>::max() - 7 ||
          cuts.size() != groups || profile.section(section_detail::false_borrows).size() !=
          ((profile.size() + 7) >> 3))
        throw std::invalid_argument("Everett index navigation shape mismatch");
      return {std::move(profile), std::move(ranks), cuts};
    }
    static mapped_index open(std::filesystem::path const & path) { return open(file<P>::open(path)); }
    static mapped_index from_slice(mapped_slice source, file_open_mode mode = file_open_mode::checked) {
      return open(file<P>::from_slice(std::move(source), mode));
    }
    profile_view<P, stream_role::borrowed> borrowed() const & { return profile_.view(); }
    profile_view<P, stream_role::borrowed> borrowed() const && = delete;
    rank_groups_view<P::group_size> interleave() const & {
      require_active(); return ranks_;
    }
    rank_groups_view<P::group_size> interleave() const && = delete;
    std::span<std::byte const> false_borrow_bits() const & { return profile_.section(section_detail::false_borrows); }
    std::span<std::byte const> false_borrow_bits() const && = delete;
    word_view cut_lcps() const & { require_active(); return cuts_; }
    word_view cut_lcps() const && = delete;
    object_id const & native_id() const & { return *profile_.layout().native_id; }
    object_id const & native_id() const && = delete;
    std::optional<blob_identity> const & target_id() const & { return profile_.layout().target_id; }
    std::optional<blob_identity> const & target_id() const && = delete;
    std::uint64_t virtual_size() const { return profile_.layout().virtual_count; }
    section_detail::directory const & layout() const & { return profile_.layout(); }
    section_detail::directory const & layout() const && = delete;
    std::span<std::byte const> section(std::size_t slot) const & { return profile_.section(slot); }
    std::span<std::byte const> section(std::size_t) const && = delete;
    void scan() const {
      profile_.scan();
      std::vector<std::uint64_t> classes;
      auto groups = ranks_.group_count();
      if (groups > classes.max_size()) throw std::length_error("Everett rank scan size");
      classes.reserve(static_cast<std::size_t>(groups));
      for (std::uint64_t group = 0; group != groups; ++group) classes.push_back(ranks_.class_at(group));
      auto expected = rank_groups<P::group_size>::build(classes, virtual_size());
      if (expected.view().count() != borrowed().size()) throw std::invalid_argument("Everett rank total mismatch");
      section_detail::equal_words(ranks_.class_words(), expected.classes, "Everett rank class words mismatch");
      section_detail::equal_words(ranks_.checkpoint_words(), expected.checkpoints, "Everett rank checkpoint mismatch");
      auto flags = false_borrow_bits();
      if (auto tail = (borrowed().size() & 7))
        if ((std::to_integer<unsigned>(flags.back()) >> tail) != 0)
          throw std::invalid_argument("nonzero Everett false-borrow padding");
    }
  private:
    void require_active() const {
      if (!profile_.active()) throw std::logic_error("Everett mapped index has no mapping");
    }
    mapped_index(mapped_profile<P, stream_role::borrowed> profile, rank_groups_view<P::group_size> ranks, word_view cuts)
      : profile_(std::move(profile)), ranks_(std::move(ranks)), cuts_(cuts) {}
    mapped_profile<P, stream_role::borrowed> profile_;
    rank_groups_view<P::group_size> ranks_;
    word_view cuts_;
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Encodes portable blob sections and retains mmap-backed profile directories.
 */
