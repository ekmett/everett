/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Maps sort-owned KV03 records and binds their ordinary fractional indexes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/sort_profile.h>
#include <diet/cola_sections.h>
#include <diet/cola_query.h>
#include <diet/mapped_cola.h>

namespace diet {
  namespace sort_profile_file_detail {
    inline constexpr std::size_t directory_bytes = 192, section_count = 8;
    inline constexpr std::size_t descriptors = 64;
    inline void append_words(std::vector<std::byte> & out, std::span<std::uint64_t const> words) {
      out.resize(words.size() << 3);
      for (std::size_t i = 0; i != words.size(); ++i) file_detail::put(out, i << 3, 8, words[i]);
    }
  }

  template <class P> struct encoded_sort_sections {
    encoded_sort_sections() = default;
    encoded_sort_sections(encoded_sort_sections const &) = delete;
    encoded_sort_sections & operator=(encoded_sort_sections const &) = delete;
    encoded_sort_sections(encoded_sort_sections && other) noexcept
      : header_(other.header_), directory_(other.directory_), zero_(other.zero_), parts_(other.parts_),
        owned_(std::move(other.owned_)), active_(std::exchange(other.active_, false)) {}
    encoded_sort_sections & operator=(encoded_sort_sections && other) noexcept {
      if (this != &other) {
        header_ = other.header_; directory_ = other.directory_; zero_ = other.zero_;
        parts_ = other.parts_; owned_ = std::move(other.owned_); active_ = std::exchange(other.active_, false);
      }
      return *this;
    }
    template <class Selector> static encoded_sort_sections from(sort_profile_array<P, Selector> const & source) {
      encoded_sort_sections result;
      auto & directory = result.directory_;
      for (unsigned i = 0; i != 4; ++i) directory[i] = std::byte("KV03"[i]);
      file_detail::put(directory, 4, 2, 3); file_detail::put(directory, 6, 2, 8);
      auto const & metadata = source.metadata();
      auto const & ef = source.group_offsets();
      file_detail::put(directory, 8, 8, metadata.extent);
      file_detail::put(directory, 16, 8, metadata.terminal_key_units);
      file_detail::put(directory, 24, 8, ef.universe);
      file_detail::put(directory, 32, 8, source.dictionary().bit_size);
      file_detail::put(directory, 40, 8, source.seeds().bit_size);
      directory[48] = std::byte(ef.low_width);
      result.parts_[0] = source.data().bytes;
      result.parts_[5] = source.dictionary().bytes;
      result.parts_[7] = source.seeds().bytes;
      result.words(1, ef.low); result.words(2, ef.high); result.words(4, ef.sparse);
      result.words(6, source.dictionary_offsets());
      auto & samples = result.owned_[3]; samples.resize(ef.samples.size() << 4);
      for (std::size_t i = 0; i != ef.samples.size(); ++i) {
        file_detail::put(samples, i << 4, 8, ef.samples[i].first);
        file_detail::put(samples, (i << 4) + 8, 8, ef.samples[i].sparse);
      }
      result.parts_[3] = samples;
      std::uint64_t end = directory.size();
      for (std::size_t i = 0; i != result.parts_.size(); ++i) {
        auto start = section_detail::align(end);
        file_detail::put(directory, 64 + 16 * i, 8, start);
        file_detail::put(directory, 72 + 16 * i, 8, result.parts_[i].size());
        end = profile_detail::add(start, result.parts_[i].size());
      }
      result.header_.record_count = source.size();
      result.header_.common_value_width = metadata.common_value_width;
      result.header_.extent = profile_detail::multiply(end, 8);
      return result;
    }
    template <class Selector> static encoded_sort_sections from(sort_profile_array<P, Selector> const &&) = delete;
    file_header<P> const & header() const & { require_active(); return header_; }
    file_header<P> const & header() const && = delete;
    std::vector<std::span<std::byte const>> chunks() const & {
      require_active();
      std::vector<std::span<std::byte const>> result{directory_};
      std::uint64_t end = directory_.size();
      for (auto part : parts_) {
        auto gap = section_detail::align(end) - end;
        if (gap) result.push_back(std::span(zero_).first(gap));
        result.push_back(part); end += gap + part.size();
      }
      return result;
    }
    std::vector<std::span<std::byte const>> chunks() const && = delete;
    std::vector<std::byte> materialize() const {
      std::vector<std::byte> body;
      for (auto part : chunks()) body.insert(body.end(), part.begin(), part.end());
      return encode_file(header_, body);
    }
    object_seal_receipt seal(std::filesystem::path const & root, object_id const & id,
        object_attempt_id const & attempt) const {
      auto parts = chunks(); return object_writer<P>::seal(root, id, attempt, header_, parts);
    }
  private:
    file_header<P> header_;
    std::array<std::byte, 192> directory_{};
    std::array<std::byte, 8> zero_{};
    std::array<std::span<std::byte const>, 8> parts_;
    std::array<std::vector<std::byte>, 8> owned_;
    bool active_ = true;
    void require_active() const { if (!active_) throw std::logic_error("inactive encoded sort profile"); }
    void words(std::size_t slot, std::span<std::uint64_t const> source) {
      sort_profile_file_detail::append_words(owned_[slot], source); parts_[slot] = owned_[slot];
    }
  };

  template <class P, class Selector = registry_selector<typename P::registry_type>> struct mapped_sort_profile {
    using policy_type = P;
    using stream_family = sort_profile_family<P, Selector>;
    using view_type = sort_profile_view<P, Selector>;
    static mapped_sort_profile open(file<P> source) {
      auto header = source.header(); auto body = source.body(); auto bytes = body.bytes();
      if (header.kind != file_kind::native_blob || bytes.size() < 192 ||
          header.extent != profile_detail::multiply(bytes.size(), 8))
        throw std::invalid_argument("sort profile file extent or kind");
      for (unsigned i = 0; i != 4; ++i)
        if (bytes[i] != std::byte("KV03"[i])) throw std::invalid_argument("sort profile file revision");
      if (file_detail::get(bytes, 4, 2) != 3 || file_detail::get(bytes, 6, 2) != 8)
        throw std::invalid_argument("sort profile section revision");
      section_detail::zero(bytes.subspan(49, 15), "nonzero sort profile reserved bytes");
      std::array<std::span<std::byte const>, 8> parts;
      std::uint64_t end = 192;
      for (std::size_t i = 0; i != parts.size(); ++i) {
        auto first = file_detail::get(bytes, 64 + 16 * i, 8), count = file_detail::get(bytes, 72 + 16 * i, 8);
        if (first != section_detail::align(end) || first > bytes.size() || count > bytes.size() - first)
          throw std::invalid_argument("sort profile section range");
        parts[i] = bytes.subspan(first, count); end = first + count;
      }
      if (end != bytes.size()) throw std::invalid_argument("trailing sort profile sections");
      auto metadata = profile_detail::initial_metadata<P, stream_role::native>();
      metadata.version = 3; metadata.extent = file_detail::get(bytes, 8, 8);
      metadata.record_count = header.record_count; metadata.terminal_key_units = file_detail::get(bytes, 16, 8);
      metadata.common_value_width = header.common_value_width;
      metadata.policy_fixed_values = header.policy_value_width.has_value();
      metadata.policy_value_width = header.policy_value_width.value_or(0);
      auto dictionary_bits = file_detail::get(bytes, 32, 8), seed_bits = file_detail::get(bytes, 40, 8);
      if (parts[0].size() != profile_detail::byte_count(metadata.extent) ||
          parts[5].size() != profile_detail::byte_count(dictionary_bits) ||
          parts[7].size() != profile_detail::byte_count(seed_bits))
        throw std::invalid_argument("sort profile bit section extent");
      auto blocks = header.record_count / P::codec_block_size + (header.record_count % P::codec_block_size != 0);
      elias_fano_view ef{word_view::little_endian(parts[1]), word_view::little_endian(parts[2]),
        sample_view::little_endian(parts[3]), word_view::little_endian(parts[4]), blocks + 1,
        file_detail::get(bytes, 24, 8), unsigned(std::to_integer<unsigned char>(bytes[48]))};
      view_type view({parts[0], metadata.extent}, ef, metadata, {parts[5], dictionary_bits},
        word_view::little_endian(parts[6]), {parts[7], seed_bits});
      return {std::move(source), std::move(body), std::move(view)};
    }
    static mapped_sort_profile open(std::filesystem::path const & path) { return open(file<P>::open(path)); }
    static mapped_sort_profile from_slice(mapped_slice source, file_open_mode mode = file_open_mode::checked) {
      return open(file<P>::from_slice(std::move(source), mode));
    }
    view_type view() const & { require_active(); return view_; }
    view_type view() const && = delete;
    std::uint64_t size() const { require_active(); return view_.size(); }
    void scan() const {
      require_active(); source_.scan(); view_.scan();
      auto bytes = body_.bytes(); std::uint64_t end = 192;
      for (unsigned i = 0; i != 8; ++i) {
        auto first = file_detail::get(bytes, 64 + 16 * i, 8), count = file_detail::get(bytes, 72 + 16 * i, 8);
        section_detail::zero(bytes.subspan(end, first - end), "nonzero sort profile section gap"); end = first + count;
      }
      for (auto bits : {view_.data(), view_.dictionary(), view_.seeds()})
        if ((bits.size() & 7) && (std::to_integer<unsigned>(bits.storage().back()) & ((1u << (8 - (bits.size() & 7))) - 1)))
          throw std::invalid_argument("sort profile tail padding");
    }
  private:
    void require_active() const { if (body_.empty()) throw std::logic_error("inactive mapped sort profile"); }
    file<P> source_;
    mapped_slice body_;
    view_type view_;
    mapped_sort_profile(file<P> source, mapped_slice body, view_type view)
      : source_(std::move(source)), body_(std::move(body)), view_(view) {}
  };

  template <class P, class Selector = registry_selector<typename P::registry_type>> struct mapped_sort_cola {
    using policy_type = P;
    using native_type = mapped_sort_profile<P, Selector>;
    using native_pointer = std::shared_ptr<native_type const>;
    using index_type = mapped_cola_index<P>;
    using index_pointer = std::shared_ptr<index_type const>;
    using pair_type = std::shared_ptr<mapped_sort_cola const>;
    using view_type = cola_index_view<P, sort_profile_family<P, Selector>>;
    mapped_sort_cola(mapped_sort_cola const &) = delete;
    mapped_sort_cola & operator=(mapped_sort_cola const &) = delete;
    mapped_sort_cola(mapped_sort_cola &&) = delete;
    mapped_sort_cola & operator=(mapped_sort_cola &&) = delete;
    static pair_type bind(blob_identity identity, native_pointer native,
        std::shared_ptr<mapped_cola_index<P> const> index, pair_type main = {},
        native_pointer secondary = {}, std::optional<object_id> secondary_id = {}) {
      if (!native || !index || identity.native != index->native_id() ||
          bool(index->main_id()) != bool(main) || (main && *index->main_id() != main->identity()) ||
          bool(secondary) != bool(secondary_id) || index->secondary_id() != secondary_id)
        throw std::invalid_argument("sort COLA pinned identity mismatch");
      if (index->borrowed(0).size() != (main ? main->group_count() : 0) ||
          index->borrowed(1).size() != (secondary ? secondary->size() / P::group_size + (secondary->size() % P::group_size != 0) : 0))
        throw std::invalid_argument("sort COLA sample count mismatch");
      // Validate the immutable combined shape once, without touching payload.
      // Its spans remain backed by the native/index owners retained below.
      view_type view{native->view(), {index->borrowed(0), index->borrowed(1)}, {index->interleave(0), index->interleave(1)},
        {index->false_borrow_bits(0), index->false_borrow_bits(1)}, {index->cut_lcps(0), index->cut_lcps(1)}, index->virtual_size()};
      return pair_type(new mapped_sort_cola(identity, std::move(native), std::move(index), std::move(main),
        std::move(secondary), std::move(view)));
    }
    auto const & identity() const & noexcept { return identity_; }
    auto const & identity() const && = delete;
    native_pointer native_owner() const noexcept { return native_; }
    native_pointer native_object() const noexcept { return native_; }
    index_pointer index_object() const noexcept { return index_; }
    pair_type main_target() const noexcept { return main_; }
    native_pointer secondary_target() const noexcept { return secondary_; }
    std::uint64_t virtual_size() const noexcept { return view_.virtual_size(); }
    std::uint64_t group_count() const noexcept { return view_.group_count(); }
    view_type view() const & { return view_; }
    view_type view() const && = delete;
    void scan() const { scan_mapped_cola(*this); }
  private:
    blob_identity identity_;
    native_pointer native_;
    std::shared_ptr<mapped_cola_index<P> const> index_;
    pair_type main_;
    native_pointer secondary_;
    view_type view_;
    mapped_sort_cola(blob_identity identity, native_pointer native, std::shared_ptr<mapped_cola_index<P> const> index,
        pair_type main, native_pointer secondary, view_type view)
      : identity_(identity), native_(std::move(native)), index_(std::move(index)), main_(std::move(main)),
        secondary_(std::move(secondary)), view_(std::move(view)) {}
  };
}
