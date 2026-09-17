/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Streams native and borrowed profile sections with bounded payload buffering.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/object_stream.h>
#include <everett/sections.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace everett {
  namespace profile_detail {
    // Borrowed final sections. The caller retains these arrays, exact identities
    // and their dependency pins through finish; no section is copied here.
    template <class P> struct index_file_sections {
      object_id const & native_id;
      std::optional<blob_identity> const & target_id;
      rank_groups<P::group_size> const & interleave;
      std::span<std::byte const> false_borrows;
      std::span<std::uint64_t const> cut_lcps;
      std::uint64_t virtual_count;
    };

    // Internal ordinary-FC sink. The caller supplies an exact retained prefix
    // and proves ordered keys (strictly ordered for native records). Input spans
    // are borrowed only during append.
    // FC buffering is bounded; only one residual offset per block is retained.
    template <class P, stream_role Role, class Ops = posix_object_ops> struct profile_file_output {
      using policy_type = P;
      static_assert(Role == stream_role::native || Role == stream_role::borrowed);
      static constexpr bool borrowed = Role == stream_role::borrowed;
      static constexpr file_kind kind = borrowed ? file_kind::fractional_index : file_kind::native_blob;
      static constexpr std::size_t directory_bytes = borrowed ? section_detail::index_directory_bytes :
        section_detail::native_directory_bytes;
      static constexpr std::size_t descriptor_offset = borrowed ? section_detail::index_descriptor_offset :
        section_detail::native_descriptor_offset;
      static constexpr std::size_t section_count = borrowed ? 9 : 5;
      static constexpr std::optional<std::uint64_t> default_width = borrowed ?
        std::optional<std::uint64_t>(0) : P::value_width;
      profile_file_output(std::filesystem::path root, object_id id, object_attempt_id attempt,
          std::optional<std::uint64_t> common = default_width)
        : common_(checked_width(common)), control_(control_buffer()),
          stream_(std::move(root), std::move(id), std::move(attempt), kind, directory_bytes) {}
      profile_file_output(std::filesystem::path root, object_id id, object_attempt_id attempt,
          std::optional<std::uint64_t> common, Ops & ops)
        : common_(checked_width(common)), control_(control_buffer()),
          stream_(std::move(root), std::move(id), std::move(attempt), kind, directory_bytes, ops) {}
      profile_file_output(profile_file_output const &) = delete;
      profile_file_output & operator=(profile_file_output const &) = delete;
      profile_file_output(profile_file_output &&) = delete;
      profile_file_output & operator=(profile_file_output &&) = delete;

      std::uint64_t size() const noexcept { return count_; }
      bool failed() const noexcept { return failed_ || stream_.failed(); }
      bool finished() const noexcept { return stream_.finished(); }
      std::optional<std::uint64_t> common_value_width() const noexcept { return common_; }
      object_write_paths const & paths() const & noexcept { return stream_.paths(); }
      object_write_paths const & paths() const && = delete;
      void require_active() const {
        if (failed() || finished())
          error_detail::raise<std::logic_error>("profile file writer is not active");
      }

      void append(std::uint64_t retained, bit_view literal, bit_view value) {
        require_active();
        if ((literal.size() & (P::bits_per_unit - 1)) || (value.size() & (P::bits_per_unit - 1)))
          error_detail::raise<std::invalid_argument>("profile file record disagrees with policy units");
        auto value_units = value.size() >> P::unit_shift;
        if (common_ && value_units != *common_)
          error_detail::raise<std::invalid_argument>("profile file value disagrees with common width");
        if (retained > previous_units_ || (!borrowed && count_ && literal.empty()))
          error_detail::raise<std::invalid_argument>("profile file invalid known prefix");
        auto literal_units = literal.size() >> P::unit_shift;
        auto key_units = add(retained, literal_units);
        (void)multiply(key_units, P::bits_per_unit);
        auto next_count = add(count_, 1);
        control_.bytes.clear(); control_.bit_size = 0;
        std::uint64_t zero_bits = 0;
        auto block = count_ % P::codec_block_size == 0;
        if (block) write_count<P>(control_, retained);
        else if constexpr (P::unit == profile_unit::bit && P::backspace_code == bit_backspace_code::golomb) {
          auto backspace = previous_units_ - retained;
          auto bits = backspace_bits<P>(backspace);
          if (bits <= 129) write_backspace<P>(control_, backspace);
          else {
            // The unary quotient can be arbitrarily longer than the key's
            // new literal. Emit it separately, never grow the control scratch.
            constexpr auto modulus = P::backspace_parameter;
            constexpr auto width = unsigned(std::bit_width(modulus - 1));
            constexpr auto cutoff = golomb_cutoff<P>();
            zero_bits = backspace / modulus;
            auto remainder = backspace % modulus;
            auto remainder_width = width - unsigned(remainder < cutoff);
            resize(control_, 1 + remainder_width);
            std::uint64_t at = 0;
            put_fixed(control_, at, 1, 1);
            put_fixed(control_, at, remainder < cutoff ? remainder : remainder + cutoff, remainder_width);
          }
        } else write_backspace<P>(control_, previous_units_ - retained);
        write_count<P>(control_, literal_units);
        if (!common_) write_count<P>(control_, value_units);
        auto frame_bits = add(add(zero_bits, control_.bit_size), add(literal.size(), value.size()));
        auto next_bits = add(fc_bits_, frame_bits);
        if (next_bits > std::numeric_limits<std::uint64_t>::max() - 7)
          error_detail::raise<std::length_error>("profile file FC extent is too large");
        // Allocate the next navigation entry before any output is emitted.
        if (block) offsets_.push_back((fc_bits_ >> P::unit_shift) - multiply(count_, common_.value_or(0)));
        try {
          emit_zeroes(zero_bits);
          emit(control_.view()); emit(literal); emit(value);
        } catch (...) { failed_ = true; throw; }
        fc_bits_ = next_bits; previous_units_ = key_units; count_ = next_count;
      }

      object_seal_receipt finish() requires (!borrowed) { return finish_impl(nullptr); }
      object_seal_receipt finish(index_file_sections<P> const & index) requires (borrowed) {
        return finish_impl(&index);
      }

    private:
      object_seal_receipt finish_impl(index_file_sections<P> const * index) {
        require_active();
        if constexpr (borrowed) validate_index(*index);
        auto extent = fc_bits_ >> P::unit_shift;
        offsets_.push_back(extent - multiply(count_, common_.value_or(0)));
        elias_fano offsets;
        try { offsets = elias_fano::build<typename P::architecture>(offsets_); }
        catch (...) { offsets_.pop_back(); throw; }
        offsets_.pop_back();
        std::array<std::byte, directory_bytes> directory{};
        auto magic = borrowed ? "IX02" : "KV02";
        for (unsigned i = 0; i != 4; ++i) directory[i] = std::byte(magic[i]);
        file_detail::put(directory, 4, 2, section_detail::version);
        file_detail::put(directory, 6, 2, section_count);
        file_detail::put(directory, 8, 8, extent);
        file_detail::put(directory, 16, 8, previous_units_);
        file_detail::put(directory, 24, 8, offsets.universe);
        file_detail::put(directory, 32, 1, offsets.low_width);
        std::array<std::uint64_t, section_count> lengths{byte_count(fc_bits_), multiply(offsets.low.size(), 8),
          multiply(offsets.high.size(), 8), multiply(offsets.samples.size(), 16), multiply(offsets.sparse.size(), 8)};
        if constexpr (borrowed) {
          file_detail::put(directory, 40, 8, index->virtual_count);
          section_detail::put_id(directory, 48, index->native_id);
          if (index->target_id) {
            file_detail::put(directory, 33, 1, 1);
            section_detail::put_id(directory, 64, index->target_id->native);
            section_detail::put_id(directory, 80, index->target_id->index);
          }
          lengths[5] = multiply(index->interleave.classes.size(), 8);
          lengths[6] = multiply(index->interleave.checkpoints.size(), 8);
          lengths[7] = index->false_borrows.size();
          lengths[8] = multiply(index->cut_lcps.size(), 8);
        }
        std::uint64_t end = directory.size();
        for (std::size_t i = 0; i != lengths.size(); ++i) {
          auto start = add(end, 7) & ~std::uint64_t{7};
          end = add(start, lengths[i]);
          file_detail::put(directory, descriptor_offset + (i << 4), 8, start);
          file_detail::put(directory, descriptor_offset + (i << 4) + 8, 8, lengths[i]);
        }
        file_header<P> header{kind, multiply(end, 1u << (3 - P::unit_shift)), count_, common_};
        file_detail::validate_metadata(header);
        if (file_detail::total_bytes<P>(header.extent) > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
          error_detail::raise<std::length_error>("profile file exceeds supported file offsets");
        // EF allocation and complete metadata admission above remain retryable.
        // Once final bytes are emitted, no retry may duplicate the directories.
        try {
          flush_tail();
          align_stream(); emit_words(offsets.low);
          align_stream(); emit_words(offsets.high);
          align_stream(); emit_samples(offsets.samples);
          align_stream(); emit_words(offsets.sparse);
          if constexpr (borrowed) {
            align_stream(); emit_words(index->interleave.classes);
            align_stream(); emit_words(index->interleave.checkpoints);
            align_stream(); stream_.append(index->false_borrows);
            align_stream(); emit_words(index->cut_lcps);
          }
          return stream_.finish(header, directory);
        } catch (...) { failed_ = true; throw; }
      }

      static std::optional<std::uint64_t> checked_width(std::optional<std::uint64_t> common) {
        if constexpr (borrowed) {
          if (common != std::optional<std::uint64_t>(0))
            error_detail::raise<std::invalid_argument>("borrowed file values must be empty");
        } else if constexpr (P::fixed_width)
          if (common != P::value_width)
            error_detail::raise<std::invalid_argument>("profile file width disagrees with fixed policy");
        if (common) (void)multiply(*common, P::bits_per_unit);
        return common;
      }
      void validate_index(index_file_sections<P> const & index) const {
        auto groups = index.virtual_count / P::group_size + (index.virtual_count % P::group_size != 0);
        if (index.virtual_count < count_ || count_ > std::numeric_limits<std::uint64_t>::max() - 7 ||
            index.false_borrows.size() != ((count_ + 7) >> 3) || index.cut_lcps.size() != groups ||
            index.interleave.virtual_count != index.virtual_count || (!index.target_id && count_))
          error_detail::raise<std::invalid_argument>("streamed index metadata shape mismatch");
        if (index.interleave.view().count() != count_)
          error_detail::raise<std::invalid_argument>("streamed index borrowed count mismatch");
        if ((count_ & 7) && (std::to_integer<unsigned>(index.false_borrows.back()) >> (count_ & 7)))
          error_detail::raise<std::invalid_argument>("streamed index flag padding is nonzero");
      }

      static bit_string control_buffer() { bit_string result; result.bytes.reserve(64); return result; }
      static constexpr std::size_t buffer_bytes = 64 * 1024;
      std::optional<std::uint64_t> common_;
      bit_string control_;
      std::array<std::byte, buffer_bytes> buffer_{};
      std::vector<std::uint64_t> offsets_;
      object_stream<P, Ops> stream_;
      std::uint64_t buffered_bits_ = 0, fc_bits_ = 0, count_ = 0, previous_units_ = 0;
      bool failed_ = false;

      void emit(bit_view source) {
        while (!source.empty()) {
          if (((buffered_bits_ | source.offset()) & 7) == 0 && source.size() >= buffer_bytes * 8) {
            flush_tail();
            auto bytes = source.size() >> 3;
            stream_.append(source.storage().subspan(static_cast<std::size_t>(source.offset() >> 3),
                                                   static_cast<std::size_t>(bytes)));
            source = source.subview(bytes << 3, source.size() - (bytes << 3));
            continue;
          }
          auto count = std::min<std::uint64_t>(source.size(), buffer_bytes * 8 - buffered_bits_);
          copy_bits(buffer_.data(), buffered_bits_, source.prefix(count));
          buffered_bits_ += count;
          source = source.subview(count, source.size() - count);
          if (buffered_bits_ == buffer_bytes * 8) flush_tail();
        }
      }
      void emit_zeroes(std::uint64_t bits) {
        static constexpr std::array<std::byte, 4096> zero{};
        while (bits) {
          auto count = std::min<std::uint64_t>(bits, zero.size() * 8);
          emit(bit_view(zero, count)); bits -= count;
        }
      }
      void flush_tail() {
        if (!buffered_bits_) return;
        if (buffered_bits_ & 7)
          buffer_[buffered_bits_ >> 3] &= std::byte(0xffu << (8 - (buffered_bits_ & 7)));
        stream_.append(std::span(buffer_).first(static_cast<std::size_t>(byte_count(buffered_bits_))));
        buffered_bits_ = 0;
      }
      void align_stream() {
        static constexpr std::array<std::byte, 8> zero{};
        auto gap = (0 - stream_.body_bytes()) & 7;
        stream_.append(std::span(zero).first(static_cast<std::size_t>(gap)));
      }
      void emit_words(std::span<std::uint64_t const> words) {
        if constexpr (std::endian::native == std::endian::little) stream_.append(std::as_bytes(words));
        else {
          while (!words.empty()) {
            auto count = std::min(words.size(), buffer_bytes / 8);
            for (std::size_t i = 0; i != count; ++i) file_detail::put(buffer_, i << 3, 8, words[i]);
            stream_.append(std::span(buffer_).first(count << 3)); words = words.subspan(count);
          }
        }
      }
      void emit_samples(std::span<elias_fano_sample const> samples) {
        static_assert(sizeof(elias_fano_sample) == 16 && offsetof(elias_fano_sample, first) == 0 &&
                      offsetof(elias_fano_sample, sparse) == 8);
        if constexpr (std::endian::native == std::endian::little) stream_.append(std::as_bytes(samples));
        else {
          while (!samples.empty()) {
            auto count = std::min(samples.size(), buffer_bytes / 16);
            for (std::size_t i = 0; i != count; ++i) {
              file_detail::put(buffer_, i << 4, 8, samples[i].first);
              file_detail::put(buffer_, (i << 4) + 8, 8, samples[i].sparse);
            }
            stream_.append(std::span(buffer_).first(count << 4)); samples = samples.subspan(count);
          }
        }
      }
    };
  }

}
