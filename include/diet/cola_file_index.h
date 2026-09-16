/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Streams dual-target IX03 indexes with one private secondary payload spool.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/cola_sections.h>
#include <diet/sort_profile_file_writer.h>

namespace diet {
  // Scratch bytes are never objects or durable checkpoints. Unlink the unique
  // private name immediately after opening it; the descriptor owns its life.
  struct posix_index_spool_ops {
    int create(std::filesystem::path const & path) noexcept {
      return ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    int remove(std::filesystem::path const & path) noexcept { return ::unlink(path.c_str()); }
    std::ptrdiff_t write(int fd, std::span<std::byte const> bytes) noexcept { return ::write(fd, bytes.data(), bytes.size()); }
    std::ptrdiff_t read_at(int fd, std::span<std::byte> bytes, std::uint64_t at) noexcept {
      return ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(at));
    }
    int close(int fd) noexcept { return ::close(fd); }
  };
  struct cola_file_dependencies {
    object_id native;
    std::optional<blob_identity> main;
    std::optional<object_id> secondary;
  };
  namespace cola_detail {
    template <class Ops> struct secondary_spool {
      secondary_spool(std::filesystem::path path, Ops & ops) : path_(std::move(path)), ops_(ops) {}
      secondary_spool(secondary_spool const &) = delete;
      secondary_spool & operator=(secondary_spool const &) = delete;
      ~secondary_spool() { if (fd_ >= 0) (void)ops_.close(fd_); }
      std::uint64_t body_bytes() const noexcept { return bytes_; }
      void append(std::span<std::byte const> bytes) {
        if (bytes.size() > std::uint64_t(std::numeric_limits<std::int64_t>::max()) - bytes_)
          throw std::length_error("secondary index spool extent");
        if (bytes.empty()) return;
        if (fd_ < 0) {
          fd_ = ops_.create(path_); if (fd_ < 0) fail("create secondary index spool");
          if (ops_.remove(path_) < 0) fail("unlink secondary index spool");
        }
        while (!bytes.empty()) {
          auto part = bytes.first(std::min(bytes.size(), std::size_t{1} << 20));
          auto n = ops_.write(fd_, part);
          if (n < 0 && errno == EINTR) continue;
          if (n <= 0 || std::uint64_t(n) > part.size()) fail("write secondary index spool", n < 0 ? errno : EIO);
          bytes_ += std::uint64_t(n); bytes = bytes.subspan(std::size_t(n));
        }
      }
      template <class Stream> void replay(Stream & out) {
        std::array<std::byte, 64 * 1024> buffer;
        std::uint64_t at = 0;
        while (at != bytes_) {
          auto part = std::span(buffer).first(std::size_t(std::min<std::uint64_t>(bytes_ - at, buffer.size())));
          auto n = ops_.read_at(fd_, part, at);
          if (n < 0 && errno == EINTR) continue;
          if (n <= 0 || std::uint64_t(n) > part.size()) fail("read secondary index spool", n < 0 ? errno : EIO);
          out.append(std::span<std::byte const>(part).first(std::size_t(n))); at += std::uint64_t(n);
        }
      }
    private:
      std::filesystem::path path_;
      Ops & ops_;
      int fd_ = -1;
      std::uint64_t bytes_ = 0;
      [[noreturn]] static void fail(char const * operation, int code = errno) {
        throw std::system_error(code, std::generic_category(), operation);
      }
    };
    template <class P> struct borrowed_file_state {
      profile_metadata metadata = profile_detail::initial_metadata<P, stream_role::borrowed>();
      std::vector<std::uint64_t> starts;
      elias_fano offsets;
      template <class Sink> void append(Sink & sink, bit_view key, std::uint64_t common) {
        if (key.size() & (P::bits_per_unit - 1)) throw std::invalid_argument("borrowed file key units");
        auto retained = common >> P::unit_shift;
        auto size = key.size() >> P::unit_shift;
        if (retained > metadata.terminal_key_units || retained > size) throw std::invalid_argument("borrowed file retained prefix");
        bool block = metadata.record_count % P::codec_block_size == 0;
        if (block) starts.push_back(sink.position() >> P::unit_shift);
        auto control = block ? retained : metadata.terminal_key_units - retained;
        if constexpr (P::unit == profile_unit::byte) count(sink, control);
        else if (block) sink.template write_count<exponential_golomb<0>>(control);
        else sink.template write_count<typename P::backspace_encoding>(control);
        count(sink, size - retained);
        sink.append(key.subview(retained << P::unit_shift, key.size() - (retained << P::unit_shift)));
        metadata.terminal_key_units = size; ++metadata.record_count;
      }
      template <class Sink> void finish(Sink & sink) {
        metadata.extent = sink.position() >> P::unit_shift;
        starts.push_back(metadata.extent); offsets = elias_fano::build(starts);
        starts.clear(); starts.shrink_to_fit(); sink.finish_payload();
      }
    private:
      template <class Sink> static void count(Sink & sink, std::uint64_t value) {
        if constexpr (P::unit == profile_unit::bit) sink.template write_count<exponential_golomb<0>>(value);
        else {
          do { auto byte = unsigned(value & 127); value >>= 7; sink.write_bits(byte | (value ? 128 : 0), 8); } while (value);
        }
      }
    };
    template <class P, class Ops, class SpoolOps> struct file_index_output {
      using stream_type = object_stream<P, Ops>;
      using spool_type = secondary_spool<SpoolOps>;
      using main_sink = sort_profile_detail::file_bit_sink<P, Ops>;
      using secondary_sink = sort_profile_detail::file_bit_sink<P, SpoolOps, spool_type>;
      file_index_output(std::filesystem::path root, object_id id, object_attempt_id attempt,
          cola_file_dependencies dependencies, Ops & ops, SpoolOps & spool_ops)
        : dependencies_(checked(std::move(dependencies))),
          stream_(std::move(root), std::move(id), std::move(attempt), file_kind::fractional_index,
            cola_section_detail::directory_bytes, ops),
          spool_(stream_.paths().private_output.string() + ".secondary", spool_ops), main_(stream_), secondary_(spool_) {}
      bool failed() const noexcept { return failed_ || stream_.failed(); }
      bool finished() const noexcept { return stream_.finished(); }
      object_write_paths const & paths() const & noexcept { return stream_.paths(); }
      object_write_paths const & paths() const && = delete;
      std::uint64_t spooled_bytes() const noexcept { return spool_.body_bytes(); }
      void append_known(unsigned route, bit_view key, std::uint64_t common) {
        require_active();
        try {
          if (!route) profiles_[0].append(main_, key, common);
          else if (route == 1) profiles_[1].append(secondary_, key, common);
          else throw std::out_of_range("borrowed file route");
        } catch (...) { failed_ = true; throw; }
      }
      template <class Native, class Main> object_seal_receipt finish(std::shared_ptr<Native const> native,
          std::shared_ptr<Main const> main, std::shared_ptr<Native const> secondary, index_metadata<P> metadata) {
        require_active();
        try {
          if (bool(main) != bool(dependencies_.main) || bool(secondary) != bool(dependencies_.secondary) || !native)
            throw std::invalid_argument("streamed COLA dependency shape");
          profiles_[0].finish(main_); profiles_[1].finish(secondary_);
          std::array<std::byte, cola_section_detail::directory_bytes> directory{};
          for (unsigned i = 0; i != 4; ++i) directory[i] = std::byte("IX03"[i]);
          file_detail::put(directory, 4, 2, 3); file_detail::put(directory, 6, 2, cola_section_detail::section_count);
          file_detail::put(directory, 8, 8, metadata.count); section_detail::put_id(directory, 88, dependencies_.native);
          if (dependencies_.main) {
            directory[82] = std::byte{1}; section_detail::put_id(directory, 104, dependencies_.main->native);
            section_detail::put_id(directory, 120, dependencies_.main->index);
          }
          if (dependencies_.secondary) { directory[83] = std::byte{1}; section_detail::put_id(directory, 136, *dependencies_.secondary); }
          std::array<std::uint64_t, cola_section_detail::section_count> lengths{};
          std::uint64_t count = 0;
          for (unsigned route = 0; route != 2; ++route) {
            auto const & state = profiles_[route]; auto const & m = state.metadata; auto const & ef = state.offsets;
            file_detail::put(directory, 16 + 8 * route, 8, m.record_count);
            file_detail::put(directory, 32 + 8 * route, 8, m.extent);
            file_detail::put(directory, 48 + 8 * route, 8, m.terminal_key_units);
            file_detail::put(directory, 64 + 8 * route, 8, ef.universe); directory[80 + route] = std::byte(ef.low_width);
            auto slot = cola_section_detail::profile_slot(route);
            lengths[slot] = profile_detail::byte_count(profile_detail::multiply(m.extent, P::bits_per_unit));
            lengths[slot + 1] = ef.low.size() * 8; lengths[slot + 2] = ef.high.size() * 8;
            lengths[slot + 3] = ef.samples.size() * 16; lengths[slot + 4] = ef.sparse.size() * 8;
            slot = cola_section_detail::rank_slot(route);
            lengths[slot] = metadata.ranks[route].classes.size() * 8;
            lengths[slot + 1] = metadata.ranks[route].checkpoints.size() * 8;
            lengths[cola_section_detail::flags_slot(route)] = metadata.flags[route].size();
            lengths[cola_section_detail::cuts_slot(route)] = metadata.cuts[route].size() * 8;
            count += m.record_count;
          }
          if (count > metadata.count || native->size() != metadata.count - count)
            throw std::invalid_argument("streamed COLA native count");
          std::uint64_t end = directory.size();
          for (std::size_t i = 0; i != lengths.size(); ++i) {
            auto start = profile_detail::add(end, 7) & ~std::uint64_t{7}; end = profile_detail::add(start, lengths[i]);
            file_detail::put(directory, cola_section_detail::descriptor_offset + (i << 4), 8, start);
            file_detail::put(directory, cola_section_detail::descriptor_offset + (i << 4) + 8, 8, lengths[i]);
          }
          emit_offsets(profiles_[0].offsets);
          main_.align(); spool_.replay(stream_); emit_offsets(profiles_[1].offsets);
          for (unsigned route = 0; route != 2; ++route) {
            main_.align(); main_.words(metadata.ranks[route].classes);
            main_.align(); main_.words(metadata.ranks[route].checkpoints);
          }
          for (auto const & flags : metadata.flags) { main_.align(); stream_.append(flags); }
          for (auto const & cuts : metadata.cuts) { main_.align(); main_.words(cuts); }
          file_header<P> header{file_kind::fractional_index, profile_detail::multiply(end, 1u << (3 - P::unit_shift)), count, 0};
          return stream_.finish(header, directory);
        } catch (...) { failed_ = true; throw; }
      }
    private:
      cola_file_dependencies dependencies_;
      stream_type stream_;
      spool_type spool_;
      main_sink main_;
      secondary_sink secondary_;
      std::array<borrowed_file_state<P>, 2> profiles_;
      bool failed_ = false;
      void require_active() const { if (failed() || finished()) throw std::logic_error("inactive streamed COLA output"); }
      static cola_file_dependencies checked(cola_file_dependencies deps) {
        auto valid = [](object_id const & id) { if (id.hex().size() != 32) throw std::invalid_argument("COLA file dependency identity"); };
        valid(deps.native); if (deps.main) { valid(deps.main->native); valid(deps.main->index); }
        if (deps.secondary) valid(*deps.secondary);
        return deps;
      }
      void emit_offsets(elias_fano const & ef) {
        main_.align(); main_.words(ef.low); main_.align(); main_.words(ef.high);
        main_.align(); main_.samples(ef.samples); main_.align(); main_.words(ef.sparse);
      }
    };
    template <class Output> struct file_index_output_ref {
      Output * output;
      bool failed() const noexcept { return output->failed(); }
      void append_known(unsigned route, bit_view key, std::uint64_t common) { output->append_known(route, key, common); }
      template <class Native, class Main, class Metadata> auto finish(std::shared_ptr<Native const> native,
          std::shared_ptr<Main const> main, std::shared_ptr<Native const> secondary, Metadata metadata) {
        return output->finish(std::move(native), std::move(main), std::move(secondary), std::move(metadata));
      }
    };
  }
  // Exact sources remain pinned. Only finish seals an adoptable IX03 object;
  // a partial stream/spool is rebuilt from its source recipe after restart.
  template <class P, class Native = profile_array<P>, class Main = void,
            class Ops = posix_object_ops, class SpoolOps = posix_index_spool_ops>
  struct cola_file_index_builder {
    using output_type = cola_detail::file_index_output<P, Ops, SpoolOps>;
    using builder_type = cola_index_builder<P, Native, Main, cola_detail::file_index_output_ref<output_type>>;
    using native_pointer = typename builder_type::native_pointer;
    using main_pointer = typename builder_type::main_pointer;
    cola_file_index_builder(std::filesystem::path root, object_id id, object_attempt_id attempt,
        cola_file_dependencies dependencies, native_pointer native, main_pointer main = {}, native_pointer secondary = {})
      : owned_ops_(std::in_place), owned_spool_ops_(std::in_place),
        output_(std::move(root), std::move(id), std::move(attempt), std::move(dependencies), *owned_ops_, *owned_spool_ops_),
        builder_({&output_}, std::move(native), std::move(main), std::move(secondary)) {}
    cola_file_index_builder(std::filesystem::path root, object_id id, object_attempt_id attempt,
        cola_file_dependencies dependencies, native_pointer native, main_pointer main, native_pointer secondary,
        Ops & ops, SpoolOps & spool_ops)
      : output_(std::move(root), std::move(id), std::move(attempt), std::move(dependencies), ops, spool_ops),
        builder_({&output_}, std::move(native), std::move(main), std::move(secondary)) {}
    bool done() const noexcept { return builder_.done(); }
    bool failed() const noexcept { return builder_.failed(); }
    bool finished() const noexcept { return builder_.finished(); }
    std::uint64_t size() const noexcept { return builder_.size(); }
    std::uint64_t step(std::uint64_t budget) { return builder_.step(budget); }
    object_seal_receipt finish() { return builder_.finish(); }
    std::uint64_t spooled_bytes() const noexcept { return output_.spooled_bytes(); }
    object_write_paths const & paths() const & noexcept { return output_.paths(); }
    object_write_paths const & paths() const && = delete;
  private:
    std::optional<Ops> owned_ops_;
    std::optional<SpoolOps> owned_spool_ops_;
    output_type output_;
    builder_type builder_;
  };
}
