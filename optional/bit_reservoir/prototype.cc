/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares count decoding and real sort-owned bit streams across reader revisions.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sort_profile_file.h>
#include <everett/sort_profile_merge.h>

#include <chrono>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using registry = bin<tip<strings>, sort_undefined>;
  using policy = storage_policy<registry>;
  using array = sort_profile_array<policy>;
  using native = mapped_sort_profile<policy>;
  using row = std::pair<std::string, std::optional<std::string>>;
  using clock_type = std::chrono::steady_clock;
  void compiler_barrier() {
#if defined(__GNUC__)
    asm volatile("" : : : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }
  void require(bool condition, char const *message) { if (!condition) throw std::runtime_error(message); }
  std::uint64_t mix(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }
  void append_word(std::string &out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(char(value >> shift));
  }
  std::vector<row> rows(std::size_t count, bool hash_keys, unsigned value_max) {
    std::vector<row> result;
    for (std::size_t i = 0; i != count; ++i) {
      std::string key = hash_keys ? "" : "documents/persistent/shared/string/prefix/";
      append_word(key, hash_keys ? mix(i) : i);
      if (hash_keys) append_word(key, mix(i + 0x12345678));
      std::optional<std::string> value;
      if (i % 17) {
        value.emplace(mix(i + 3) % (value_max + 1), '\0');
        for (std::size_t j = 0; j != value->size(); ++j) (*value)[j] = char(mix(i * 65537 + j));
      }
      result.emplace_back(std::move(key), std::move(value));
    }
    std::sort(result.begin(), result.end(), [](auto const &a, auto const &b) { return a.first < b.first; });
    return result;
  }
  array encode(std::vector<row> const &input) {
    sort_profile_writer<policy> out;
    for (auto const &[key, value] : input) out.append<strings>(key, value);
    return out.finish();
  }
  auto save(array const &input, std::filesystem::path const &path) {
    auto bytes = encoded_sort_sections<policy>::from(input).materialize();
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<char const *>(bytes.data()), bytes.size());
    output.close(); require(bool(output), "write fixture");
    auto result = std::make_shared<native const>(native::open(path));
    result->scan();
    return result;
  }
  std::uint64_t frame_scan(native const &input) {
    auto view = input.view();
    if (!view.size()) return 0;
    auto frame = view.encoded_at(0);
    std::uint64_t result = 0;
    for (std::uint64_t i = 0; i != view.size(); ++i) {
      result += frame.next_offset + frame.retained + frame.key_units + frame.value.size();
      if (i + 1 != view.size()) frame = view.next(frame);
    }
    require(frame.next_offset == view.data().size(), "frame terminal offset");
    return result;
  }
  std::uint64_t cursor_scan(native const &input) {
    auto cursor = input.view().cursor();
    std::uint64_t result = 0;
    while (!cursor.done()) {
      auto item = cursor.peek();
      result += item.key.prefix.size() + item.value.size();
      if (!item.key.prefix.empty()) result += profile_detail::load_bits(item.key.prefix, 0, 8);
      cursor.advance();
    }
    return result;
  }
  void verify(native const &input, std::vector<row> const &expected) {
    auto cursor = input.view().cursor();
    for (auto const &[key, value] : expected) {
      require(!cursor.done(), "short decoded native");
      auto item = cursor.peek();
      auto expected_key = sort_profile_query<policy, strings>(key);
      require(compare_bits(item.key.prefix, expected_key.view()) == 0, "native key mismatch");
      sort_bit_reader reader(item.value);
      require(sort_codec<strings>::value_codec::read(reader) == value && reader.empty(), "native value mismatch");
      cursor.advance();
    }
    require(cursor.done(), "trailing decoded native");
  }
  template <class Function> void measure(std::string const &name, std::uint64_t items,
      std::uint64_t bits, unsigned trials, unsigned loops, Function &&function) {
    auto expected = function();
    for (unsigned trial = 0; trial <= trials; ++trial) {
      std::uint64_t checksum = 0;
      auto start = clock_type::now();
      for (unsigned repeat = 0; repeat != loops; ++repeat) {
        compiler_barrier();
        checksum += function();
      }
      auto ns = std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
      require(checksum == expected * loops, "timed checksum changed");
      std::cout << name << ',' << (trial == 0 ? "warmup" : "timed") << ',' << trial << ','
        << items << ',' << bits << ',' << loops << ',' << std::setprecision(12)
        << ns / (double(items) * loops) << ',' << checksum << '\n';
    }
  }
  template <class Code, class Generate> void count_case(char const *name, std::size_t count,
      unsigned trials, unsigned loops, Generate generate) {
    bit_string data; sort_bit_writer output(data);
    output.write_bits(5, 3);
    std::vector<std::uint64_t> values;
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i != count; ++i) {
      auto value = generate(i); values.push_back(value); expected += value;
      output.write_count<Code>(value);
    }
    auto bits = data.view().subview(3, data.bit_size - 3);
    sort_bit_reader check(bits);
    for (auto value : values) require(check.read_count<Code>() == value, "count roundtrip");
    require(check.empty(), "count terminal offset");
    auto run = [&] {
      sort_bit_reader input(bits); std::uint64_t sum = 0;
      for (std::size_t i = 0; i != count; ++i) sum += input.read_count<Code>();
      require(input.empty() && sum == expected, "count scan result");
      return sum + input.position();
    };
    measure(name, count, bits.size(), trials, loops, run);
  }
  void native_case(std::filesystem::path const &directory, std::size_t count,
      bool hash_keys, unsigned value_max, unsigned trials, unsigned loops) {
    auto name = std::string(hash_keys ? "hash" : "prefix") + '-' + std::to_string(value_max);
    auto input = rows(count, hash_keys, value_max);
    auto encoded = encode(input);
    auto mapped = save(encoded, directory / (name + ".kv"));
    verify(*mapped, input);
    measure(name + "/frames", count, encoded.data().bit_size, trials, loops, [&] { return frame_scan(*mapped); });
    measure(name + "/cursor", count, encoded.data().bit_size, trials, loops, [&] { return cursor_scan(*mapped); });

    sort_record_writer<registry> record_writer;
    for (auto const &[key, value] : input) record_writer.append<strings>(key, value);
    auto const &records = record_writer.data();
    {
      sort_record_reader<registry> check(records.view()); std::size_t i = 0;
      while (check.next([&](auto, auto const &key, auto const &value, auto const &) {
        require(i < input.size() && key == input[i].first && value == input[i].second, "record stream mismatch"); ++i;
      })) {}
      require(i == input.size(), "record stream count");
    }
    measure(name + "/records", count, records.bit_size, trials, loops, [&] {
      sort_record_reader<registry> reader(records.view()); std::uint64_t result = 0;
      while (reader.next([&](auto, auto const &key, auto const &value, auto const &control) {
        result += key.size() + control.end;
        if (!key.empty()) result += static_cast<unsigned char>(key.back());
        if (value) { result += value->size(); if (!value->empty()) result += static_cast<unsigned char>(value->back()); }
      })) {}
      return result;
    });
    for (bool overlap : {false, true}) {
      std::vector<row> older, newer;
      for (std::size_t i = 0; i != input.size(); ++i) {
        if (i % 2 == 0 || overlap) older.push_back(input[i]);
        if (i % 2) newer.push_back(input[i]);
      }
      auto a = save(encode(older), directory / (name + "-older.kv"));
      auto b = save(encode(newer), directory / (name + "-newer.kv"));
      auto merge = [&] {
        sort_profile_merge_builder<policy, native> builder(a, b);
        builder.step(std::numeric_limits<std::uint64_t>::max());
        return builder.finish();
      };
      auto expected_bytes = encoded_sort_sections<policy>::from(encoded).materialize();
      auto merged = merge();
      require(encoded_sort_sections<policy>::from(merged).materialize() == expected_bytes, "merged canonical bytes");
      measure(name + (overlap ? "/merge-overlap" : "/merge-disjoint"), count,
        a->view().data().size() + b->view().data().size(), trials, std::max(1u, loops >> 1), [&] {
          auto result = merge();
          return result.data().bit_size + result.size() + std::to_integer<unsigned>(result.data().bytes.back());
        });
    }
  }
}

int main(int argc, char **argv) {
  try {
    if (argc != 5) throw std::invalid_argument("usage: reservoir DIRECTORY RECORDS TRIALS LOOPS");
    std::filesystem::path directory(argv[1]); std::filesystem::create_directories(directory);
    auto count = std::stoull(argv[2]); auto trials = unsigned(std::stoul(argv[3])); auto loops = unsigned(std::stoul(argv[4]));
    require(count >= 16 && trials && loops, "invalid workload extent");
    std::cout << "workload,phase,trial,items,bits,loops,ns_per_item,checksum\n";
    count_case<exponential_golomb<0>>("counts/eg0-small", count * 8, trials, loops,
      [](std::uint64_t i) { return mix(i) & 127; });
    count_case<exponential_golomb<0>>("counts/eg0-wide", count * 8, trials, loops,
      [](std::uint64_t i) { return mix(i) & profile_detail::low_mask(unsigned(i % 65)); });
    count_case<exponential_golomb<3>>("counts/eg3", count * 8, trials, loops,
      [](std::uint64_t i) { return mix(i) & 1023; });
    count_case<golomb<7>>("counts/g7", count * 8, trials, loops,
      [](std::uint64_t i) { return mix(i) & 127; });
    for (bool hash_keys : {false, true}) for (unsigned value_max : {6, 512})
      native_case(directory, count, hash_keys, value_max, trials, loops);
  } catch (std::exception const &error) { std::cerr << error.what() << '\n'; return 1; }
}
