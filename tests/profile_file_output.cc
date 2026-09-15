/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/profile_file_output.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  using namespace diet;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class E = std::exception, class F> void rejects(F && f) {
    bool caught = false;
    try { f(); } catch (E const &) { caught = true; }
    require(caught, "invalid borrowed output operation accepted");
  }
  object_id id(unsigned n = 1) {
    char text[33]; std::snprintf(text, sizeof text, "0123456789abcdef01234567%08x", n); return object_id(text);
  }
  object_attempt_id attempt(unsigned n = 1) {
    char text[33]; std::snprintf(text, sizeof text, "fedcba9876543210fedcba98%08x", n); return object_attempt_id(text);
  }
  unsigned bit(bit_view v, std::uint64_t at) {
    at += v.offset();
    return (std::to_integer<unsigned>(v.storage()[at >> 3]) >> (7 - (at & 7))) & 1;
  }
  std::uint64_t common(bit_view a, bit_view b) {
    std::uint64_t n = 0;
    while (n != std::min(a.size(), b.size()) && bit(a, n) == bit(b, n)) ++n;
    return n;
  }
  void equal(bit_view a, bit_view b) {
    require(a.size() == b.size(), "borrowed key length mismatch");
    require(common(a, b) == a.size(), "borrowed key bits mismatch");
  }
  struct model_ops {
    static constexpr bool supported = true;
    std::vector<std::string> calls;
    std::optional<std::size_t> fail_at;
    int fail_errno = EIO;
    bool install_before_failure = false, header_before_failure = false;
    bool short_writes = false;
    bool interrupt_write = false, interrupt_header = false;
    bool zero_write = false, zero_header = false;
    bool private_exists = false, final_exists = false, read_only = false;
    std::array<bool, 4> opened{};
    std::vector<std::byte> bytes;
    std::size_t offset = 0;
    std::span<std::byte const> source;
    bool direct_source = false;
    unsigned writes = 0, headers = 0, installs = 0, removals = 0;
    object_sync_barrier barrier() const noexcept { return object_sync_barrier::fsync; }
    bool event(char const * name) {
      calls.emplace_back(name);
      if (fail_at && calls.size() - 1 == *fail_at) { errno = fail_errno; return false; }
      return true;
    }
    int open_root(std::filesystem::path const &) {
      if (!event("open root")) return -1;
      opened[0] = true;
      return 10;
    }
    int make_directory(int, char const *) { return event("mkdir") ? 0 : -1; }
    int open_directory(int parent, char const *) {
      if (!event("open directory")) return -1;
      opened[std::size_t(parent - 9)] = true;
      return parent + 1;
    }
    int create_private(int, char const *) {
      if (!event("create private")) return -1;
      if (private_exists) { errno = EEXIST; return -1; }
      private_exists = true;
      opened[3] = true;
      return 13;
    }
    std::ptrdiff_t write(int, std::span<std::byte const> data) {
      ++writes;
      if (!event("write")) return -1;
      if (zero_write) return 0;
      if (std::exchange(interrupt_write, false)) { errno = EINTR; return -1; }
      if (!source.empty() && data.data() == source.data()) direct_source = true;
      auto count = short_writes ? std::min(std::size_t{7}, data.size()) : data.size();
      bytes.resize(offset + count);
      std::copy_n(data.begin(), count, bytes.begin() + std::ptrdiff_t(offset));
      offset += count;
      return std::ptrdiff_t(count);
    }
    std::ptrdiff_t write_at(int, std::span<std::byte const> data, std::uint64_t at) {
      ++headers;
      bool acknowledged = event("pwrite");
      if (!acknowledged && !header_before_failure) return -1;
      if (zero_header) return 0;
      if (std::exchange(interrupt_header, false)) { errno = EINTR; return -1; }
      auto count = short_writes ? std::min(std::size_t{3}, data.size()) : data.size();
      require(at + count <= bytes.size(), "header write exceeded reserved prefix");
      std::copy_n(data.begin(), count, bytes.begin() + std::ptrdiff_t(at));
      return acknowledged ? std::ptrdiff_t(count) : -1;
    }
    int make_read_only(int) {
      if (!event("chmod")) return -1;
      read_only = true;
      return 0;
    }
    int sync_file(int) { return event("sync file") ? 0 : -1; }
    int sync_directory(int fd) {
      require(fd >= 10 && fd <= 12, "wrong directory sync handle");
      return event(fd == 12 ? "sync leaf" : fd == 11 ? "sync first" : "sync root") ? 0 : -1;
    }
    int install(int, char const *, char const *) {
      ++installs;
      bool acknowledged = event("link");
      if (!acknowledged && !install_before_failure) return -1;
      require(private_exists && read_only, "installed writable or absent private output");
      if (final_exists) { errno = EEXIST; return -1; }
      final_exists = true;
      return acknowledged ? 0 : -1;
    }
    int remove_private(int, char const *) {
      ++removals;
      if (!event("unlink private")) return -1;
      require(final_exists, "removed only output name");
      private_exists = false;
      return 0;
    }
    int close(int fd) {
      require(fd >= 10 && fd <= 13 && opened[std::size_t(fd - 10)], "closed descriptor twice");
      opened[std::size_t(fd - 10)] = false; // close errors can consume the descriptor.
      return event("close") ? 0 : -1;
    }
  };

  template <class P> using output = profile_detail::profile_file_output<P, stream_role::borrowed, model_ops>;
  template <class P> bit_string key(unsigned number) {
    std::string bits(67, '0');
    for (unsigned j = 20; j; --j) bits += char('0' + ((number >> (j - 1)) & 1));
    if constexpr (P::unit == profile_unit::byte) bits.append((8 - bits.size() % 8) % 8, '0');
    return bit_string::from_bits(bits);
  }
  template <class P> std::vector<bit_string> keys(unsigned count) {
    std::vector<bit_string> result;
    for (unsigned i = 0; i != count; ++i) result.push_back(i < 2 ? bit_string{} : key<P>((i / 2) * 2));
    return result;
  }
  template <class P> profile_blob<P> pair_for(std::span<bit_string const> borrowed, unsigned natives = 0) {
    std::vector<profile_record> native;
    for (unsigned i = 0; i != natives; ++i) {
      auto units = P::value_width.value_or(i % 5);
      native.push_back({i ? key<P>(i * 3) : bit_string{},
        bit_string::from_bits(std::string(units * P::bits_per_unit, '1'))});
    }
    return profile_blob<P>::build(native, borrowed);
  }
  template <class P> profile_detail::index_file_sections<P> sections(profile_blob<P> const & pair,
      object_id const & native, std::optional<blob_identity> const & target) {
    return {native, target, pair.interleave(), pair.false_borrow_bits(), pair.cut_lcps(), pair.virtual_size()};
  }
  template <class P> void append_all(output<P> & writer, std::span<bit_string const> input) {
    bit_view previous;
    for (auto const & current : input) {
      auto retained = common(previous, current.view()) >> P::unit_shift;
      auto suffix = current.view().subview(retained << P::unit_shift, current.bit_size - (retained << P::unit_shift));
      // The literal comes from temporary, unaligned scratch and expires now.
      auto scratch = bit_string::from_bits("101");
      profile_detail::append(scratch, suffix);
      writer.append(retained, scratch.view().subview(3, suffix.size()), {});
      previous = current.view();
    }
  }
  template <class P> void oracle(std::span<std::byte const> wire, profile_blob<P> const & pair,
      std::span<bit_string const> input, object_id const & native, std::optional<blob_identity> target) {
    auto expected = encode_index_sections(pair, native, target).materialize();
    require(std::equal(wire.begin(), wire.end(), expected.begin(), expected.end()), "streamed .index wire differs from batch encoder");
    auto header = validate_file<P>(wire);
    require(header.kind == file_kind::fractional_index && header.common_value_width == 0,
      "borrowed envelope lost zero-width role");
    auto body = wire.subspan(file_detail::header_bytes);
    auto layout = section_detail::parse(header, body);
    require(layout.native_id == native && layout.target_id == target && layout.virtual_count == pair.virtual_size(),
      "streamed index identity/count mismatch");
    auto view = section_detail::profile<P, stream_role::borrowed>(header, body, layout);
    section_detail::scan_profile(view);
    auto cursor = view.cursor();
    bit_view previous;
    for (std::size_t i = 0; i != input.size(); ++i) {
      require(!cursor.done() && cursor.peek().ordinal == i && cursor.peek().value.empty(), "borrowed record ordinal/value");
      equal(cursor.peek().key.prefix, input[i].view());
      auto encoded = view.encoded_at(i);
      require(encoded.retained == (common(previous, input[i].view()) >> P::unit_shift), "borrowed physical LCP mismatch");
      previous = input[i].view();
      cursor.advance();
    }
    require(cursor.done(), "extra borrowed file records");
  }
  template <class P> void matrix() {
    for (auto count : {0u, 1u, unsigned(P::codec_block_size - 1), unsigned(P::codec_block_size),
        unsigned(P::codec_block_size + 1), unsigned(2 * P::codec_block_size + 1), 513u}) {
      auto input = keys<P>(count);
      auto pair = pair_for<P>(input, count / 3 + 3);
      auto native = id(2);
      std::optional<blob_identity> target = count ? std::optional(blob_identity{id(3), id(4)}) : std::nullopt;
      model_ops ops;
      output<P> writer("model-root", id(), attempt(), 0, ops);
      require(writer.common_value_width() == 0 && !writer.failed() && !writer.finished(), "borrowed initial state");
      append_all<P>(writer, input);
      auto receipt = writer.finish(sections(pair, native, target));
      require(writer.finished() && !writer.failed() && writer.size() == count && receipt.bytes == ops.bytes.size(), "borrowed finish state");
      oracle<P>(ops.bytes, pair, input, native, target);
      auto calls = ops.calls.size();
      rejects<std::logic_error>([&] { writer.append(0, {}, {}); });
      rejects<std::logic_error>([&] { writer.finish(sections(pair, native, target)); });
      require(calls == ops.calls.size(), "finished borrowed output touched I/O");
    }
    auto pair = pair_for<P>({});
    auto native = id(2);
    std::optional<blob_identity> target;
    model_ops ops;
    output<P> empty("model-root", id(), attempt(), 0, ops);
    empty.finish(sections(pair, native, target));
    oracle<P>(ops.bytes, pair, {}, native, target);
  }
  void rejected_metadata() {
    using P = storage_policy<profile_unit::byte, fixed_values<7>, 3, exponential_golomb<0>, 16>;
    auto input = keys<P>(5);
    auto pair = pair_for<P>(input, 4);
    auto native = id(2);
    std::optional<blob_identity> target = blob_identity{id(3), id(4)}, missing;
    model_ops ops;
    output<P> writer("model-root", id(), attempt(), 0, ops);
    append_all<P>(writer, input);
    auto calls = ops.calls.size();
    auto original = ops.bytes;
    auto rejected = [&](auto const & metadata) {
      rejects<std::invalid_argument>([&] { writer.finish(metadata); });
      require(!writer.failed() && !writer.finished() && writer.size() == input.size() &&
        calls == ops.calls.size() && ops.bytes == original, "metadata rejection changed output or performed I/O");
    };
    auto good = sections(pair, native, target);
    auto absent = sections(pair, native, missing); rejected(absent);
    auto short_flags = good; short_flags.false_borrows = {}; rejected(short_flags);
    auto short_cuts = good; short_cuts.cut_lcps = good.cut_lcps.first(good.cut_lcps.size() - 1); rejected(short_cuts);
    auto bad_count = good; --bad_count.virtual_count; rejected(bad_count);
    auto below_count = good; below_count.virtual_count = input.size() - 1; rejected(below_count);
    auto padded = std::vector<std::byte>(good.false_borrows.begin(), good.false_borrows.end());
    padded.back() |= std::byte{0x80};
    auto padding = good; padding.false_borrows = padded; rejected(padding);
    auto malformed = pair.interleave(); malformed.classes.clear();
    auto wrong_shape = profile_detail::index_file_sections<P>{native, target, malformed,
      good.false_borrows, good.cut_lcps, good.virtual_count}; rejected(wrong_shape);
    std::vector<std::uint64_t> zero_classes(pair.group_count());
    auto wrong_rank = rank_groups<P::group_size>::build(zero_classes, pair.virtual_size());
    auto wrong_total = profile_detail::index_file_sections<P>{native, target, wrong_rank,
      good.false_borrows, good.cut_lcps, good.virtual_count}; rejected(wrong_total);
    auto value = bit_string::from_bytes("x");
    rejects<std::invalid_argument>([&] { writer.append(0, {}, value.view()); });
    rejects<std::invalid_argument>([&] { writer.append(std::numeric_limits<std::uint64_t>::max(), {}, {}); });
    auto bit_key = bit_string::from_bits("1");
    rejects<std::invalid_argument>([&] { writer.append(0, bit_key.view(), {}); });
    require(calls == ops.calls.size() && !writer.failed() && writer.size() == input.size(), "invalid append changed borrowed sink");
    writer.finish(good); oracle<P>(ops.bytes, pair, input, native, target);
    for (auto width : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{7}}) {
      model_ops bad;
      rejects<std::invalid_argument>([&] { output<P> invalid("model-root", id(), attempt(), width, bad); });
      require(bad.calls.empty(), "invalid borrowed width performed I/O");
    }
  }
  void final_section_failures() {
    using P = storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>;
    auto input = keys<P>(101);
    auto pair = pair_for<P>(input, 73);
    auto native = id(2);
    std::optional<blob_identity> target = blob_identity{id(3), id(4)};
    model_ops baseline;
    std::size_t first, last;
    {
      output<P> writer("model-root", id(), attempt(), 0, baseline);
      append_all<P>(writer, input); first = baseline.calls.size();
      writer.finish(sections(pair, native, target)); last = baseline.calls.size();
    }
    unsigned checked = 0;
    for (auto cut = first; cut != last; ++cut) {
      if (baseline.calls[cut] != "write") continue;
      ++checked;
      model_ops ops; ops.fail_at = cut;
      {
        output<P> writer("model-root", id(), attempt(), 0, ops);
        append_all<P>(writer, input);
        rejects<object_write_error>([&] { writer.finish(sections(pair, native, target)); });
        require(writer.failed() && !writer.finished() && ops.private_exists && !ops.final_exists,
          "final-section I/O failure did not preserve poisoned private attempt");
        auto calls = ops.calls.size();
        rejects<std::logic_error>([&] { writer.finish(sections(pair, native, target)); });
        rejects<std::logic_error>([&] { writer.append(0, {}, {}); });
        require(calls == ops.calls.size(), "poisoned borrowed output retried I/O");
      }
      require(std::none_of(ops.opened.begin(), ops.opened.end(), [](bool x) { return x; }), "section I/O fault leaked descriptors");
    }
    require(checked >= 8, "fault fixture did not reach appended index metadata sections");
    model_ops short_ops; short_ops.short_writes = true; short_ops.interrupt_write = true; short_ops.interrupt_header = true;
    output<P> short_writer("model-root", id(), attempt(), 0, short_ops);
    append_all<P>(short_writer, input); short_writer.finish(sections(pair, native, target));
    oracle<P>(short_ops.bytes, pair, input, native, target);
  }
  void large_controls() {
    using P = storage_policy<profile_unit::bit, fixed_values<13>, 7, golomb<1>, 16>;
    std::vector<bit_string> input{bit_string::from_bits(std::string(600001, '0')),
      bit_string::from_bits("1"), bit_string::from_bits("1"), bit_string::from_bits("10")};
    auto pair = pair_for<P>(input);
    auto native = id(2);
    std::optional<blob_identity> target = blob_identity{id(3), id(4)};
    model_ops ops;
    output<P> writer("model-root", id(), attempt(), 0, ops);
    append_all<P>(writer, input); writer.finish(sections(pair, native, target));
    oracle<P>(ops.bytes, pair, input, native, target);
  }
#if defined(__APPLE__) || defined(__linux__)
  template <class P> void guarded_literal() {
    auto page_result = ::sysconf(_SC_PAGESIZE);
    require(page_result > 0, "page size unavailable");
    auto page = static_cast<std::size_t>(page_result), length = 17 * page;
    auto address = ::mmap(nullptr, length + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(address != MAP_FAILED, "guarded borrowed literal allocation");
    auto data = static_cast<std::byte *>(address);
    require(::mprotect(data + length, page, PROT_NONE) == 0, "guarded borrowed literal end");
    for (std::size_t i = 0; i != length; ++i) data[i] = std::byte(i * 17);
    auto storage = std::span<std::byte const>(data + 1, length - 1);
    auto source = P::unit == profile_unit::byte ? bit_view(storage, storage.size() * 8) :
      bit_view(storage, storage.size() * 8 - 7, 3);
    std::vector<bit_string> input{bit_string::copy(source), bit_string::copy(source)};
    auto pair = pair_for<P>(input);
    auto native = id(2);
    std::optional<blob_identity> target = blob_identity{id(3), id(4)};
    model_ops ops; ops.source = storage;
    output<P> writer("model-root", id(), attempt(), 0, ops);
    writer.append(0, source, {});
    writer.append(source.size() >> P::unit_shift, {}, {});
    if constexpr (P::unit == profile_unit::byte)
      require(ops.direct_source, "large aligned borrowed literal was copied into staging buffer");
    require(::mprotect(address, length, PROT_NONE) == 0, "protect consumed borrowed literal");
    writer.finish(sections(pair, native, target));
    require(::munmap(address, length + page) == 0, "unmap borrowed literal");
    oracle<P>(ops.bytes, pair, input, native, target);
  }
  void real_file() {
    using P = storage_policy<profile_unit::byte, fixed_values<11>, 15, exponential_golomb<0>, 16>;
    auto pattern = (std::filesystem::temp_directory_path() / "diet-borrowed-file-XXXXXX").string();
    auto created = ::mkdtemp(pattern.data()); require(created, "borrowed file temporary directory");
    std::filesystem::path root(created);
    auto input = keys<P>(37); auto pair = pair_for<P>(input, 11); auto native = id(2);
    std::optional<blob_identity> target = blob_identity{id(3), id(4)};
    {
      // Default width remains zero even though native values have width eleven.
      profile_detail::profile_file_output<P, stream_role::borrowed> writer(root, id(), attempt());
      require(writer.common_value_width() == 0, "fixed policy leaked native value width into borrowed role");
      bit_view previous;
      for (auto const & item : input) {
        auto retained = common(previous, item.view()) >> P::unit_shift;
        writer.append(retained, item.view().subview(retained << P::unit_shift, item.bit_size - (retained << P::unit_shift)), {});
        previous = item.view();
      }
      auto receipt = writer.finish(sections(pair, native, target));
      auto mapped = mapped_index<P>::open(receipt.path); mapped.scan();
      auto file = mapped_file::open(receipt.path); auto bytes = file.slice(0, file.size());
      oracle<P>(bytes.bytes(), pair, input, native, target);
      std::filesystem::remove(receipt.path);
      require(mapped.borrowed().size() == input.size(), "unlinked borrowed mapping lost records");
      mapped.scan();
    }
    std::filesystem::remove_all(root);
  }
#endif
}
int main() {
  try {
    matrix<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
    matrix<storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::byte, fixed_values<8>, 15, exponential_golomb<0>, 16>>();
    matrix<storage_policy<profile_unit::byte, fixed_values<3>, 31, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::bit, variable_values, 3, golomb<3>, 16>>();
    matrix<storage_policy<profile_unit::bit, fixed_values<0>, 7, exponential_golomb<2>, 15>>();
    matrix<storage_policy<profile_unit::bit, fixed_values<13>, 15, golomb<5>, 16>>();
    matrix<storage_policy<profile_unit::bit, variable_values, 31, exponential_golomb<0>, 1>>();
    rejected_metadata(); final_section_failures(); large_controls();
#if defined(__APPLE__) || defined(__linux__)
    guarded_literal<storage_policy<profile_unit::byte>>();
    guarded_literal<storage_policy<profile_unit::bit, fixed_values<7>, 7, golomb<3>, 16>>();
    real_file();
#endif
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
