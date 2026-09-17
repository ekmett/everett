/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Appends immutable object bodies before their final envelope is known.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/object_writer.h>

#include <optional>

namespace everett {
  // Append an object whose final extent is not yet known. Each append consumes
  // complete physical bytes, including for bit policies. The caller keeps an
  // unfinished bit tail private until it can supply the final canonical byte.
  // Body chunks need live only during append. An optional fixed-size prefix is
  // initially zero and supplied at finish, for directories known only afterward.
  // Its checksum combines with the streamed suffix without rereading payloads.
  //
  // Construction reserves an exclusive private file under caller-reserved
  // identities. The root and filesystem premises are object_writer's. No
  // partial-output persistence receipt or restart operation is provided.
  // Destruction closes handles and preserves surviving names after failure or
  // abandonment. A borrowed Ops must outlive this nonmovable stream.
  template <class P, class Ops = posix_object_ops> struct object_stream {
    static_assert(Ops::supported, "Everett object streams require supported filesystem operations");
    using policy_type = P;

    object_stream(std::filesystem::path root, object_id id,
        object_attempt_id attempt, file_kind kind, std::size_t prefix_bytes = 0)
      : root_(std::move(root)), id_(std::move(id)), attempt_(std::move(attempt)),
        kind_(checked(kind)), prefix_bytes_(checked_prefix(prefix_bytes)),
        owned_ops_(std::in_place), ops_(*owned_ops_),
        run_(root_, id_, attempt_, kind_, ops_) { open(); }
    object_stream(std::filesystem::path root, object_id id,
        object_attempt_id attempt, file_kind kind, Ops & ops)
      : object_stream(std::move(root), std::move(id), std::move(attempt), kind, 0, ops) {}
    object_stream(std::filesystem::path root, object_id id,
        object_attempt_id attempt, file_kind kind, std::size_t prefix_bytes, Ops & ops)
      : root_(std::move(root)), id_(std::move(id)), attempt_(std::move(attempt)),
        kind_(checked(kind)), prefix_bytes_(checked_prefix(prefix_bytes)),
        ops_(ops), run_(root_, id_, attempt_, kind_, ops_) { open(); }
    object_stream(object_stream const &) = delete;
    object_stream & operator=(object_stream const &) = delete;
    object_stream(object_stream &&) = delete;
    object_stream & operator=(object_stream &&) = delete;

    std::uint64_t body_bytes() const noexcept { return body_bytes_; }
    // Before finish this describes the zero-filled prefix and appended bytes;
    // afterward it describes the final prefix and body. Failure makes no claim
    // about which writes reached the underlying file.
    std::uint32_t body_crc32c<typename P::architecture>() const noexcept { return crc_; }
    bool failed() const noexcept { return failed_; }
    bool finished() const noexcept { return finished_; }
    object_write_paths const & paths() const & noexcept { return run_.paths; }
    object_write_paths const & paths() const && = delete;

    // Invalid size is rejected before writing. Once a write fails, acknowledged
    // byte/CRC progress cannot certify the uncertain file and the stream is
    // poisoned. A later successful call must never rehabilitate that attempt.
    void append(std::span<std::byte const> bytes) {
      require_active();
      constexpr auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max()) -
        file_detail::header_bytes;
      if (bytes.size() > maximum - body_bytes_)
        throw std::length_error("Everett object exceeds supported file offsets");
      try {
        while (!bytes.empty()) {
          auto part = bytes.first(std::min(bytes.size(), std::size_t{1} << 20));
          run_.write_all(part);
          crc_ = crc32c<typename P::architecture>(part, crc_);
          body_bytes_ += part.size();
          last_ = part.back();
          bytes = bytes.subspan(part.size());
        }
      } catch (...) {
        failed_ = true;
        throw;
      }
    }

    // Metadata-only rejection leaves the stream available for corrected
    // metadata or more body bytes. Once sealing starts, any failure poisons it.
    object_seal_receipt finish(file_header<P> const & header) {
      return finish(header, {});
    }
    object_seal_receipt finish(file_header<P> const & header, std::span<std::byte const> prefix) {
      require_active();
      file_detail::validate_metadata(header);
      if (prefix.size() != prefix_bytes_ || header.kind != kind_ ||
          file_detail::body_bytes<P>(header.extent) != body_bytes_)
        throw std::invalid_argument("Everett streamed body metadata mismatch");
      auto last = body_bytes_ == prefix_bytes_ && !prefix.empty() ? prefix.back() : last_;
      if constexpr (P::unit == profile_unit::bit)
        if ((header.extent & 7) &&
            (std::to_integer<unsigned>(last) & ((1u << (8 - (header.extent & 7))) - 1)))
          throw std::invalid_argument("noncanonical bit-profile tail padding");
      auto crc = crc_ ^ crc32c_combine(prefix_crc_ ^ crc32c<typename P::architecture>(prefix), 0, body_bytes_ - prefix_bytes_);
      auto encoded = encode_file_header(header, crc);
      object_seal_receipt receipt{id_, attempt_, run_.paths.final,
        body_bytes_ + file_detail::header_bytes, crc, ops_.barrier()};
      try {
        run_.write_at(prefix, file_detail::header_bytes, "write object prefix");
        run_.write_header(encoded);
        run_.stage = object_write_stage::body_written;
        run_.finish();
        crc_ = crc;
        finished_ = true;
        return receipt;
      } catch (...) {
        failed_ = true;
        throw;
      }
    }

  private:
    static std::size_t checked_prefix(std::size_t bytes) {
      if (bytes > std::uint64_t(std::numeric_limits<std::int64_t>::max()) - file_detail::header_bytes)
        throw std::length_error("Everett object prefix exceeds supported file offsets");
      return bytes;
    }
    static file_kind checked(file_kind kind) {
      if (kind != file_kind::native_blob && kind != file_kind::fractional_index)
        throw std::invalid_argument("unsupported Everett object kind");
      return kind;
    }
    void open() {
      run_.open();
      std::array<std::byte, file_detail::header_bytes> placeholder{};
      run_.write_all(placeholder);
      std::array<std::byte, 4096> zero{};
      auto left = prefix_bytes_;
      while (left) {
        auto count = std::min(left, zero.size());
        append(std::span(zero).first(count));
        left -= count;
      }
      prefix_crc_ = crc_;
    }
    void require_active() const {
      if (failed_ || finished_) throw std::logic_error("Everett object stream is inactive");
    }
    std::filesystem::path root_;
    object_id id_;
    object_attempt_id attempt_;
    file_kind kind_;
    std::size_t prefix_bytes_ = 0;
    std::optional<Ops> owned_ops_;
    Ops & ops_;
    typename object_writer<P, Ops>::operation run_;
    std::uint64_t body_bytes_ = 0;
    std::uint32_t crc_ = 0;
    std::uint32_t prefix_crc_ = 0;
    std::byte last_{};
    bool failed_ = false;
    bool finished_ = false;
  };
}
