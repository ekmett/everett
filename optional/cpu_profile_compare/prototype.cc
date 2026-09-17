/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures complete CPU native merges on identical logical byte strings.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include "../host_backend.h"
#include "../fixed_kv_compare/cases.h"
#include <everett/native_merge.h>
#include <everett/sections.h>
#include <everett/sort_profile_file.h>
#include <everett/sort_profile_merge.h>
#include <everett/typed_world.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <sys/mman.h>
#include <unistd.h>

namespace {
  using clock_type = std::chrono::steady_clock;
  using record = fixed_fixture::record;
  using strings = everett::unsorted<std::optional<std::string>>;
  using typed_bit_policy = everett_experiment::policy<everett::bin<everett::tip<strings>, everett::sort_undefined>>;
  using typed_byte_policy = everett_experiment::policy<>;
  template <bool Byte, bool Fixed> using raw_policy = everett_experiment::policy<everett::tip<everett::encoded_sort<
    std::conditional_t<Byte, everett::byte_encoding<std::conditional_t<Fixed, everett::fixed_values<16>, everett::variable_values>>,
      everett::bit_encoding<std::conditional_t<Fixed, everett::fixed_values<128>, everett::variable_values>>>>>>;

  void require(bool value, char const *message) { if (!value) throw std::runtime_error(message); }
  double elapsed(clock_type::time_point start) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
  }
  std::string value_string(record const &row) {
    std::string result;
    if (!row.value.empty()) result.assign(reinterpret_cast<char const *>(row.value.data()), row.value.size());
    return result;
  }
  struct output_mapping {
    int descriptor = -1;
    std::byte *data = nullptr;
    std::size_t capacity = 0;
    output_mapping(std::filesystem::path const &path, std::size_t bytes) {
      auto page = ::sysconf(_SC_PAGESIZE);
      require(page > 0 && bytes && bytes <= std::numeric_limits<std::size_t>::max() - std::size_t(page), "mapping extent");
      capacity = (bytes + std::size_t(page) - 1) / std::size_t(page) * std::size_t(page);
      descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
      require(descriptor >= 0, "create output");
      if (::ftruncate(descriptor, off_t(capacity))) { ::close(descriptor); descriptor = -1; throw std::runtime_error("size output"); }
      void *address = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
      if (address == MAP_FAILED) { ::close(descriptor); descriptor = -1; throw std::runtime_error("map output"); }
      data = static_cast<std::byte *>(address);
    }
    output_mapping(output_mapping const &) = delete;
    output_mapping &operator=(output_mapping const &) = delete;
    ~output_mapping() { if (data) ::munmap(data, capacity); if (descriptor >= 0) ::close(descriptor); }
    void clip(std::size_t bytes) { require(::ftruncate(descriptor, off_t(bytes)) == 0, "clip output"); }
  };
  template <class P, class Sections> std::size_t write_mapped(std::filesystem::path const &path, Sections const &sections) {
    auto const &metadata = sections.header();
    auto total = everett::file_detail::total_bytes<P>(metadata.extent);
    require(total <= std::numeric_limits<std::size_t>::max(), "output too large");
    output_mapping mapped(path, std::size_t(total));
    std::size_t offset = everett::file_detail::header_bytes;
    for (auto chunk : sections.chunks()) {
      require(chunk.size() <= total - offset, "section extent");
      if (!chunk.empty()) std::memcpy(mapped.data + offset, chunk.data(), chunk.size());
      offset += chunk.size();
    }
    require(offset == total, "incomplete output sections");
    auto crc = everett::crc32c<everett_experiment::architecture>(std::span<std::byte const>(mapped.data + everett::file_detail::header_bytes,
      std::size_t(total) - everett::file_detail::header_bytes));
    auto header = everett::encode_file_header(metadata, crc);
    std::memcpy(mapped.data, header.data(), header.size());
    mapped.clip(total);
    return total;
  }
  void write_file(std::filesystem::path const &path, std::span<std::byte const> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<char const *>(bytes.data()), bytes.size());
    require(bool(stream), "write input");
  }
  template <class P, bool Typed, bool SortProfile = false, class Native> void verify(Native const &native, std::vector<record> const &records) {
    auto cursor = native.view().cursor();
    for (auto const &row : records) {
      require(!cursor.done(), "logical output short");
      auto frame = cursor.peek();
      auto key = matched_fixture::key_bytes(row.k);
      if constexpr (Typed) {
        auto equal = [&] {
          if constexpr (SortProfile) {
            // KV03 carries the one-bit selector and raw order bytes; the
            // opaque KV02 typed transport has a different key grammar.
            if (frame.key.prefix.size() != 129 || frame.key.prefix.at(0)) return false;
            auto expected_key = everett::bit_string::from_bytes(key);
            return everett::compare_bits(frame.key.prefix.subview(1, 128), expected_key.view()) == 0;
          } else return everett::typed_detail::dispatch_key<P>(frame.key.prefix,
            [&]<class S>(std::type_identity<S>, auto const &decoded) {
              if constexpr (std::is_same_v<S, strings>) return decoded == key;
              else return false;
            });
        }();
        require(equal, "typed logical key");
        auto value = everett::typed_detail::value<P, strings>(frame.value);
        require(value && *value == value_string(row), "typed logical value");
      } else {
        auto expected_key = everett::bit_string::from_bytes(key);
        require(everett::compare_bits(frame.key.prefix, expected_key.view()) == 0, "raw logical key");
        auto expected_value = everett::bit_string::from_bytes(std::span<std::byte const>(row.value));
        require(everett::compare_bits(frame.value, expected_value.view()) == 0, "raw logical value");
      }
      cursor.advance();
    }
    require(cursor.done(), "logical output long");
  }
  struct merge_result { double merge_ms, output_ms; std::size_t bytes; };
  template <class P, bool Typed = false> struct raw_codec {
    using native = everett::mapped_native<P>;
    using pointer = std::shared_ptr<native const>;
    static std::vector<everett::profile_record> encode_records(std::vector<record> const &rows) {
      std::vector<everett::profile_record> records;
      records.reserve(rows.size());
      for (auto const &row : rows) {
        auto key = matched_fixture::key_bytes(row.k);
        if constexpr (Typed) records.push_back({everett::typed_detail::key<P, strings>(key),
          everett::typed_detail::value<P, strings>(std::optional<std::string>(value_string(row)))});
        else records.push_back({everett::bit_string::from_bytes(key), everett::bit_string::from_bytes(std::span<std::byte const>(row.value))});
      }
      return records;
    }
    static std::vector<std::byte> encode(std::vector<record> const &rows) {
      auto records = encode_records(rows);
      // Match ordinary runtime construction: common width is the policy's
      // declared width, not an untimed whole-batch inference unavailable to
      // the incremental production merge.
      everett::profile_native_writer<P> writer;
      for (auto const &record : records) writer.append(record);
      auto array = writer.finish();
      return everett::encode_native_sections(array).materialize();
    }
    static pointer open(std::filesystem::path const &path) {
      auto result = std::make_shared<native const>(native::open(path)); result->scan(); return result;
    }
    static void check(native const &input, std::vector<record> const &rows) { verify<P, Typed>(input, rows); }
    static merge_result merge(pointer const &a, pointer const &b, std::filesystem::path const &path) {
      auto start = clock_type::now();
      everett::native_merge_builder<P, native> merge(a, b);
      merge.step(std::numeric_limits<std::uint64_t>::max());
      auto result = merge.finish();
      auto merge_ms = elapsed(start);
      start = clock_type::now();
      auto sections = everett::encode_native_sections(result);
      auto bytes = write_mapped<P>(path, sections);
      return {merge_ms, elapsed(start), bytes};
    }
  };
  struct typed_bit_codec {
    using P = typed_bit_policy;
    using native = everett::mapped_sort_profile<P>;
    using pointer = std::shared_ptr<native const>;
    static std::vector<std::byte> encode(std::vector<record> const &rows) {
      everett::sort_profile_writer<P> writer;
      for (auto const &row : rows) writer.template append<strings>(matched_fixture::key_bytes(row.k),
        std::optional<std::string>(value_string(row)));
      auto result = writer.finish();
      return everett::encoded_sort_sections<P>::from(result).materialize();
    }
    static pointer open(std::filesystem::path const &path) {
      auto result = std::make_shared<native const>(native::open(everett::file<P>::open(path)));
      result->scan(); return result;
    }
    static void check(native const &input, std::vector<record> const &rows) { verify<P, true, true>(input, rows); }
    static merge_result merge(pointer const &a, pointer const &b, std::filesystem::path const &path) {
      auto start = clock_type::now();
      everett::sort_profile_merge_builder<P, native> merge(a, b);
      merge.step(std::numeric_limits<std::uint64_t>::max());
      auto result = merge.finish();
      auto merge_ms = elapsed(start);
      start = clock_type::now();
      auto sections = everett::encoded_sort_sections<P>::from(result);
      auto bytes = write_mapped<P>(path, sections);
      return {merge_ms, elapsed(start), bytes};
    }
  };
  template <class Codec> void run(char const *mode, unsigned index, unsigned trials, std::filesystem::path const &root) {
    auto fixture = matched_fixture::make(index);
    auto expected = matched_fixture::merged(fixture);
    std::filesystem::create_directories(root);
    matched_fixture::save(root, fixture, expected);
    auto left = Codec::encode(fixture.a), right = Codec::encode(fixture.b), oracle = Codec::encode(expected);
    write_file(root / "input-a.kv", left); write_file(root / "input-b.kv", right);
    auto a = Codec::open(root / "input-a.kv"), b = Codec::open(root / "input-b.kv");
    Codec::check(*a, fixture.a); Codec::check(*b, fixture.b);
    std::size_t value_bytes = 0;
    for (auto const &row : expected) value_bytes += row.value.size();
    std::cout << std::setprecision(17)
      << "case,mode,distribution,older,newer,cancelled,trial,input_a_bytes,input_b_bytes,output_bytes,output_records,value_bytes,complete_ms,merge_ms,output_ms\n";
    for (int trial = -1; trial < int(trials); ++trial) {
      auto start = clock_type::now();
      auto result = Codec::merge(a, b, root / "output.kv");
      auto complete = elapsed(start);
      std::ifstream input(root / "output.kv", std::ios::binary);
      std::vector<char> actual((std::istreambuf_iterator<char>(input)), {});
      require(actual.size() == oracle.size() && std::memcmp(actual.data(), oracle.data(), oracle.size()) == 0, "canonical CPU output");
      auto checked = Codec::open(root / "output.kv");
      if (trial < 0) Codec::check(*checked, expected);
      std::cout << index << ',' << mode << ',' << (matched_fixture::cases[index].hashed ? "hash-like" : "structured")
        << ',' << fixture.a.size() << ',' << fixture.b.size() << ",0," << trial << ',' << left.size() << ',' << right.size()
        << ',' << result.bytes << ',' << expected.size() << ',' << value_bytes << ',' << complete << ',' << result.merge_ms
        << ',' << result.output_ms << '\n';
    }
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 5, "usage: cpu-profile MODE DIRECTORY CASE TRIALS");
    std::string mode(argv[1]);
    auto index = unsigned(std::stoul(argv[3])), trials = unsigned(std::stoul(argv[4]));
    require(index < matched_fixture::cases.size() && trials && trials <= 100, "case/trial bounds");
    auto variable = matched_fixture::cases[index].variable;
    if (mode == "raw-bit") {
      if (variable) run<raw_codec<raw_policy<false, false>>>(argv[1], index, trials, argv[2]);
      else run<raw_codec<raw_policy<false, true>>>(argv[1], index, trials, argv[2]);
    } else if (mode == "raw-byte") {
      if (variable) run<raw_codec<raw_policy<true, false>>>(argv[1], index, trials, argv[2]);
      else run<raw_codec<raw_policy<true, true>>>(argv[1], index, trials, argv[2]);
    } else if (mode == "typed-bit") run<typed_bit_codec>(argv[1], index, trials, argv[2]);
    else if (mode == "typed-byte") run<raw_codec<typed_byte_policy, true>>(argv[1], index, trials, argv[2]);
    else throw std::invalid_argument("unknown profile mode");
    return 0;
  } catch (std::exception const &error) { std::cerr << error.what() << '\n'; return 1; }
}
