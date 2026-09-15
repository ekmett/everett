/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks mixed bit sort grammars, prefix inheritance and malformed input.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/sort_codec.h>

#include <array>
#include <cassert>
#include <random>
#include <vector>

namespace {
  using namespace diet;
  struct names {
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
    using encoding = bit_encoding<>;
  };
  struct golomb_names {
    using key_codec = fc_string_key<golomb<7>>;
    using value_codec = string_value<exponential_golomb<2>>;
    using encoding = bit_encoding<>;
  };
  struct integers {
    using key_codec = unsigned_key<32>;
    using value_codec = niche_value<unsigned_value<16>, 65535u>;
    using encoding = bit_encoding<fixed_values<16>>;
  };
  struct raw_names {
    using key_codec = raw_string_key<golomb<3>>;
    using value_codec = unsigned_value<64>;
    using encoding = bit_encoding<fixed_values<64>>;
  };
  struct members {
    using key_codec = unsigned_key<3>;
    using value_codec = no_value;
    using encoding = bit_encoding<fixed_values<0>>;
  };
  using registry = bin<bin<bin<bin<tip<names>, tip<golomb_names>>, tip<integers>>, tip<raw_names>>, tip<members>>;
  using writer = sort_record_writer<registry>;
  using reader = sort_record_reader<registry>;
  static_assert(!tombstone_value<unsigned_value<16>>::fixed_value_bits);
  static_assert(tombstone_value<no_value>::fixed_value_bits == 1);
  static_assert(integers::value_codec::fixed_value_bits == 16);
  static_assert(std::is_same_v<sort_codec<unsorted<std::optional<std::string>>>::key_codec, fc_string_key<>>);

  template <class Exception = std::invalid_argument, class F> void rejects(F && f) {
    bool caught = false;
    try { f(); } catch (Exception const &) { caught = true; }
    assert(caught);
  }
  auto ignore = [](auto, auto const &, auto const &, auto const &) {};

  void mixed() {
    writer out;
    std::vector<std::string> keys{"", std::string(1, '\0'), "a", std::string(128, 'a'),
      std::string(127, 'a') + "b", "b", std::string(1, static_cast<char>(255))};
    std::vector<sort_record_control> controls;
    for (auto const & key : keys) controls.push_back(out.append<names>(key, std::optional<std::string>{key + "value"}));
    controls.push_back(out.append<golomb_names>("aardvark", ""));
    controls.push_back(out.append<golomb_names>("aardwolf", "x"));
    controls.push_back(out.append<integers>(0, 7));
    controls.push_back(out.append<integers>(0xffffffffu, std::nullopt));
    controls.push_back(out.append<raw_names>("x", 0xffffffffffffffffull));
    controls.push_back(out.append<members>(1, {}));
    controls.push_back(out.append<members>(7, {}));
    assert(controls.front().sort_bits == 4 && controls.front().sort_retained == 0);
    assert(controls[1].sort_retained == 4 && controls[1].key_start - controls[1].start == 1);
    assert(controls[7].sort_retained == 3);
    assert(controls[9].sort_retained == 2);
    assert(controls[11].sort_retained == 1);
    assert(controls[12].sort_retained == 0);
    for (auto i : {9u, 10u}) assert(controls[i].value_start - controls[i].key_start == 32);
    assert(controls[12].value_start == controls[12].end);
    assert(controls[13].end - controls[13].start == 4); // one tree count bit + three key bits
    reader in(out.data().view());
    std::size_t count = 0;
    while (in.next([&]<class S>(std::type_identity<S>, auto const & key, auto const & value, auto control) {
      auto expected = controls[count];
      assert(control.start == expected.start && control.end == expected.end);
      assert(control.key_start == expected.key_start && control.value_start == expected.value_start);
      if constexpr (std::is_same_v<S, names>) { assert(key == keys[count]); assert(value == key + "value"); }
      else if constexpr (std::is_same_v<S, golomb_names>) {
        assert(key == (count == 7 ? "aardvark" : "aardwolf"));
        assert(value == (count == 7 ? "" : "x"));
      } else if constexpr (std::is_same_v<S, integers>) {
        assert(key == (count == 9 ? 0 : 0xffffffffu));
        assert(value == (count == 9 ? std::optional<std::uint64_t>{7} : std::nullopt));
      } else if constexpr (std::is_same_v<S, raw_names>) { assert(key == "x"); assert(value == 0xffffffffffffffffull); }
      else { assert(key == (count == 12 ? 1 : 7)); static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, std::monostate>); }
      ++count;
    })) {}
    assert(count == controls.size() && in.empty() && !in.failed());
  }

  void prefix_comparison() {
    std::array<std::string, 9> values{"", "a", "aa", "ab", "abc", "b", "bc", "c", "zzzz"};
    for (auto const & previous : values) for (auto const & key : values) {
      bit_string bits;
      sort_bit_writer out(bits);
      fc_string_key<>::write(out, key, &previous);
      sort_bit_reader in(bits.view());
      auto frame = fc_string_key<>::read_frame(in, previous.size() << 3);
      assert(in.empty());
      for (auto const & query : values) {
        auto q = sort_codec_detail::string_bits(query);
        auto expected = compare_common_bits(sort_codec_detail::string_bits(key), q);
        auto actual = frame.compare(q, compare_common_bits(sort_codec_detail::string_bits(previous), q));
        assert(actual.common_bits == expected.common_bits && actual.order == expected.order);
      }
    }
    std::string previous = "a", key = "b";
    bit_string bits;
    sort_bit_writer out(bits);
    fc_string_key<>::write(out, key, &previous);
    sort_bit_reader in(bits.view());
    auto frame = fc_string_key<>::read_frame(in, 8);
    assert(frame.retained_bits == 6 && frame.literal.size() == 2); // 01100001 -> 01100010
  }

  void ordered_keys() {
    std::mt19937_64 random(61723);
    std::vector<std::string> keys{"", std::string(1, '\0'), std::string("a\0b", 3), "a", "ab"};
    for (unsigned i = 0; i != 400; ++i) {
      std::string value;
      for (unsigned j = unsigned(random() % 20); j; --j) value.push_back(static_cast<char>(random()));
      keys.push_back(value);
    }
    std::sort(keys.begin(), keys.end(), ordered_string_key::less);
    std::vector<bit_string> codes;
    for (auto const & key : keys) {
      bit_string code;
      sort_bit_writer out(code);
      // Use a non-byte-aligned start, as a bit tree does.
      out.write_bits(5, 3);
      ordered_string_key::write_ordered(out, key);
      assert((code.bit_size & 7) == 3);
      sort_bit_reader in(code.view().subview(3, code.bit_size - 3));
      assert(ordered_string_key::read_ordered(in) == key && in.empty());
      if (!codes.empty()) assert(compare_bits(codes.back().view(), code.view()) <= 0);
      codes.push_back(std::move(code));
    }
    auto invalid = bit_string::from_bytes(std::string("\0\x01", 2));
    sort_bit_reader in(invalid.view());
    rejects([&] { (void)ordered_string_key::read_ordered(in); });
    assert((sort_code<registry, names>() == bit_string::from_bits("0000")));
    assert((sort_code<registry, integers>() == bit_string::from_bits("001")));
    using bytes = encoded_sort<byte_encoding<>>;
    using list = sort_list<bytes, sort_undefined, unsorted<std::string>>;
    assert((sort_code<list, unsorted<std::string>>() == bit_string::from_bits("00000010")));
  }

  void anchors() {
    writer first;
    first.append<names>("prefix-aaaaaaaaa", "one");
    auto anchor = first.state();
    writer continuation(anchor);
    continuation.append<names>("prefix-aaaaaaaab", "two");
    continuation.append<integers>(123, 45);
    reader in(continuation.data().view(), anchor);
    unsigned count = 0;
    while (in.next([&]<class S>(std::type_identity<S>, auto const & key, auto const & value, auto) {
      if constexpr (std::is_same_v<S, names>) { assert(key == "prefix-aaaaaaaab" && value == "two"); }
      else if constexpr (std::is_same_v<S, integers>) { assert(key == 123 && value == 45); }
      else assert(false);
      ++count;
    })) {}
    assert(count == 2);
    anchor.path = bit_string::from_bits("001");
    rejects([&] { reader bad(continuation.data().view(), anchor); });
    rejects([&] { writer bad(anchor); });

    reader first_in(first.data().view());
    assert(first_in.next(ignore));
    reader resume(continuation.data().view(), first_in.state());
    assert(resume.next(ignore) && resume.next(ignore) && !resume.next(ignore));
  }

  template <class Code> void counts() {
    bit_string data;
    sort_bit_writer out(data);
    constexpr std::array<std::uint64_t, 11> values{0, 1, 2, 3, 7, 8, 15, 16, 31, 127, 512};
    out.write_bits(1, 1);
    for (auto value : values) out.write_count<Code>(value);
    sort_bit_reader in(data.view());
    assert(in.read_bits(1) == 1);
    for (auto value : values) assert(in.read_count<Code>() == value);
    assert(in.empty());
  }

  void malformed() {
    writer original;
    original.append<names>("hello", "value");
    for (std::uint64_t n = 1; n != original.data().bit_size; ++n) {
      reader in(original.data().view().prefix(n));
      rejects([&] { (void)in.next(ignore); });
      assert(in.failed());
      rejects<std::logic_error>([&] { (void)in.next(ignore); });
    }
    using holes = bin<tip<names>, sort_undefined>;
    auto hole = bit_string::from_bits("11"); // initial backspace0, undefined right child
    sort_record_reader<holes> unknown(hole.view());
    rejects([&] { (void)unknown.next(ignore); });
    auto too_far = bit_string::from_bits("010"); // initial backspace1
    reader backspace(too_far.view());
    rejects([&] { (void)backspace.next(ignore); });
    auto half_byte = bit_string::from_bits("10100"); // key backspace0, suffix1, bit0
    sort_bit_reader fragment(half_byte.view());
    rejects([&] { (void)fc_string_key<>::read(fragment); });
    auto beyond_key = bit_string::from_bits("010");
    sort_bit_reader invalid_prefix(beyond_key.view());
    rejects([&] { (void)fc_string_key<>::read(invalid_prefix); });

    writer duplicates;
    duplicates.append<integers>(5, 0);
    rejects([&] { duplicates.append<integers>(5, 1); });
    assert(duplicates.failed());
    rejects<std::logic_error>([&] { duplicates.append<integers>(6, 1); });
    writer descending;
    descending.append<integers>(5, 0);
    rejects([&] { descending.append<names>("a", "b"); });
    writer niche;
    rejects([&] { niche.append<integers>(5, 65535); });
    writer overflow;
    rejects([&] { overflow.append<integers>(std::uint64_t{1} << 32, 0); });

    writer values;
    auto a = values.append<integers>(1, 1);
    auto b = values.append<integers>(2, 2);
    auto corrupted = values.data();
    // A validly framed duplicate must still fail the sorted-key check.
    profile_detail::store_bits(corrupted.bytes.data(), b.key_start, 1, 32);
    reader duplicate_input(corrupted.view());
    assert(duplicate_input.next(ignore));
    assert(duplicate_input.position() == a.end);
    rejects([&] { (void)duplicate_input.next(ignore); });

    auto huge = bit_string::from_bits("1");
    sort_bit_writer huge_out(huge);
    huge_out.write_count(~std::uint64_t{});
    sort_bit_reader huge_in(huge.view().subview(1, huge.bit_size - 1));
    rejects([&] { (void)string_value<>::read(huge_in); });

    // Random malformed frames must fail within their bounded input, without
    // trusting a count enough to allocate a purported enormous string.
    std::mt19937_64 random(4233);
    for (unsigned trial = 0; trial != 5000; ++trial) {
      auto size = random() % 257;
      bit_string noise;
      sort_bit_writer fill(noise);
      for (std::uint64_t i = 0; i != size; ++i) fill.write_bits(random() & 1, 1);
      reader input(noise.view());
      try { while (input.next(ignore)) {} }
      catch (std::invalid_argument const &) { assert(input.failed()); }
    }
  }

  void tombstones() {
    bit_string data;
    sort_bit_writer out(data);
    using tagged = tombstone_value<unsigned_value<16>>;
    tagged::write(out, std::nullopt);
    tagged::write(out, 0xffff);
    assert(data.bit_size == 18);
    sort_bit_reader in(data.view());
    assert(!tagged::read(in));
    assert(tagged::read(in) == 65535 && in.empty());
    writer records;
    records.append<names>("a", std::nullopt);
    records.append<names>("b", "");
    records.append<integers>(0, std::nullopt);
    records.append<integers>(1, 0);
    reader r(records.data().view());
    unsigned count = 0;
    while (r.next([&]<class S>(std::type_identity<S>, auto const &, auto const & value, auto) {
      if constexpr (std::is_same_v<S, names> || std::is_same_v<S, integers>) assert(value.has_value() == bool(count & 1));
      ++count;
    })) {}
    assert(count == 4);
  }

  void packed_and_borrowed() {
    struct packed {
      using key_codec = fc_bit_key<exponential_golomb<2>>;
      using value_codec = bit_value<golomb<3>>;
      using encoding = bit_encoding<>;
    };
    using tree = bin<tip<packed>, tip<integers>>;
    sort_record_writer<tree> out;
    std::vector<bit_string> keys;
    for (auto bits : {"", "0", "00", "000", "001", "01", "011", "1", "11101100101011101"})
      keys.push_back(bit_string::from_bits(bits));
    for (auto const & key : keys) out.append<packed>(key, key);
    sort_record_reader<tree> in(out.data().view());
    std::size_t at = 0;
    while (in.next([&]<class S>(std::type_identity<S>, auto const & key, auto const & value, auto) {
      if constexpr (std::is_same_v<S, packed>) assert(key == keys[at] && value == key);
      else assert(false);
      ++at;
    })) {}
    assert(at == keys.size());
    sort_record_writer<tree> tails;
    for (std::size_t length = 0; length != 64; ++length) {
      auto key = bit_string::from_bits(std::string(length, '0'));
      tails.append<packed>(key, key);
    }
    sort_record_reader<tree> tail_reader(tails.data().view());
    std::uint64_t length = 0;
    while (tail_reader.next([&]<class S>(std::type_identity<S>, auto const & key, auto const & value, auto) {
      if constexpr (std::is_same_v<S, packed>) assert(key.bit_size == length && value == key);
      else assert(false);
      ++length;
    })) {}
    assert(length == 64);
    std::vector<bit_string> encoded;
    for (auto const & key : keys) {
      bit_string data;
      sort_bit_writer bits(data);
      packed::key_codec::write_ordered(bits, key);
      sort_bit_reader read(data.view());
      assert(packed::key_codec::read_ordered(read) == key && read.empty());
      if (!encoded.empty()) assert(compare_bits(encoded.back().view(), data.view()) < 0);
      encoded.push_back(std::move(data));
    }
    sort_record_writer<registry, golomb<2>, stream_role::borrowed> borrowed;
    borrowed.append<names>("a");
    borrowed.append<names>("b");
    auto integer = borrowed.append<integers>(42);
    assert(integer.end - integer.key_start == 32);
    sort_record_reader<registry, golomb<2>, stream_role::borrowed> read(borrowed.data().view());
    unsigned count = 0;
    while (read.next([&](auto, auto const &, std::monostate, auto) { ++count; })) {}
    assert(count == 3);

    bit_string values;
    sort_bit_writer write_values(values);
    using variable = tombstone_value<string_value<>>;
    variable::write(write_values, "a long value that need not be copied");
    variable::write(write_values, std::nullopt);
    integers::value_codec::write(write_values, std::nullopt);
    bit_value<>::write(write_values, keys.back());
    sort_bit_reader skip_values(values.view());
    variable::skip(skip_values);
    variable::skip(skip_values);
    integers::value_codec::skip(skip_values);
    bit_value<>::skip(skip_values);
    assert(skip_values.empty());
  }
}

int main() {
  mixed();
  prefix_comparison();
  ordered_keys();
  anchors();
  counts<exponential_golomb<0>>();
  counts<exponential_golomb<3>>();
  counts<golomb<1>>();
  counts<golomb<7>>();
  malformed();
  tombstones();
  packed_and_borrowed();
}
