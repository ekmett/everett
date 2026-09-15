/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks registry encoding inference, additive extension and typed dispatch.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/policy.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
  using namespace diet;
  struct a { using encoding = byte_encoding<fixed_values<2>>; };
  struct b { using encoding = byte_encoding<fixed_values<2>>; };
  struct c { using encoding = byte_encoding<fixed_values<3>>; };
  struct varying { using encoding = byte_encoding<>; };
  struct bit_word { using encoding = bit_encoding<fixed_values<16>>; };
  struct bit_triple { using encoding = bit_encoding<fixed_values<3>>; };
  struct zero { using encoding = byte_encoding<fixed_values<0>>; };
  template <std::size_t I> struct numbered { using encoding = byte_encoding<>; };

  using raw_byte = encoded_sort<byte_encoding<>>;
  using raw_bit = encoded_sort<bit_encoding<>>;
  using tree = bin<tip<a>, bin<tip<bit_triple>, sort_undefined>>;
  using extended_tree = bin<tip<a>, bin<tip<bit_triple>, bin<tip<b>, tip<c>>>>;
  using list = sort_list<a, sort_undefined, b>;
  using extended_list = sort_list<a, c, b, varying>;
  using nested = bin<sort_list<a, b>, tip<bit_triple>>;

  static_assert(registry_traits<tip<raw_byte>>::unit == profile_unit::byte);
  static_assert(registry_traits<tip<raw_bit>>::unit == profile_unit::bit);
  static_assert(!registry_traits<tip<raw_byte>>::fixed_width);
  static_assert(registry_traits<tip<a>>::fixed_value_bits == 16);
  static_assert(registry_traits<tip<a>>::value_width == 2);
  static_assert(registry_traits<bin<tip<a>, tip<b>>>::value_width == 16);
  static_assert(registry_traits<bin<tip<a>, tip<bit_word>>>::fixed_value_bits == 16);
  static_assert(!registry_traits<bin<tip<a>, tip<bit_triple>>>::fixed_width);
  static_assert(!registry_traits<sort_list<a, varying>>::fixed_width);
  static_assert(!registry_traits<sort_list<a, c>>::fixed_width);
  static_assert(registry_traits<sort_list<zero>>::value_width == 0);
  static_assert(registry_traits<sort_list<zero>>::fixed_width);
  static_assert(registry_traits<list>::sort_count == 2);
  static_assert(registry_traits<list>::value_width == 2);
  static_assert(registry_traits<bin<tip<a>, tip<sort_undefined>>>::value_width == 16);
  static_assert(registry_traits<bin<sort_none, tip<a>>>::value_width == 16);
  static_assert(registry_traits<sort_none>::unit == profile_unit::byte);
  static_assert(!registry_traits<sort_none>::value_width);
  static_assert(!registry_traits<sort_undefined>::fixed_width);
  static_assert(!registry_traits<tip<sort_none>>::value_width);
  static_assert(registry_traits<sort_list<>>::unit == profile_unit::byte);
  static_assert(!registry_traits<sort_list<>>::value_width);
  static_assert(registry_traits<bin<sort_none, sort_undefined>>::unit == profile_unit::bit);
  static_assert(!registry_traits<bin<sort_none, sort_undefined>>::fixed_width);

  using byte_policy = storage_policy<list, 7, exponential_golomb<0>, 16>;
  using bit_policy = storage_policy<bin<tip<a>, tip<bit_word>>, 3, golomb<5>, 15>;
  static_assert(std::is_same_v<byte_policy::registry_type, list>);
  static_assert(byte_policy::unit == profile_unit::byte && byte_policy::bits_per_unit == 8);
  static_assert(byte_policy::value_width == 2 && byte_policy::fixed_width);
  static_assert(byte_policy::group_size == 7 && byte_policy::codec_block_size == 16);
  static_assert(bit_policy::unit == profile_unit::bit && bit_policy::bits_per_unit == 1);
  static_assert(bit_policy::value_width == 16 && bit_policy::backspace_parameter == 5);
  static_assert(storage_policy<sort_none>::unit == profile_unit::byte);
  static_assert(!storage_policy<sort_none>::fixed_width);
  static_assert(storage_policy<tip<a>>::codec_block_size == 15);

  static_assert(registry_extends_v<tree, extended_tree>);
  static_assert(!registry_extends_v<extended_tree, tree>);
  static_assert(registry_extends_v<list, extended_list>);
  static_assert(!registry_extends_v<extended_list, list>);
  static_assert(registry_extends_v<sort_none, tree>);
  static_assert(registry_extends_v<sort_undefined, tip<a>>);
  static_assert(registry_extends_v<tip<sort_undefined>, tree>);
  static_assert(registry_extends_v<bin<tip<a>, tip<sort_undefined>>, bin<tip<a>, tip<b>>>);
  static_assert(registry_extends_v<sort_list<a>, sort_list<a, c>>);
  static_assert(!registry_traits<sort_list<a, c>>::fixed_width);
  static_assert(!registry_extends_v<sort_list<a, b>, sort_list<b, a>>);
  static_assert(!registry_extends_v<sort_list<a, b>, sort_list<a>>);
  static_assert(!registry_extends_v<tip<a>, bin<tip<a>, tip<b>>>);
  static_assert(!registry_extends_v<tip<a>, tip<b>>);
  static_assert(!registry_extends_v<sort_list<a>, tip<a>>);

  template <std::size_t... I>
  auto full_list(std::index_sequence<I...>) -> sort_list<numbered<I>...>;
  using full = decltype(full_list(std::make_index_sequence<256>{}));
  static_assert(registry_traits<full>::sort_count == 256);

  void check(bool condition) {
    if (!condition) { std::fputs("registry check failed\n", stderr); std::abort(); }
  }
  template <class E, class F> void rejects(F && f) {
    try { f(); }
    catch (E const &) { return; }
    check(false);
  }

  // Independent MSB-first reader. The registry only needs this one method;
  // no profile or encoded-record parser participates in these checks.
  struct reader {
    std::string_view bits;
    std::size_t position = 0;
    std::uint64_t read_bits(unsigned width) {
      if (width > bits.size() - position) throw std::out_of_range("truncated test reader");
      std::uint64_t result = 0;
      for (unsigned i = 0; i != width; ++i) result = (result << 1) | unsigned(bits[position++] == '1');
      return result;
    }
  };
  struct identify {
    template <class S> unsigned operator()(std::type_identity<S>, reader &) const {
      if constexpr (std::is_same_v<S, a>) return 1;
      else if constexpr (std::is_same_v<S, b>) return 2;
      else if constexpr (std::is_same_v<S, c>) return 3;
      else if constexpr (std::is_same_v<S, bit_triple>) return 4;
      else return 5;
    }
  };

  void bit_dispatch() {
    reader single{"101"};
    check(dispatch_sort<tip<bit_triple>>(single, identify{}) == 4 && single.position == 0);
    check(single.read_bits(3) == 5);

    reader left{"010100101"};
    check(dispatch_sort<tree>(left, [](auto tag, reader & source) {
      using S = typename decltype(tag)::type;
      check((std::is_same_v<S, a>));
      check(source.position == 1);
      return source.read_bits(8);
    }) == 0xa5);
    check(left.position == 9);

    reader right{"10110"};
    check(dispatch_sort<tree>(right, identify{}) == 4 && right.position == 2);
    check(right.read_bits(3) == 6);
    reader hole{"110101"};
    rejects<std::invalid_argument>([&] { (void)dispatch_sort<tree>(hole, identify{}); });
    check(hole.position == 2);
    reader extension{"111010"};
    check(dispatch_sort<extended_tree>(extension, identify{}) == 3 && extension.position == 3);
    check(extension.read_bits(3) == 2);

    for (std::string_view truncated : {"", "1"}) {
      reader source{truncated};
      rejects<std::out_of_range>([&] { (void)dispatch_sort<tree>(source, identify{}); });
    }
    reader nested_source{"000000001101"};
    check(dispatch_sort<nested>(nested_source, identify{}) == 2 && nested_source.position == 9);
    check(nested_source.read_bits(3) == 5);
  }

  void byte_dispatch() {
    for (auto code : {0u, 2u}) {
      std::array<char, 11> storage{};
      for (unsigned i = 0; i != 8; ++i) storage[i] = char('0' + ((code >> (7 - i)) & 1));
      storage[8] = '1'; storage[9] = '0'; storage[10] = '1';
      reader before{std::string_view(storage.data(), storage.size())};
      reader after = before;
      check(dispatch_sort<list>(before, identify{}) == dispatch_sort<extended_list>(after, identify{}));
      check(before.position == 8 && after.position == 8);
      check(before.read_bits(3) == 5 && after.read_bits(3) == 5);
    }
    reader replaced{"00000001"};
    check(dispatch_sort<extended_list>(replaced, identify{}) == 3 && replaced.position == 8);
    for (std::string_view unknown : {"00000001", "00000011", "11111111"}) {
      reader source{unknown};
      rejects<std::invalid_argument>([&] { (void)dispatch_sort<list>(source, identify{}); });
      check(source.position == 8);
    }
    for (std::size_t n = 0; n != 8; ++n) {
      reader source{std::string_view("00000000", n)};
      rejects<std::out_of_range>([&] { (void)dispatch_sort<list>(source, identify{}); });
      check(source.position == 0);
    }
    reader maximum{"111111111"};
    check(dispatch_sort<full>(maximum, [](auto tag, reader &) {
      return std::is_same_v<typename decltype(tag)::type, numbered<255>>;
    }));
    check(maximum.position == 8 && maximum.read_bits(1) == 1);
  }

  void visitors_and_empty() {
    bool visited = false;
    reader source{"0"};
    dispatch_sort<tree>(source, [&](auto, reader &) { visited = true; });
    check(visited && source.position == 1);
    int value = 7;
    reader reference_source{"0"};
    auto & reference = dispatch_sort<tree>(reference_source, [&](auto, reader &) -> int & { return value; });
    reference = 9;
    check(value == 9);
    reader throwing{"0"};
    rejects<std::runtime_error>([&] {
      dispatch_sort<tree>(throwing, [](auto, reader &) { throw std::runtime_error("visitor failure"); });
    });
    check(throwing.position == 1);
    auto unreachable = [&](auto, reader &) { check(false); };
    reader none{"101"};
    rejects<std::invalid_argument>([&] { dispatch_sort<sort_none>(none, unreachable); });
    rejects<std::invalid_argument>([&] { dispatch_sort<sort_undefined>(none, unreachable); });
    rejects<std::invalid_argument>([&] { dispatch_sort<tip<sort_undefined>>(none, unreachable); });
    rejects<std::invalid_argument>([&] { dispatch_sort<tip<sort_none>>(none, unreachable); });
    check(none.position == 0);
    reader empty_list{"00000000"};
    rejects<std::invalid_argument>([&] { dispatch_sort<sort_list<>>(empty_list, unreachable); });
    check(empty_list.position == 8);
    struct invalid_reader { std::uint64_t read_bits(unsigned) { return 256; } } invalid;
    rejects<std::invalid_argument>([&] { dispatch_sort<list>(invalid, [](auto, auto &) {}); });
    rejects<std::invalid_argument>([&] { dispatch_sort<tree>(invalid, [](auto, auto &) {}); });
  }
}

int main() {
  bit_dispatch();
  byte_dispatch();
  visitors_and_empty();
}
