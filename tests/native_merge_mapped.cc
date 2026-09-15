/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/mapped_blob.h>
#include <diet/native_merge.h>
#include <diet/sections.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace {
  using namespace diet;
  using table = std::map<std::string, std::string>;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid mapped merge operation accepted");
  }
  std::string original_bits(bit_view value) {
    std::string result;
    for (std::uint64_t i = 0; i < value.size(); ++i) {
      auto at = value.offset() + i;
      auto bit = (std::to_integer<unsigned>(value.storage()[at / 8]) >> (7 - at % 8)) & 1;
      result.push_back(bit ? '1' : '0');
    }
    return result;
  }
  std::string unsigned_bits(unsigned value, unsigned width) {
    std::string result;
    for (unsigned i = width; i; --i) result.push_back(((value >> (i - 1)) & 1) ? '1' : '0');
    return result;
  }
  unsigned unsigned_value(std::string_view value) {
    require(value.size() <= 8, "oracle integer is too wide");
    unsigned result = 0;
    for (auto bit : value) result = 2 * result + unsigned(bit == '1');
    return result;
  }
  std::vector<profile_record> records(table const & source) {
    std::vector<profile_record> result;
    for (auto const & [key, value] : source)
      result.push_back({bit_string::from_bits(key), bit_string::from_bits(value)});
    return result;
  }
  template <class View> table decode(View view) {
    table result;
    auto cursor = view.cursor();
    std::string previous;
    bool first = true;
    while (!cursor.done()) {
      auto item = cursor.peek();
      auto key = original_bits(item.key.prefix), value = original_bits(item.value);
      require(first || previous < key, "merged keys are not strictly sorted");
      require(result.emplace(key, value).second, "merged duplicate key");
      previous = std::move(key); first = false;
      cursor.advance();
    }
    return result;
  }

  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
#if defined(__unix__) || defined(__APPLE__)
      auto pattern = (std::filesystem::temp_directory_path() / "diet-native-merge-XXXXXX").string();
      auto result = ::mkdtemp(pattern.data());
      if (!result) throw std::system_error(errno, std::generic_category(), "create mapped merge fixture");
      path = result;
#else
      for (unsigned i = 0; i < 10000; ++i) {
        auto candidate = std::filesystem::temp_directory_path() / ("diet-native-merge-" + std::to_string(i));
        if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
      }
      throw std::runtime_error("cannot reserve mapped merge fixture directory");
#endif
    }
    ~temporary_directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  object_id id(unsigned number) {
    char text[33];
    std::snprintf(text, sizeof text, "abcdeffedcba012345678901%08x", number);
    return object_id(text);
  }
  template <class P> struct mapped_input {
    std::shared_ptr<mapped_native<P> const> owner;
    std::filesystem::path path;
  };
  template <class P> struct storage {
    temporary_directory directory;
    unsigned next = 1;
    object_id fresh() { return id(next++); }
    object_attempt_id attempt() { return object_attempt_id(fresh().hex()); }
    mapped_input<P> map(profile_array<P> const & array) {
      auto object = fresh();
      auto encoded = encode_native_sections(array);
      auto receipt = encoded.seal(directory.path, object, attempt());
      auto owner = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(receipt.path));
      owner->scan();
      return {std::move(owner), receipt.path};
    }
    mapped_input<P> map(table const & source) {
      auto input = records(source);
      auto array = profile_array<P>::build(input);
      return map(array);
    }
    mapped_input<P> map_malformed(std::vector<std::byte> const & bytes) {
      auto object = fresh();
      auto header = decode_file_header<P>(bytes);
      auto receipt = object_writer<P>::seal(directory.path, object, attempt(), header,
        std::span<std::byte const>(bytes).subspan(96));
      auto owner = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(receipt.path));
      return {std::move(owner), receipt.path};
    }
  };

  struct concatenate {
    bit_string operator()(bit_view, bit_view older, bit_view newer) const {
      return bit_string::from_bits(original_bits(older) + original_bits(newer));
    }
  };
  // Encodes the affine map x -> a*x+b over Z/16 in one byte. Composition is
  // associative, remains fixed-width, and is generally noncommutative.
  struct affine {
    bit_string operator()(bit_view, bit_view older, bit_view newer) const {
      auto old = unsigned_value(original_bits(older));
      auto next = unsigned_value(original_bits(newer));
      auto a = ((next >> 4) * (old >> 4)) & 15;
      auto b = ((next >> 4) * (old & 15) + (next & 15)) & 15;
      return bit_string::from_bits(unsigned_bits(16 * a + b, 8));
    }
  };
  struct throws {
    bit_view operator()(bit_view, bit_view, bit_view) const { throw std::runtime_error("injected composition failure"); }
  };
  struct wrong_width {
    bit_string operator()(bit_view, bit_view, bit_view) const { return {}; }
  };

  template <class P, class Compose = replace_native_value>
  profile_array<P> merge(mapped_input<P> older, mapped_input<P> newer, Compose compose = {}) {
    std::weak_ptr<mapped_native<P> const> weak_old = older.owner, weak_new = newer.owner;
    auto count = older.owner->size() + newer.owner->size();
    auto output = [&] {
      native_merge_builder<P, mapped_native<P>, Compose> builder(older.owner, newer.owner, std::move(compose));
      older.owner.reset(); newer.owner.reset();
      require(!weak_old.expired() && !weak_new.expired(), "builder did not retain mapped inputs");
      auto zero = builder.step(0);
      require(zero.keys == 0 && zero.input_records == 0, "zero merge budget did work");
      if (!builder.done()) {
        rejects([&] { (void)builder.finish(); });
        auto work = builder.step(1);
        require(work.keys == 1 && work.input_records >= 1 && work.input_records <= 2, "first mapped merge step exceeded budget");
      }
      auto moved = std::move(builder);
      rejects([&] { (void)builder.step(0); });
      rejects([&] { (void)builder.finish(); });
      require(!weak_old.expired() && !weak_new.expired(), "moving paused builder lost mapping pins");
      require(std::filesystem::remove(older.path) && std::filesystem::remove(newer.path), "cannot unlink mapped merge inputs");
      unsigned budget = 1;
      while (!moved.done()) {
        auto before = moved.progress();
        auto work = moved.step(budget);
        require(work.keys > 0 && work.keys <= budget && work.input_records >= work.keys && work.input_records <= 2 * work.keys,
                "mapped merge key/input budget mismatch");
        require(moved.progress().keys == before.keys + work.keys &&
                moved.progress().input_records == before.input_records + work.input_records, "mapped merge progress mismatch");
        budget = budget % 3 + 1;
      }
      require(moved.progress().input_records == count, "mapped merge lost input records");
      auto result = moved.finish();
      require(result.size() == moved.progress().keys && moved.finished(), "mapped merge finish count mismatch");
      require(!weak_old.expired() && !weak_new.expired(), "finished builder dropped pins while still alive");
      rejects([&] { (void)moved.step(); });
      rejects([&] { (void)moved.finish(); });
      return result;
    }();
    require(weak_old.expired() && weak_new.expired(), "destroyed builder leaked input mappings");
    return output;
  }

  template <class P> std::array<table, 3> inputs() {
    std::vector<std::string> keys{"", "00000000", "01000001", "0100000100000000", "11111111"};
    // Cross several independent W checkpoints, with a shared prefix much
    // longer than a virtual K group and bit keys ending inside a byte.
    for (unsigned n = 0; n < 45; ++n) {
      std::string key(257 * 8, '0');
      key[0] = '1';
      key += unsigned_bits(n, 16);
      if constexpr (P::unit == profile_unit::bit) key += unsigned_bits(n % 8, 3);
      keys.push_back(std::move(key));
    }
    if constexpr (P::unit == profile_unit::bit) {
      keys.push_back("0"); keys.push_back("01"); keys.push_back("01111");
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::array<table, 3> result;
    for (unsigned layer = 0; layer < 3; ++layer) for (unsigned i = 0; i < keys.size(); ++i) {
      if ((i + layer) % 4 == 1) continue;
      std::string value;
      if constexpr (P::fixed_width) {
        if constexpr (*P::value_width != 0) {
          static_assert(*P::value_width * P::bits_per_unit == 8);
          auto a = (3 * layer + i + 1) & 15, b = (7 * layer + i + 3) & 15;
          value = unsigned_bits(16 * a + b, 8);
        }
      } else {
        auto size = (1 + (i + layer) % 4) * P::bits_per_unit;
        for (unsigned bit = 0; bit < size; ++bit) value.push_back(((i + layer * 3 + bit) % 5) < 2 ? '1' : '0');
      }
      result[layer].emplace(keys[i], std::move(value));
    }
    return result;
  }
  table replacement(std::span<table const> inputs) {
    table result;
    for (auto const & input : inputs) for (auto const & [key, value] : input) result[key] = value;
    return result;
  }
  table concatenation(std::span<table const> inputs) {
    table result;
    for (auto const & input : inputs) for (auto const & [key, value] : input) result[key] += value;
    return result;
  }
  void check_affine(table const & actual, std::span<table const> inputs) {
    auto domain = replacement(inputs);
    require(actual.size() == domain.size(), "affine merge changed key domain");
    for (auto const & [key, unused] : domain) {
      (void)unused;
      auto found = actual.find(key);
      require(found != actual.end() && found->second.size() == 8, "affine merge lost key or fixed width");
      auto value = unsigned_value(found->second);
      for (unsigned x = 0; x < 16; ++x) {
        auto expected = x;
        for (auto const & input : inputs) if (auto next = input.find(key); next != input.end()) {
          auto f = unsigned_value(next->second);
          expected = ((f >> 4) * expected + (f & 15)) % 16;
        }
        require(((value >> 4) * x + (value & 15)) % 16 == expected, "affine output fails independent pointwise composition oracle");
      }
    }
  }

  template <class P> void persisted_query(storage<P> & files, profile_array<P> array, table const & expected) {
    auto pair = std::make_shared<profile_blob<P> const>(profile_blob<P>::adopt_native(std::move(array)));
    auto prepared = query_root<P>::build(pair);
    std::vector<std::shared_ptr<profile_blob<P> const>> chain;
    std::vector<blob_identity> identities;
    for (auto current = prepared.head(); current; current = current->target()) {
      chain.push_back(current); identities.push_back({files.fresh(), files.fresh()});
    }
    for (std::size_t i = 0; i < chain.size(); ++i) {
      auto native = encode_native_sections(chain[i]->native());
      auto index = encode_index_sections(*chain[i], identities[i].native,
        i + 1 < chain.size() ? std::optional<blob_identity>(identities[i + 1]) : std::nullopt);
      (void)native.seal(files.directory.path, identities[i].native, files.attempt());
      (void)index.seal(files.directory.path, identities[i].index, files.attempt());
    }
    auto reopened = open_mapped_query<P>(files.directory.path, identities.front());
    reopened.head()->scan();
    for (auto const & [key, value] : expected) {
      auto query = bit_string::from_bits(key);
      auto cursor = reopened.cursor(query.view());
      std::size_t found = 0;
      while (!cursor.done()) {
        if (cursor.has_match()) {
          auto match = cursor.take_match();
          require(match.source->identity() == identities.back() && original_bits(match.value.view()) == value,
                  "reopened merged query differs from source oracle");
          ++found;
        } else require(cursor.step(1) == 1, "reopened merged query stalled");
      }
      require(found == 1, "reopened merged query missed or duplicated key");
    }
    auto absent = bit_string::from_bits(std::string(3000 * 8, '1'));
    auto cursor = reopened.cursor(absent.view());
    while (!cursor.done()) { require(!cursor.has_match(), "merged query found absent key"); (void)cursor.step(1); }
  }

  template <class P> void run_policy() {
    static_assert(P::group_size != P::codec_block_size);
    storage<P> files;
    auto source = inputs<P>();
    auto first_two = std::span<table const>(source).first(2);
    auto output = merge(files.map(source[0]), files.map(source[1]));
    auto expected = replacement(first_two);
    require(decode(output.view()) == expected, "mapped replacement differs from original-record oracle");
    persisted_query(files, std::move(output), expected);
    auto empty_older = merge(files.map(table{}), files.map(source[0]));
    auto empty_newer = merge(files.map(source[0]), files.map(table{}));
    require(decode(empty_older.view()) == source[0], "empty mapped older input changed output");
    require(decode(empty_newer.view()) == source[0], "empty mapped newer input changed output");
    auto empty = merge(files.map(table{}), files.map(table{}));
    require(empty.size() == 0, "two empty mapped inputs produced records");
    if constexpr (!P::fixed_width) {
      auto ab = merge(files.map(source[0]), files.map(source[1]), concatenate{});
      auto left = merge(files.map(ab), files.map(source[2]), concatenate{});
      auto bc = merge(files.map(source[1]), files.map(source[2]), concatenate{});
      auto right = merge(files.map(source[0]), files.map(bc), concatenate{});
      auto expected_concat = concatenation(source);
      require(decode(left.view()) == expected_concat && decode(right.view()) == expected_concat,
              "mapped concatenation grouping changed chronological composition");
      auto reversed = merge(files.map(source[1]), files.map(source[0]), concatenate{});
      require(decode(reversed.view()) != decode(ab.view()), "concatenation fixture is accidentally commutative");
      persisted_query(files, std::move(left), expected_concat);
    } else if constexpr (*P::value_width != 0) {
      auto ab = merge(files.map(source[0]), files.map(source[1]), affine{});
      auto left = merge(files.map(ab), files.map(source[2]), affine{});
      auto bc = merge(files.map(source[1]), files.map(source[2]), affine{});
      auto right = merge(files.map(source[0]), files.map(bc), affine{});
      auto actual = decode(left.view());
      require(actual == decode(right.view()), "fixed-width mapped composition grouping changed output");
      check_affine(actual, source);
      auto reversed = merge(files.map(source[1]), files.map(source[0]), affine{});
      require(decode(reversed.view()) != decode(ab.view()), "affine fixture is accidentally commutative");
      persisted_query(files, std::move(left), actual);
    }
  }

  std::uint64_t get_le(std::span<std::byte const> bytes, std::size_t at) {
    require(at <= bytes.size() && 8 <= bytes.size() - at, "malformed fixture descriptor bounds");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[at + i])) << (8 * i);
    return value;
  }
  template <class P> void malformed_input_test() {
    storage<P> files;
    auto source = records({{"0110000101100001", ""}, {"0110000101100010", ""}, {"0110000101100011", ""}});
    auto array = profile_array<P>::build(source);
    for (bool first : {false, true}) {
      auto encoded = encode_native_sections(array).materialize();
      auto fc = 96 + get_le(encoded, 96 + section_detail::native_descriptor_offset);
      if constexpr (P::unit == profile_unit::byte) {
        auto at = first ? 1 : array.view().encoded_at(1).next_offset;
        encoded[fc + at] = std::byte{127}; // Backspace exceeds full predecessor length.
      } else {
        auto at = first ? 0 : array.view().encoded_at(1).next_offset;
        for (auto bit = at; bit < array.metadata().extent; ++bit)
          encoded[fc + bit / 8] &= std::byte(~(1u << (7 - bit % 8))); // Unterminated final count.
      }
      auto bad = files.map_malformed(encoded), empty = files.map(table{});
      std::weak_ptr<mapped_native<P> const> weak_bad = bad.owner, weak_empty = empty.owner;
      if (first) {
        rejects([&] { native_merge_builder<P, mapped_native<P>> builder(bad.owner, empty.owner); });
        bad.owner.reset(); empty.owner.reset();
      } else {
        {
          native_merge_builder<P, mapped_native<P>> builder(bad.owner, empty.owner);
          bad.owner.reset(); empty.owner.reset();
          require(builder.step(1).keys == 1, "valid first record did not complete before malformed tail");
          require(std::filesystem::remove(bad.path) && std::filesystem::remove(empty.path), "cannot unlink malformed input fixture");
          rejects([&] { (void)builder.step(1); });
          require(builder.failed() && !builder.done() && !weak_bad.expired() && !weak_empty.expired(),
                  "malformed later record did not poison merge and retain owners");
          rejects([&] { (void)builder.step(0); });
          rejects([&] { (void)builder.finish(); });
          auto moved = std::move(builder);
          require(moved.failed() && !weak_bad.expired(), "moving failed builder lost failure state or owner");
          rejects([&] { (void)moved.finish(); });
        }
      }
      require(weak_bad.expired() && weak_empty.expired(), "failed mapped merge leaked owners");
    }
  }
  template <class Compose> void composition_failure_test(Compose compose) {
    using P = storage_policy<profile_unit::bit, fixed_values<8>, 3, exponential_golomb<0>, 16>;
    storage<P> files;
    table input{{"011", "00110101"}};
    auto a = files.map(input), b = files.map(input);
    std::weak_ptr<mapped_native<P> const> weak_a = a.owner, weak_b = b.owner;
    {
      native_merge_builder<P, mapped_native<P>, Compose> builder(a.owner, b.owner, std::move(compose));
      a.owner.reset(); b.owner.reset();
      require(std::filesystem::remove(a.path) && std::filesystem::remove(b.path), "cannot unlink composition fixture");
      rejects([&] { (void)builder.step(); });
      require(builder.failed() && !weak_a.expired() && !weak_b.expired(), "composition failure lost pinned sources");
      rejects([&] { (void)builder.finish(); });
    }
    require(weak_a.expired() && weak_b.expired(), "destroyed failed composition leaked mappings");
  }
}

int main() {
  try {
#if defined(__APPLE__) || defined(__linux__)
    run_policy<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
    run_policy<storage_policy<profile_unit::bit, variable_values, 7, golomb<3>, 15>>();
    run_policy<storage_policy<profile_unit::byte, fixed_values<1>, 15, exponential_golomb<0>, 16>>();
    run_policy<storage_policy<profile_unit::bit, fixed_values<8>, 31, exponential_golomb<2>, 15>>();
    run_policy<storage_policy<profile_unit::bit, fixed_values<0>, 7, exponential_golomb<0>, 16>>();
    malformed_input_test<storage_policy<profile_unit::byte, fixed_values<0>, 3, exponential_golomb<0>, 16>>();
    malformed_input_test<storage_policy<profile_unit::bit, fixed_values<0>, 7, exponential_golomb<0>, 15>>();
    composition_failure_test(throws{});
    composition_failure_test(wrong_width{});
#endif
    std::cout << "mapped native merge tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks mapped incremental merges, retained inputs and sealed output queries.
 */
