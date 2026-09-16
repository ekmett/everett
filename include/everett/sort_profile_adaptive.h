/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Retains small native merge outputs and spills larger encoded streams once.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/output_budget.h>
#include <everett/sort_profile_file_writer.h>
#include <everett/sort_profile_merge.h>

#include <variant>

namespace everett {
  namespace sort_profile_detail {
    // A factory reserves its identity before constructing the private stream.
    // Until the first flush, only the existing fixed bit-sink buffer is used.
    template <class P, class Ops, class Factory> struct lazy_native_stream {
      using stream_type = object_stream<P, Ops>;
      explicit lazy_native_stream(Factory factory) : factory_(std::move(factory)) {}
      bool spilled() const noexcept { return bool(stream_); }
      bool failed() const noexcept { return failed_ || (stream_ && stream_->failed()); }
      std::uint64_t body_bytes() const noexcept { return stream_ ? stream_->body_bytes() : 192; }
      void start() {
        if (failed()) throw std::logic_error("failed adaptive native stream");
        if (stream_) return;
        try {
          stream_ = factory_();
          if (!stream_) throw std::logic_error("null adaptive native stream");
        } catch (...) { poison(); throw; }
      }
      void append(std::span<std::byte const> bytes) {
        if (bytes.empty()) return;
        try { start(); stream_->append(bytes); }
        catch (...) { poison(); throw; }
      }
      object_seal_receipt finish(file_header<P> const & header, std::span<std::byte const> directory) {
        try { start(); return stream_->finish(header, directory); }
        catch (...) { poison(); throw; }
      }
      object_write_paths const & paths() const & {
        if (!stream_) throw std::logic_error("native output has not spilled");
        return stream_->paths();
      }
      object_write_paths const & paths() const && = delete;
      void poison() noexcept {
        failed_ = true;
        if constexpr (requires { { factory_.poison() } noexcept; }) factory_.poison();
      }
    private:
      Factory factory_;
      std::unique_ptr<stream_type> stream_;
      bool failed_ = false;
    };

    // Compose may expand values, so it cannot bound payload bytes in advance.
    // It cannot introduce additional records or selector codes. These cheap
    // source-metadata bounds reject a clearly oversized navigation structure;
    // actual finished allocation capacities are checked again before retention.
    template <class P, class View> bool small_native_metadata(View const & a, View const & b, std::size_t limit) {
      auto use = [&](std::uint64_t count, std::size_t width) {
        if (count > limit / width) return false;
        limit -= std::size_t(count) * width; return true;
      };
      if (b.size() > std::numeric_limits<std::uint64_t>::max() - a.size()) return false;
      auto records = a.size() + b.size();
      auto blocks = records / P::codec_block_size + (records % P::codec_block_size != 0);
      // EF low/high/select storage and packed selector seeds. This intentionally
      // allows slack; it is a preflight filter, not allocator measurement.
      return use(1, 512) && use(blocks + 1, 64) &&
        use(a.dictionary_size() + 1, 16) && use(b.dictionary_size() + 1, 16) &&
        use(profile_detail::byte_count(a.dictionary().size()), 2) &&
        use(profile_detail::byte_count(b.dictionary().size()), 2);
    }
  }

  template <class P, class Selector, class Ops, class Factory> struct sort_profile_adaptive_writer {
    using array_type = sort_profile_array<P, Selector>;
    using result_type = std::variant<std::shared_ptr<array_type const>, object_seal_receipt>;
    using stream_type = sort_profile_detail::lazy_native_stream<P, Ops, Factory>;
    using sink_type = sort_profile_detail::file_bit_sink<P, Ops, stream_type>;
    sort_profile_adaptive_writer(Factory factory, output_budget budget, std::size_t ceiling, bool metadata_fits)
      : stream_(std::move(factory)), sink_(stream_), budget_(std::move(budget)), ceiling_(ceiling) {
      if (!ceiling_ || !budget_.limit() || !metadata_fits) stream_.start();
    }
    sort_profile_adaptive_writer(sort_profile_adaptive_writer const &) = delete;
    sort_profile_adaptive_writer & operator=(sort_profile_adaptive_writer const &) = delete;
    bool failed() const noexcept { return failed_ || stream_.failed(); }
    bool finished() const noexcept { return finished_; }
    bool spilled() const noexcept { return stream_.spilled(); }
    std::uint64_t size() const noexcept { return encoder_.size(); }
    object_write_paths const & paths() const & { return stream_.paths(); }
    object_write_paths const & paths() const && = delete;
    void append_frame(sort_profile_frame const & frame, std::span<bit_view const> key,
        std::uint64_t common, bit_view value) {
      require_active();
      try { encoder_.append_frame(sink_, frame, key, common, value); }
      catch (...) { failed_ = true; stream_.poison(); throw; }
    }
    result_type finish() {
      require_active();
      try {
        encoder_.finish(sink_.position());
        if (!stream_.spilled()) {
          auto data = bit_string::copy(sink_.buffered_payload());
          auto const & ef = encoder_.offsets;
          auto bytes = sizeof(array_type) + sizeof(output_budget::lease) + data.bytes.capacity() + encoder_.dictionary.bytes.capacity() +
            encoder_.seeds.bytes.capacity() + 8 * (encoder_.dictionary_offsets.capacity() +
              ef.low.capacity() + ef.high.capacity() + ef.sparse.capacity()) +
            sizeof(elias_fano_sample) * ef.samples.capacity();
          if (bytes <= ceiling_) if (auto lease = budget_.try_acquire(bytes)) {
            auto result = output_budget::attach(encoder_.take(std::move(data)), std::move(*lease));
            finished_ = true; return result;
          }
        }
        auto result = sort_profile_detail::seal<P>(encoder_, sink_, stream_);
        finished_ = true; return result;
      } catch (...) { failed_ = true; stream_.poison(); throw; }
    }
  private:
    stream_type stream_;
    sink_type sink_;
    output_budget budget_;
    std::size_t ceiling_;
    sort_profile_detail::encoder<P, Selector> encoder_;
    bool failed_ = false, finished_ = false;
    void require_active() const { if (failed() || finished_) throw std::logic_error("inactive adaptive native writer"); }
  };

  template <class P, class Native, class Compose, class Selector, class Ops, class Factory>
  struct sort_profile_adaptive_merge {
    using source_pointer = std::shared_ptr<Native const>;
    using output_type = sort_profile_adaptive_writer<P, Selector, Ops, Factory>;
    struct output_ref {
      output_type * output;
      bool failed() const noexcept { return output->failed(); }
      void append_frame(sort_profile_frame const & frame, std::span<bit_view const> key,
          std::uint64_t common, bit_view value) { output->append_frame(frame, key, common, value); }
      auto finish() { return output->finish(); }
    };
    using builder_type = sort_profile_merge_builder<P, Native, Compose, Selector, output_ref>;
    sort_profile_adaptive_merge(Factory factory, output_budget budget, std::size_t ceiling,
        source_pointer older, source_pointer newer, Compose compose)
      : output_(std::move(factory), std::move(budget), ceiling, metadata_fits(older, newer, ceiling)),
        builder_({&output_}, std::move(older), std::move(newer), std::move(compose)) {}
    sort_profile_adaptive_merge(sort_profile_adaptive_merge const &) = delete;
    sort_profile_adaptive_merge & operator=(sort_profile_adaptive_merge const &) = delete;
    bool done() const noexcept { return builder_.done(); }
    bool failed() const noexcept { return builder_.failed(); }
    bool finished() const noexcept { return builder_.finished(); }
    bool spilled() const noexcept { return output_.spilled(); }
    native_merge_progress progress() const noexcept { return builder_.progress(); }
    std::uint64_t materialized_keys() const noexcept { return builder_.materialized_keys(); }
    native_merge_progress step(std::uint64_t budget = 1) { return builder_.step(budget); }
    auto finish() { return builder_.finish(); }
    object_write_paths const & paths() const & { return output_.paths(); }
    object_write_paths const & paths() const && = delete;
  private:
    output_type output_;
    builder_type builder_;
    static bool metadata_fits(source_pointer const & a, source_pointer const & b, std::size_t ceiling) {
      if (!a || !b) throw std::invalid_argument("null adaptive merge source");
      return sort_profile_detail::small_native_metadata<P>(a->view(), b->view(), ceiling);
    }
  };
}
