/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks native query encoding across registry paths and custom codecs.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sort_profile.h>

#include <array>
#include <iostream>
#include <string_view>

namespace fixture {
  struct custom_key { using value_type = std::uint64_t; };
  inline unsigned custom_calls = 0;
}

namespace diet {
  // Deliberately supplies only the original owning order protocol.
  template <> struct sort_profile_key<fixture::custom_key> {
    static bit_string order(std::uint64_t value) {
      ++fixture::custom_calls;
      if (value >= 8192) throw std::invalid_argument("custom query key width");
      return {{std::byte(value >> 5), std::byte((value & 31) << 3)}, 13};
    }
  };
}

namespace {
  using namespace diet;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && fn) {
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error("invalid query accepted");
  }
  template <class Key> struct sort {
    using encoding = bit_encoding<>;
    using key_codec = Key;
    using value_codec = no_value;
  };
  using strings = sort<fc_string_key<>>;
  using raw = sort<raw_string_key<>>;
  using bits = sort<fc_bit_key<>>;
  using integers = sort<unsigned_key<32>>;
  using custom = sort<fixture::custom_key>;
  using registry = bin<tip<strings>, bin<tip<raw>, bin<tip<bits>, bin<tip<integers>, tip<custom>>>>>;
  using policy = storage_policy<registry>;
  using selector = registry_selector<registry>;
  static_assert(selector::code_size<strings> == 1);
  static_assert(selector::code_size<raw> == 2);
  static_assert(selector::code_size<bits> == 3);
  static_assert(selector::code_size<integers> == 4);
  static_assert(selector::code_size<custom> == 4);
  static_assert(registry_selector<tip<strings>>::code_size<strings> == 0);
  static_assert(registry_selector<unsorted<std::string>>::code_size<unsorted<std::string>> == 0);
  static_assert(registry_selector<sort_list<unsorted<std::string>>>::code_size<unsorted<std::string>> == 8);

  bool bit(std::span<std::byte const> data, std::uint64_t at) {
    return (std::to_integer<unsigned>(data[at >> 3]) >> (7 - (at & 7))) & 1;
  }
  void expected(bit_string const & actual, std::string_view code,
      std::span<std::byte const> key, std::uint64_t key_bits) {
    actual.validate();
    check(actual.bit_size == code.size() + key_bits, "query extent differs");
    for (std::uint64_t i = 0; i != code.size(); ++i)
      check(bit(actual.bytes, i) == (code[i] == '1'), "query sort bits differ");
    for (std::uint64_t i = 0; i != key_bits; ++i)
      check(bit(actual.bytes, code.size() + i) == bit(key, i), "query key bits differ");
  }
  void expected_string(bit_string const & actual, std::string_view code, std::string const & key) {
    expected(actual, code, std::as_bytes(std::span(key.data(), key.size())), key.size() * 8);
  }

  void string_queries() {
    std::array keys{std::string{}, std::string("a"), std::string("a\0", 2),
      std::string("\0\xff\x80\0", 4), std::string(4096, '\xff')};
    for (auto const & key : keys) {
      auto fc = sort_profile_query<policy, strings>(key);
      auto plain = sort_profile_query<policy, raw>(key);
      expected_string(fc, "0", key);
      expected_string(plain, "10", key);
      auto borrowed = sort_profile_key<fc_string_key<>>::order_view(key);
      auto raw_borrowed = sort_profile_key<raw_string_key<>>::order_view(key);
      check(borrowed.storage().data() == reinterpret_cast<std::byte const *>(key.data()) &&
        raw_borrowed.storage().data() == borrowed.storage().data(), "string order view copied storage");
    }
    std::string key("kept\0value", 10);
    auto fc = sort_profile_query<policy, strings>(key);
    auto saved = key;
    key.assign(300, 'z');
    expected_string(fc, "0", saved);
    using plain_policy = storage_policy<unsorted<std::string>>;
    expected_string(sort_profile_query<plain_policy, unsorted<std::string>>(saved), "", saved);
    using byte_policy = storage_policy<sort_list<unsorted<std::string>>>;
    expected_string(sort_profile_query<byte_policy, unsorted<std::string>>(saved), "00000000", saved);
  }

  void bit_queries() {
    for (std::uint64_t count = 0; count != 138; ++count) {
      bit_string key;
      key.bit_size = count;
      key.bytes.resize(static_cast<std::size_t>((count + 7) / 8));
      for (std::size_t i = 0; i != key.bytes.size(); ++i) key.bytes[i] = std::byte((i * 73 + 0xa6) & 255);
      if (count & 7) key.bytes.back() &= std::byte(0xffu << (8 - (count & 7)));
      auto borrowed = sort_profile_key<fc_bit_key<>>::order_view(key);
      check(borrowed.storage().data() == key.bytes.data() && borrowed.size() == count,
        "bit order view copied storage");
      auto query = sort_profile_query<policy, bits>(key);
      expected(query, "110", key.bytes, count);
      auto saved = key;
      key.bytes.clear();
      expected(query, "110", saved.bytes, count);
    }
    bit_string bad{{std::byte{0xff}}, 1};
    rejects([&] { (void)sort_profile_query<policy, bits>(bad); });
    bad = {{std::byte{0}}, 9};
    rejects([&] { (void)sort_profile_query<policy, bits>(bad); });
  }

  void owning_fallback() {
    for (auto value : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xffffffff}}) {
      std::array data{std::byte(value >> 24), std::byte(value >> 16), std::byte(value >> 8), std::byte(value)};
      expected(sort_profile_query<policy, integers>(value), "1110", data, 32);
    }
    rejects([] { (void)sort_profile_query<policy, integers>(std::uint64_t{1} << 32); });
    for (std::uint64_t value : {0, 17, 8191}) {
      auto before = fixture::custom_calls;
      std::array data{std::byte(value >> 5), std::byte((value & 31) << 3)};
      expected(sort_profile_query<policy, custom>(value), "1111", data, 13);
      check(fixture::custom_calls == before + 1, "owning custom codec was not called once");
    }
  }

  // No width hint, and a different code on every call: query construction must
  // never cache selector output or assume the declarative registry's path.
  struct changing_selector {
    inline static unsigned calls = 0;
    inline static std::string code;
    template <class S, class Output> static void write(Output & out) {
      static_assert(std::is_same_v<S, strings>);
      ++calls;
      for (auto c : code) out.write_bits(c == '1', 1);
    }
  };
  struct sized_selector {
    template <class> static constexpr std::uint64_t code_size = 3;
    inline static unsigned calls = 0;
    inline static unsigned code = 0;
    template <class S, class Output> static void write(Output & out) {
      static_assert(std::is_same_v<S, strings>);
      ++calls;
      out.write_bits(code, 3);
    }
  };
  void custom_selectors() {
    std::string key("\xff\0x", 3);
    for (auto code : {std::string{}, std::string("1"), std::string("001001101"), std::string(137, '1')}) {
      changing_selector::code = code;
      auto before = changing_selector::calls;
      expected_string(sort_profile_query<policy, strings, changing_selector>(key), code, key);
      check(changing_selector::calls == before + 1, "custom selector was not called once");
    }
    for (auto code : {0u, 7u, 2u}) {
      sized_selector::code = code;
      auto before = sized_selector::calls;
      std::string expected_code;
      for (unsigned i = 3; i; --i) expected_code += ((code >> (i - 1)) & 1) ? '1' : '0';
      expected_string(sort_profile_query<policy, strings, sized_selector>(key), expected_code, key);
      check(sized_selector::calls == before + 1, "sized selector was not called once");
    }
  }
}

int main() {
  try {
    string_queries();
    bit_queries();
    owning_fallback();
    custom_selectors();
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
