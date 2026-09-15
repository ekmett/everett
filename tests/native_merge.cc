/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/native_merge.h>

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace everett;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "native merge accepted invalid operation");
  }
  std::string bits(bit_view input) {
    std::string result;
    for (std::uint64_t i = 0; i != input.size(); ++i) result += input.at(i) ? '1' : '0';
    return result;
  }
  using table = std::map<std::string, std::string>;
  table dictionary(std::span<profile_record const> records) {
    table result;
    for (auto const & record : records) result.emplace(bits(record.key.view()), bits(record.value.view()));
    return result;
  }
  template <class P> std::vector<profile_record> fixture(std::initializer_list<unsigned> numbers, unsigned tag) {
    std::vector<profile_record> result;
    for (auto number : numbers) {
      std::string key(16, '0');
      for (unsigned i = 0; i != 16; ++i) key[i] = ((number >> (15 - i)) & 1) ? '1' : '0';
      if constexpr (P::unit == profile_unit::bit) key.push_back('1');
      auto width = P::value_width.value_or(3);
      std::string value(width * P::bits_per_unit, '0');
      for (std::size_t i = 0; i != value.size(); ++i) value[i] = ((tag >> (i % 2)) & 1) ? '1' : '0';
      result.push_back({bit_string::from_bits(key), bit_string::from_bits(value)});
    }
    return result;
  }
  template <class P> table decode(profile_array<P> const & array) {
    table result;
    auto cursor = array.view().cursor();
    while (!cursor.done()) {
      auto item = cursor.peek();
      require(result.emplace(bits(item.key.prefix), bits(item.value)).second, "merge emitted duplicate key");
      cursor.advance();
    }
    return result;
  }
  template <class P, class Compose = replace_native_value> profile_array<P> merge(
      profile_array<P> older, profile_array<P> newer, Compose compose = {}) {
    using owner = std::shared_ptr<profile_array<P> const>;
    owner a = std::make_shared<profile_array<P> const>(std::move(older));
    owner b = std::make_shared<profile_array<P> const>(std::move(newer));
    std::weak_ptr<profile_array<P> const> weak_a = a, weak_b = b;
    auto input_count = a->size() + b->size();
    native_merge_builder<P, profile_array<P>, Compose> builder(a, b, std::move(compose));
    a.reset(); b.reset();
    require(!weak_a.expired() && !weak_b.expired(), "merge dropped input pins");
    auto zero = builder.step(0);
    require(zero.keys == 0 && zero.input_records == 0 && builder.progress().keys == 0, "zero merge budget");
    if (!builder.done()) rejects([&] { (void)builder.finish(); });
    auto moved = std::move(builder);
    rejects([&] { builder.step(0); });
    rejects([&] { (void)builder.finish(); });
    while (!moved.done()) {
      auto before = moved.progress();
      auto work = moved.step(1);
      require(work.keys == 1 && work.input_records >= 1 && work.input_records <= 2, "merge key budget");
      require(moved.progress().keys == before.keys + 1 &&
              moved.progress().input_records == before.input_records + work.input_records, "merge accounting");
    }
    require(moved.progress().input_records == input_count, "merge did not consume every record");
    auto output = moved.finish();
    require(output.size() == moved.progress().keys && moved.finished(), "merge output count");
    rejects([&] { moved.step(); });
    rejects([&] { (void)moved.finish(); });
    require(!weak_a.expired() && !weak_b.expired(), "finished merge dropped pins before owner release");
    return output;
  }
  struct concatenate {
    bit_string operator()(bit_view, bit_view older, bit_view newer) const {
      return bit_string::from_bits(bits(older) + bits(newer));
    }
  };
  struct throws {
    bit_view operator()(bit_view, bit_view, bit_view) const { throw std::runtime_error("composition failed"); }
  };
  struct throwing_move {
    throwing_move() = default;
    throwing_move(throwing_move &&) noexcept = default;
    throwing_move & operator=(throwing_move &&) { throw std::runtime_error("policy move failed"); }
    bit_view operator()(bit_view, bit_view, bit_view newer) const { return newer; }
  };
  template <class P> void replacement() {
    auto older = fixture<P>({0,2,4,7}, 1), newer = fixture<P>({1,2,4,8}, 2);
    auto expected = dictionary(older);
    for (auto const & [key, value] : dictionary(newer)) expected[key] = value;
    auto output = merge(profile_array<P>::build(older), profile_array<P>::build(newer));
    require(decode(output) == expected, "replacement merge oracle");
    require(decode(merge(profile_array<P>::build({}), profile_array<P>::build(newer))) == dictionary(newer),
            "empty older merge");
    require(decode(merge(profile_array<P>::build(older), profile_array<P>::build({}))) == dictionary(older),
            "empty newer merge");
    require(merge(profile_array<P>::build({}), profile_array<P>::build({})).size() == 0, "empty merge");
  }
  template <class P> void associative_composition() {
    auto a = fixture<P>({0,2,4,7}, 1), b = fixture<P>({1,2,4,8}, 2), c = fixture<P>({0,3,4,8,9}, 3);
    auto expected = dictionary(a);
    for (auto const & input : {b, c})
      for (auto const & [key, value] : dictionary(input)) expected[key] += value;
    auto left = merge(merge(profile_array<P>::build(a), profile_array<P>::build(b), concatenate{}),
                      profile_array<P>::build(c), concatenate{});
    auto right = merge(profile_array<P>::build(a),
                       merge(profile_array<P>::build(b), profile_array<P>::build(c), concatenate{}), concatenate{});
    require(decode(left) == expected && decode(right) == expected, "chronological associative composition");
    auto reversed = merge(profile_array<P>::build(b), profile_array<P>::build(a), concatenate{});
    auto forward = merge(profile_array<P>::build(a), profile_array<P>::build(b), concatenate{});
    require(decode(reversed) != decode(forward), "composition fixture must be noncommutative");
  }
  void failure_and_pins() {
    using P = storage_policy<profile_unit::byte>;
    auto records = fixture<P>({1}, 1);
    auto source = std::make_shared<profile_array<P> const>(profile_array<P>::build(records));
    std::weak_ptr<profile_array<P> const> weak = source;
    {
      native_merge_builder<P, profile_array<P>, throws> builder(source, source);
      source.reset();
      rejects([&] { builder.step(); });
      require(builder.failed() && !builder.done() && !weak.expired(), "failed merge state and retained pins");
      rejects([&] { builder.step(); });
      rejects([&] { (void)builder.finish(); });
    }
    require(weak.expired(), "destroyed merge retained sources");
    source = std::make_shared<profile_array<P> const>(profile_array<P>::build(records));
    native_merge_builder<P, profile_array<P>, throwing_move> a(source, source), b(source, source);
    rejects([&] { a = std::move(b); });
    require(a.failed(), "failed policy move left merge active");
    rejects([&] { a.step(); });
    rejects([&] { (void)a.finish(); });
    rejects([&] { b.step(); });
    rejects([] { native_merge_builder<P> empty({}, {}); });
  }
}

int main() {
  try {
    replacement<storage_policy<profile_unit::byte>>();
    replacement<storage_policy<profile_unit::byte, fixed_values<3>, 7, exponential_golomb<0>, 16>>();
    replacement<storage_policy<profile_unit::bit, fixed_values<0>, 3, golomb<3>, 7>>();
    replacement<storage_policy<profile_unit::bit, fixed_values<3>, 15, exponential_golomb<3>, 16>>();
    associative_composition<storage_policy<profile_unit::byte>>();
    associative_composition<storage_policy<profile_unit::bit, variable_values, 7, golomb<3>, 16>>();
    failure_and_pins();
    std::cout << "native merge tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks incremental native merges against replacement and noncommuting composition oracles.
 */
