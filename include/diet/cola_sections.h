/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Encodes two-target COLA routing in portable IX03 sections.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/cola_index.h>
#include <diet/sections.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace diet {
  namespace cola_section_detail {
    inline constexpr std::uint16_t version = 3;
    inline constexpr std::size_t section_count = 18, descriptor_offset = 160;
    inline constexpr std::size_t directory_bytes = descriptor_offset + 16 * section_count;
    constexpr std::size_t profile_slot(unsigned route) noexcept { return 5 * route; }
    constexpr std::size_t rank_slot(unsigned route) noexcept { return 10 + 2 * route; }
    constexpr std::size_t flags_slot(unsigned route) noexcept { return 14 + route; }
    constexpr std::size_t cuts_slot(unsigned route) noexcept { return 16 + route; }

    struct directory {
      std::array<section_detail::descriptor, section_count> sections{};
      std::uint64_t virtual_count = 0;
      std::array<std::uint64_t, 2> counts{}, extents{}, terminal_units{}, universes{};
      std::array<unsigned, 2> low_widths{};
      object_id native_id;
      std::optional<blob_identity> main_id;
      std::optional<object_id> secondary_id;
    };

    inline std::span<std::byte const> section(std::span<std::byte const> body,
        directory const & layout, std::size_t slot) {
      if (slot >= section_count) error_detail::raise<std::out_of_range>("COLA section slot");
      auto const & part = layout.sections[slot];
      if (part.offset > body.size() || part.length > body.size() - part.offset)
        error_detail::raise<std::invalid_argument>("COLA section exceeds body");
      return body.subspan(static_cast<std::size_t>(part.offset), static_cast<std::size_t>(part.length));
    }
    template <class P> directory parse(file_header<P> const & header, std::span<std::byte const> body) {
      if (header.kind != file_kind::fractional_index || body.size() < directory_bytes ||
          body.size() > std::numeric_limits<std::uint64_t>::max() - 7)
        error_detail::raise<std::invalid_argument>("invalid COLA index container");
      if (header.extent != profile_detail::multiply(body.size(), 1u << (3 - P::unit_shift)))
        error_detail::raise<std::invalid_argument>("COLA index must occupy complete bytes");
      for (unsigned i = 0; i != 4; ++i)
        if (std::to_integer<unsigned>(body[i]) != unsigned("IX03"[i]))
          error_detail::raise<std::invalid_argument>("unexpected COLA index magic");
      if (file_detail::get(body, 4, 2) != version || file_detail::get(body, 6, 2) != section_count)
        error_detail::raise<std::invalid_argument>("unsupported COLA index directory");
      directory result{{}, file_detail::get(body, 8, 8), {}, {}, {}, {}, {}, section_detail::get_id(body, 88), {}, {}};
      for (unsigned route = 0; route != 2; ++route) {
        result.counts[route] = file_detail::get(body, 16 + 8 * route, 8);
        result.extents[route] = file_detail::get(body, 32 + 8 * route, 8);
        result.terminal_units[route] = file_detail::get(body, 48 + 8 * route, 8);
        result.universes[route] = file_detail::get(body, 64 + 8 * route, 8);
        result.low_widths[route] = unsigned(file_detail::get(body, 80 + route, 1));
      }
      auto main = file_detail::get(body, 82, 1), secondary = file_detail::get(body, 83, 1);
      if (main > 1 || secondary > 1)
        error_detail::raise<std::invalid_argument>("invalid COLA target flag");
      section_detail::zero(body.subspan(84, 4), "nonzero COLA reserved bytes");
      section_detail::zero(body.subspan(152, 8), "nonzero COLA reserved bytes");
      if (main) result.main_id = blob_identity{section_detail::get_id(body, 104), section_detail::get_id(body, 120)};
      else section_detail::zero(body.subspan(104, 32), "absent COLA main target is nonzero");
      if (secondary) result.secondary_id = section_detail::get_id(body, 136);
      else section_detail::zero(body.subspan(136, 16), "absent COLA secondary target is nonzero");
      if (result.counts[0] > result.virtual_count || result.counts[1] > result.virtual_count - result.counts[0] ||
          result.counts[0] + result.counts[1] != header.record_count ||
          (!main && result.counts[0]) || (!secondary && result.counts[1]))
        error_detail::raise<std::invalid_argument>("COLA index count or target mismatch");
      std::uint64_t end = directory_bytes;
      for (std::size_t i = 0; i != section_count; ++i) {
        auto at = descriptor_offset + (i << 4);
        section_detail::descriptor part{file_detail::get(body, at, 8), file_detail::get(body, at + 8, 8)};
        if (part.offset != section_detail::align(end) || part.offset > body.size() || part.length > body.size() - part.offset)
          error_detail::raise<std::invalid_argument>("noncanonical COLA section range");
        end = part.offset + part.length;
        result.sections[i] = part;
      }
      if (end != body.size()) error_detail::raise<std::invalid_argument>("trailing COLA section bytes");
      return result;
    }
    template <class P> profile_view<P, stream_role::borrowed> profile(
        std::span<std::byte const> body, directory const & layout, unsigned route) {
      auto metadata = profile_detail::initial_metadata<P, stream_role::borrowed>();
      metadata.record_count = layout.counts[route];
      metadata.extent = layout.extents[route];
      metadata.terminal_key_units = layout.terminal_units[route];
      metadata.common_value_width = 0;
      auto first = profile_slot(route);
      auto part = [&](unsigned offset) { return section(body, layout, first + offset); };
      auto count = metadata.record_count;
      auto samples = profile_detail::add(count / P::codec_block_size + (count % P::codec_block_size != 0), 1);
      elias_fano_view offsets{word_view::little_endian(part(1)), word_view::little_endian(part(2)),
        sample_view::little_endian(part(3)), word_view::little_endian(part(4)), samples,
        layout.universes[route], layout.low_widths[route]};
      auto result = profile_view<P, stream_role::borrowed>::from_sections(part(0), offsets, metadata);
      result.validate_offset_metadata();
      return result;
    }
  }

  // Borrow both encoded streams and navigation from an immutable owning index.
  // This small directory is new; native KV02 bytes are shared unchanged. Spans
  // returned by chunks borrow both this encoder and its source until sealing.
  template <class P> struct encoded_cola_sections {
    using policy_type = P;
    encoded_cola_sections(encoded_cola_sections const &) = delete;
    encoded_cola_sections & operator=(encoded_cola_sections const &) = delete;
    encoded_cola_sections(encoded_cola_sections && other) noexcept
      : header_(other.header_), directory_(other.directory_), sections_(std::move(other.sections_)),
        converted_(std::move(other.converted_)), active_(std::exchange(other.active_, false)) {}
    encoded_cola_sections & operator=(encoded_cola_sections && other) noexcept {
      if (this != &other) {
        header_ = other.header_; directory_ = other.directory_; sections_ = std::move(other.sections_);
        converted_ = std::move(other.converted_); active_ = std::exchange(other.active_, false);
      }
      return *this;
    }
    template <class Native, class Main>
    static encoded_cola_sections from(cola_index<P, Native, Main> const & source, object_id const & native_id,
        std::optional<blob_identity> main_id = {}, std::optional<object_id> secondary_id = {}) {
      if (bool(source.main_target()) != bool(main_id) || bool(source.secondary_target()) != bool(secondary_id))
        error_detail::raise<std::invalid_argument>("COLA encoding needs exact target identities");
      encoded_cola_sections result;
      auto & directory = result.directory_;
      for (unsigned i = 0; i != 4; ++i) directory[i] = std::byte("IX03"[i]);
      file_detail::put(directory, 4, 2, cola_section_detail::version);
      file_detail::put(directory, 6, 2, cola_section_detail::section_count);
      file_detail::put(directory, 8, 8, source.virtual_size());
      section_detail::put_id(directory, 88, native_id);
      if (main_id) {
        file_detail::put(directory, 82, 1, 1);
        section_detail::put_id(directory, 104, main_id->native);
        section_detail::put_id(directory, 120, main_id->index);
      }
      if (secondary_id) {
        file_detail::put(directory, 83, 1, 1);
        section_detail::put_id(directory, 136, *secondary_id);
      }
      result.sections_.reserve(cola_section_detail::section_count);
      for (unsigned route = 0; route != 2; ++route) {
        auto const & array = source.borrowed(route);
        auto const & metadata = array.metadata();
        auto const & offsets = array.group_offsets();
        result.header_.record_count = profile_detail::add(result.header_.record_count, metadata.record_count);
        file_detail::put(directory, 16 + 8 * route, 8, metadata.record_count);
        file_detail::put(directory, 32 + 8 * route, 8, metadata.extent);
        file_detail::put(directory, 48 + 8 * route, 8, metadata.terminal_key_units);
        file_detail::put(directory, 64 + 8 * route, 8, offsets.universe);
        file_detail::put(directory, 80 + route, 1, offsets.low_width);
        result.sections_.push_back(array.bytes());
        result.words(offsets.low); result.words(offsets.high); result.samples(offsets.samples); result.words(offsets.sparse);
      }
      for (unsigned route = 0; route != 2; ++route) {
        result.words(source.interleave(route).classes);
        result.words(source.interleave(route).checkpoints);
      }
      for (unsigned route = 0; route != 2; ++route) result.sections_.push_back(source.false_borrow_bits(route));
      for (unsigned route = 0; route != 2; ++route) result.words(source.cut_lcps(route));
      std::uint64_t end = cola_section_detail::directory_bytes;
      for (std::size_t i = 0; i != result.sections_.size(); ++i) {
        auto start = profile_detail::add(end, 7) & ~std::uint64_t{7};
        end = profile_detail::add(start, result.sections_[i].size());
        file_detail::put(directory, cola_section_detail::descriptor_offset + (i << 4), 8, start);
        file_detail::put(directory, cola_section_detail::descriptor_offset + (i << 4) + 8, 8, result.sections_[i].size());
      }
      result.header_.extent = profile_detail::multiply(end, 1u << (3 - P::unit_shift));
      file_detail::validate_metadata(result.header_);
      return result;
    }
    template <class Native, class Main>
    static encoded_cola_sections from(cola_index<P, Native, Main> const &&, object_id const &,
        std::optional<blob_identity> = {}, std::optional<object_id> = {}) = delete;

    file_header<P> const & header() const & { require_active(); return header_; }
    file_header<P> const & header() const && = delete;
    std::vector<std::span<std::byte const>> chunks() const & {
      require_active();
      std::vector<std::span<std::byte const>> result;
      result.reserve(2 * sections_.size() + 1);
      result.push_back(directory_);
      std::uint64_t end = directory_.size();
      for (auto bytes : sections_) {
        auto gap = section_detail::align(end) - end;
        if (gap) result.push_back(std::span(zero_).first(static_cast<std::size_t>(gap)));
        result.push_back(bytes); end += gap + bytes.size();
      }
      return result;
    }
    std::vector<std::span<std::byte const>> chunks() const && = delete;
    std::vector<std::byte> materialize() const {
      std::vector<std::byte> body;
      auto count = file_detail::body_bytes<P>(header().extent);
      if (count > body.max_size()) error_detail::raise<std::length_error>("COLA section body is too large");
      body.reserve(static_cast<std::size_t>(count));
      for (auto part : chunks()) body.insert(body.end(), part.begin(), part.end());
      return encode_file(header_, body);
    }
    object_seal_receipt seal(std::filesystem::path const & root, object_id const & id,
        object_attempt_id const & attempt) const {
      auto parts = chunks();
      return object_writer<P>::seal(root, id, attempt, header(), parts);
    }
  private:
    encoded_cola_sections() = default;
    file_header<P> header_{file_kind::fractional_index, 0, 0, 0};
    std::array<std::byte, cola_section_detail::directory_bytes> directory_{};
    std::array<std::byte, 8> zero_{};
    std::vector<std::span<std::byte const>> sections_;
    std::vector<std::vector<std::byte>> converted_;
    bool active_ = true;

    void require_active() const {
      if (!active_) error_detail::raise<std::logic_error>("COLA encoding has no source");
    }
    void words(std::span<std::uint64_t const> words) {
      if constexpr (std::endian::native == std::endian::little) sections_.push_back(std::as_bytes(words));
      else {
        auto & bytes = converted_.emplace_back(profile_detail::multiply(words.size(), 8));
        for (std::size_t i = 0; i != words.size(); ++i) file_detail::put(bytes, i << 3, 8, words[i]);
        sections_.push_back(bytes);
      }
    }
    void samples(std::span<elias_fano_sample const> samples) {
      static_assert(sizeof(elias_fano_sample) == 16 && offsetof(elias_fano_sample, first) == 0 &&
        offsetof(elias_fano_sample, sparse) == 8);
      if constexpr (std::endian::native == std::endian::little) sections_.push_back(std::as_bytes(samples));
      else {
        auto & bytes = converted_.emplace_back(profile_detail::multiply(samples.size(), 16));
        for (std::size_t i = 0; i != samples.size(); ++i) {
          file_detail::put(bytes, i << 4, 8, samples[i].first);
          file_detail::put(bytes, (i << 4) + 8, 8, samples[i].sparse);
        }
        sections_.push_back(bytes);
      }
    }
  };

  template <class P, class Native, class Main> encoded_cola_sections<P> encode_cola_sections(
      cola_index<P, Native, Main> const & source,
      object_id const & native_id, std::optional<blob_identity> main_id = {}, std::optional<object_id> secondary_id = {}) {
    return encoded_cola_sections<P>::from(source, native_id, std::move(main_id), std::move(secondary_id));
  }
  template <class P, class Native, class Main> encoded_cola_sections<P> encode_cola_sections(cola_index<P, Native, Main> const &&,
      object_id const &, std::optional<blob_identity> = {}, std::optional<object_id> = {}) = delete;

  // Open touches the fixed envelope/directory, not FC, EF or rank contents.
  // scan checks this file; mapped_cola_blob::scan also checks its exact targets.
  template <class P> struct mapped_cola_index {
    using policy_type = P;
    using borrowed_view = profile_view<P, stream_role::borrowed>;
    using rank_view = rank_groups_view<P::group_size>;
    static mapped_cola_index open(file<P> source) {
      auto body = source.body();
      auto layout = cola_section_detail::parse(source.header(), body.bytes());
      auto part = [&](std::size_t slot) { return cola_section_detail::section(body.bytes(), layout, slot); };
      auto words = [&](std::size_t slot) { return word_view::little_endian(part(slot)); };
      std::array<borrowed_view, 2> profiles{cola_section_detail::profile<P>(body.bytes(), layout, 0),
        cola_section_detail::profile<P>(body.bytes(), layout, 1)};
      std::array<rank_view, 2> ranks{rank_view(words(10), words(11), layout.virtual_count),
        rank_view(words(12), words(13), layout.virtual_count)};
      std::array<std::span<std::byte const>, 2> flags{part(14), part(15)};
      std::array<word_view, 2> cuts{words(16), words(17)};
      auto groups = layout.virtual_count / P::group_size + (layout.virtual_count % P::group_size != 0);
      for (unsigned route = 0; route != 2; ++route)
        if (profiles[route].size() > std::numeric_limits<std::uint64_t>::max() - 7 ||
            flags[route].size() != ((profiles[route].size() + 7) >> 3) || cuts[route].size() != groups)
          error_detail::raise<std::invalid_argument>("COLA navigation shape mismatch");
      return {std::move(source), std::move(body), std::move(layout), profiles, ranks, flags, cuts};
    }
    static mapped_cola_index open(std::filesystem::path const & path) { return open(file<P>::open(path)); }
    static mapped_cola_index from_slice(mapped_slice source, file_open_mode mode = file_open_mode::checked) {
      return open(file<P>::from_slice(std::move(source), mode));
    }
    bool active() const noexcept { return !body_.empty(); }
    borrowed_view borrowed(unsigned route) const & { require_active(); return profiles_[cola_detail::route(route)]; }
    borrowed_view borrowed(unsigned) const && = delete;
    rank_view interleave(unsigned route) const & { require_active(); return ranks_[cola_detail::route(route)]; }
    rank_view interleave(unsigned) const && = delete;
    std::span<std::byte const> false_borrow_bits(unsigned route) const & {
      require_active(); return flags_[cola_detail::route(route)];
    }
    std::span<std::byte const> false_borrow_bits(unsigned) const && = delete;
    word_view cut_lcps(unsigned route) const & { require_active(); return cuts_[cola_detail::route(route)]; }
    word_view cut_lcps(unsigned) const && = delete;
    object_id const & native_id() const & { require_active(); return layout_.native_id; }
    object_id const & native_id() const && = delete;
    std::optional<blob_identity> const & main_id() const & { require_active(); return layout_.main_id; }
    std::optional<blob_identity> const & main_id() const && = delete;
    std::optional<object_id> const & secondary_id() const & { require_active(); return layout_.secondary_id; }
    std::optional<object_id> const & secondary_id() const && = delete;
    std::uint64_t virtual_size() const { require_active(); return layout_.virtual_count; }
    cola_section_detail::directory const & layout() const & { require_active(); return layout_; }
    cola_section_detail::directory const & layout() const && = delete;
    std::span<std::byte const> section(std::size_t slot) const & {
      require_active(); return cola_section_detail::section(body_.bytes(), layout_, slot);
    }
    std::span<std::byte const> section(std::size_t) const && = delete;
    cola_index_view<P> view(profile_view<P, stream_role::native> native) const & {
      require_active(); return {native, profiles_, ranks_, flags_, cuts_, layout_.virtual_count};
    }
    cola_index_view<P> view(profile_view<P, stream_role::native>) const && = delete;
    void scan() const {
      require_active();
      source_.scan();
      std::size_t end = cola_section_detail::directory_bytes;
      for (auto part : layout_.sections) {
        section_detail::zero(body_.bytes().subspan(end, static_cast<std::size_t>(part.offset) - end),
          "nonzero COLA section alignment padding");
        end = static_cast<std::size_t>(part.offset + part.length);
      }
      for (unsigned route = 0; route != 2; ++route) {
        section_detail::scan_profile(profiles_[route]);
        rank_groups_builder<P::group_size> builder;
        auto groups = ranks_[route].group_count();
        for (std::uint64_t group = 0; group != groups; ++group) {
          auto width = std::min<std::uint64_t>(P::group_size, layout_.virtual_count - group * P::group_size);
          auto population = ranks_[route].class_at(group);
          auto other = ranks_[1 - route].class_at(group);
          if (population > width || other > width - population)
            error_detail::raise<std::invalid_argument>("overlapping COLA rank populations");
          builder.append(population, width);
        }
        auto expected = builder.finish();
        if (expected.view().count() != profiles_[route].size())
          error_detail::raise<std::invalid_argument>("COLA rank total mismatch");
        section_detail::equal_words(ranks_[route].class_words(), expected.classes, "COLA rank class mismatch");
        section_detail::equal_words(ranks_[route].checkpoint_words(), expected.checkpoints, "COLA rank checkpoint mismatch");
        if (auto tail = (profiles_[route].size() & 7))
          if (std::to_integer<unsigned>(flags_[route].back()) >> tail)
            error_detail::raise<std::invalid_argument>("nonzero COLA false-borrow padding");
      }
    }
  private:
    file<P> source_;
    mapped_slice body_;
    cola_section_detail::directory layout_;
    std::array<borrowed_view, 2> profiles_;
    std::array<rank_view, 2> ranks_;
    std::array<std::span<std::byte const>, 2> flags_;
    std::array<word_view, 2> cuts_;
    mapped_cola_index(file<P> source, mapped_slice body, cola_section_detail::directory layout,
        std::array<borrowed_view, 2> profiles, std::array<rank_view, 2> ranks,
        std::array<std::span<std::byte const>, 2> flags, std::array<word_view, 2> cuts)
      : source_(std::move(source)), body_(std::move(body)), layout_(std::move(layout)),
        profiles_(profiles), ranks_(ranks), flags_(flags), cuts_(cuts) {}
    void require_active() const {
      if (!active()) error_detail::raise<std::logic_error>("COLA index has no mapping");
    }
  };
}
