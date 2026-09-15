/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Streams sort-owned KV03 payloads with bounded buffering and shared framing.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/object_stream.h>
#include <diet/sort_profile.h>

namespace diet {
  namespace sort_profile_detail {
    template <class P, class Ops> struct file_bit_sink {
      static constexpr std::size_t buffer_bytes = 64 * 1024;
      explicit file_bit_sink(object_stream<P, Ops> & stream) : stream_(stream) {}
      std::uint64_t position() const noexcept { return bits_; }
      void append(bit_view source) {
        auto next = profile_detail::add(bits_, source.size());
        if (next > std::numeric_limits<std::uint64_t>::max() - 7)
          throw std::length_error("sort file bit extent");
        while (!source.empty()) {
          if (((buffered_ | source.offset()) & 7) == 0 && source.size() >= buffer_bytes * 8) {
            flush();
            auto bytes = source.size() >> 3;
            stream_.append(source.storage().subspan(static_cast<std::size_t>(source.offset() >> 3),
              static_cast<std::size_t>(bytes)));
            source = source.subview(bytes << 3, source.size() - (bytes << 3));
          } else {
            auto count = std::min<std::uint64_t>(source.size(), buffer_bytes * 8 - buffered_);
            profile_detail::copy_bits(buffer_.data(), buffered_, source.prefix(count));
            buffered_ += count; source = source.subview(count, source.size() - count);
            if (buffered_ == buffer_bytes * 8) flush();
          }
        }
        bits_ = next;
      }
      void write_bits(std::uint64_t value, unsigned width) {
        if (width > 64 || (width < 64 && (value >> width))) throw std::invalid_argument("sort file bit field");
        std::array<std::byte, 8> bytes{};
        if (width) {
          auto aligned = value << (64 - width);
          for (unsigned i = 0; i != 8; ++i) bytes[i] = std::byte(aligned >> (56 - (i << 3)));
        }
        append(bit_view(bytes, width));
      }
      template <class Code> void write_count(std::uint64_t value) {
        using code = sort_codec_detail::count_policy<Code>;
        (void)profile_detail::add(position(), profile_detail::backspace_bits<code>(value));
        if constexpr (code::backspace_code == bit_backspace_code::exponential_golomb) {
          auto quotient = value >> code::backspace_parameter;
          if (quotient == std::numeric_limits<std::uint64_t>::max()) {
            zeroes(64); write_bits(1, 1); write_bits(0, 64);
          } else {
            auto prefix = quotient + 1; auto width = unsigned(std::bit_width(prefix));
            zeroes(width - 1); write_bits(prefix, width);
          }
          if constexpr (code::backspace_parameter)
            write_bits(value & ((std::uint64_t{1} << code::backspace_parameter) - 1), unsigned(code::backspace_parameter));
        } else {
          constexpr auto modulus = code::backspace_parameter;
          constexpr auto width = unsigned(std::bit_width(modulus - 1));
          constexpr auto cutoff = profile_detail::golomb_cutoff<code>();
          zeroes(value / modulus); write_bits(1, 1);
          auto remainder = value % modulus;
          if (remainder < cutoff) write_bits(remainder, width - 1);
          else write_bits(remainder + cutoff, width);
        }
      }
      // Only the final payload call may flush an incomplete physical byte.
      void finish_payload() { flush(); }
      void align() {
        static constexpr std::array<std::byte, 8> zero{};
        stream_.append(std::span(zero).first(static_cast<std::size_t>((0 - stream_.body_bytes()) & 7)));
      }
      void words(std::span<std::uint64_t const> input) {
        if constexpr (std::endian::native == std::endian::little) stream_.append(std::as_bytes(input));
        else while (!input.empty()) {
          auto count = std::min(input.size(), buffer_bytes / 8);
          for (std::size_t i = 0; i != count; ++i) file_detail::put(buffer_, i << 3, 8, input[i]);
          stream_.append(std::span(buffer_).first(count << 3)); input = input.subspan(count);
        }
      }
      void samples(std::span<elias_fano_sample const> input) {
        static_assert(sizeof(elias_fano_sample) == 16 && offsetof(elias_fano_sample, first) == 0 && offsetof(elias_fano_sample, sparse) == 8);
        if constexpr (std::endian::native == std::endian::little) stream_.append(std::as_bytes(input));
        else while (!input.empty()) {
          auto count = std::min(input.size(), buffer_bytes / 16);
          for (std::size_t i = 0; i != count; ++i) {
            file_detail::put(buffer_, i << 4, 8, input[i].first);
            file_detail::put(buffer_, (i << 4) + 8, 8, input[i].sparse);
          }
          stream_.append(std::span(buffer_).first(count << 4)); input = input.subspan(count);
        }
      }
    private:
      object_stream<P, Ops> & stream_;
      std::array<std::byte, buffer_bytes> buffer_{};
      std::uint64_t buffered_ = 0, bits_ = 0;
      void zeroes(std::uint64_t count) {
        static constexpr std::array<std::byte, 4096> zero{};
        while (count) {
          auto part = std::min<std::uint64_t>(count, zero.size() * 8); append(bit_view(zero, part)); count -= part;
        }
      }
      void flush() {
        if (!buffered_) return;
        if (buffered_ & 7) buffer_[buffered_ >> 3] &= std::byte(0xffu << (8 - (buffered_ & 7)));
        stream_.append(std::span(buffer_).first(static_cast<std::size_t>(profile_detail::byte_count(buffered_))));
        buffered_ = 0;
      }
    };
  }

  // Values and literal spans are consumed synchronously. The payload uses one
  // 64KiB buffer; sparse offsets and the shared selector dictionary stay in RAM.
  // No partially written attempt is a durable checkpoint. Any exception during
  // append_frame or finish poisons the attempt; surviving paths remain intact.
  template <class P, class Selector = registry_selector<typename P::registry_type>, class Ops = posix_object_ops>
  struct sort_profile_file_writer {
    using policy_type = P;
    static constexpr std::size_t buffer_bytes = sort_profile_detail::file_bit_sink<P, Ops>::buffer_bytes;
    sort_profile_file_writer(std::filesystem::path root, object_id id, object_attempt_id attempt)
      : stream_(std::move(root), std::move(id), std::move(attempt), file_kind::native_blob, 192), sink_(stream_) {}
    sort_profile_file_writer(std::filesystem::path root, object_id id, object_attempt_id attempt, Ops & ops)
      : stream_(std::move(root), std::move(id), std::move(attempt), file_kind::native_blob, 192, ops), sink_(stream_) {}
    sort_profile_file_writer(sort_profile_file_writer const &) = delete;
    sort_profile_file_writer & operator=(sort_profile_file_writer const &) = delete;
    sort_profile_file_writer(sort_profile_file_writer &&) = delete;
    sort_profile_file_writer & operator=(sort_profile_file_writer &&) = delete;
    std::uint64_t size() const noexcept { return encoder_.size(); }
    bool failed() const noexcept { return failed_ || stream_.failed(); }
    bool finished() const noexcept { return stream_.finished(); }
    auto common_value_width() const noexcept { return encoder_.metadata.common_value_width; }
    object_write_paths const & paths() const & noexcept { return stream_.paths(); }
    object_write_paths const & paths() const && = delete;
    void append_frame(sort_profile_frame const & frame, std::span<bit_view const> key,
                      std::uint64_t common, bit_view value) {
      require_active();
      try { encoder_.append_frame(sink_, frame, key, common, value); frame_only_ = true; }
      catch (...) { failed_ = true; throw; }
    }
    // Checked full logical keys plus already sort-encoded values. The value
    // never gets another length or a materialized record-sized scratch buffer.
    void append_encoded(bit_view key, bit_view value) {
      require_active();
      if (frame_only_) throw std::logic_error("encoded append after trusted frame stream");
      auto comparison = compare_common_bits(previous_.view(), key);
      if (size() && comparison.order >= 0) throw std::invalid_argument("sort file keys must be strictly ordered");
      try {
        sort_bit_reader input(key);
        Selector::select(input, [&]<class S>(std::type_identity<S>, auto &) {
          std::array<bit_view, 1> spans{key};
          encoder_.template append<S>(sink_, key.prefix(input.position()), input.remaining(), spans, value, comparison.common_bits);
        });
        previous_ = bit_string::copy(key);
      } catch (...) { failed_ = true; throw; }
    }
    void append_encoded(profile_record const & record) { append_encoded(record.key.view(), record.value.view()); }
    object_seal_receipt finish() {
      require_active();
      try {
        encoder_.finish(sink_.position());
        auto const & ef = encoder_.offsets;
        std::array<std::byte, 192> directory{};
        for (unsigned i = 0; i != 4; ++i) directory[i] = std::byte("KV03"[i]);
        file_detail::put(directory, 4, 2, 3); file_detail::put(directory, 6, 2, 8);
        file_detail::put(directory, 8, 8, encoder_.metadata.extent);
        file_detail::put(directory, 16, 8, encoder_.metadata.terminal_key_units);
        file_detail::put(directory, 24, 8, ef.universe);
        file_detail::put(directory, 32, 8, encoder_.dictionary.bit_size);
        file_detail::put(directory, 40, 8, encoder_.seeds.bit_size);
        directory[48] = std::byte(ef.low_width);
        std::array<std::uint64_t, 8> lengths{profile_detail::byte_count(sink_.position()),
          profile_detail::multiply(ef.low.size(), 8), profile_detail::multiply(ef.high.size(), 8),
          profile_detail::multiply(ef.samples.size(), 16), profile_detail::multiply(ef.sparse.size(), 8),
          encoder_.dictionary.bytes.size(), profile_detail::multiply(encoder_.dictionary_offsets.size(), 8), encoder_.seeds.bytes.size()};
        std::uint64_t end = directory.size();
        for (std::size_t i = 0; i != lengths.size(); ++i) {
          auto start = (profile_detail::add(end, 7)) & ~std::uint64_t{7}; end = profile_detail::add(start, lengths[i]);
          file_detail::put(directory, 64 + 16 * i, 8, start); file_detail::put(directory, 72 + 16 * i, 8, lengths[i]);
        }
        file_header<P> header{file_kind::native_blob, profile_detail::multiply(end, 8), size(), encoder_.metadata.common_value_width};
        file_detail::validate_metadata(header);
        sink_.finish_payload();
        sink_.align(); sink_.words(ef.low);
        sink_.align(); sink_.words(ef.high);
        sink_.align(); sink_.samples(ef.samples);
        sink_.align(); sink_.words(ef.sparse);
        sink_.align(); stream_.append(encoder_.dictionary.bytes);
        sink_.align(); sink_.words(encoder_.dictionary_offsets);
        sink_.align(); stream_.append(encoder_.seeds.bytes);
        return stream_.finish(header, directory);
      } catch (...) { failed_ = true; throw; }
    }
  private:
    object_stream<P, Ops> stream_;
    sort_profile_detail::file_bit_sink<P, Ops> sink_;
    sort_profile_detail::encoder<P, Selector> encoder_;
    bit_string previous_;
    bool failed_ = false, frame_only_ = false;
    void require_active() const { if (failed() || finished()) throw std::logic_error("inactive sort file writer"); }
  };
}
