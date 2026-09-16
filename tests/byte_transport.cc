/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks byte string framing, canonical values and typed merge/query semantics.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/redundant_runtime.h>
#include <everett/typed_scan.h>

#include <cassert>
#include <iostream>
#include <map>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using bytes = storage_policy<>;
  template <class F> void rejects(F && action) {
    bool caught = false;
    try { action(); } catch (std::invalid_argument const &) { caught = true; }
    assert(caught);
  }
  bit_string raw(std::string const & value) { return bit_string::copy(sort_codec_detail::string_bits(value)); }
  bool equal(bit_view a, bit_view b) { return compare_bits(a, b) == 0; }
  std::vector<std::string> keys() {
    std::vector<std::string> result{"", "a", "aa", "ab", "b", std::string(1, '\0'),
      std::string("\0\0", 2), std::string("a\0", 2), std::string("a\0b", 3), std::string(1, '\xff')};
    for (unsigned i = 0; i != 65; ++i) {
      std::string key = "shared/prefix/"; key.push_back(char(i)); result.push_back(std::move(key));
    }
    std::sort(result.begin(), result.end(), ordered_string_key::less);
    return result;
  }
  void framing() {
    for (auto const & key : keys()) {
      auto encoded = typed_detail::key<bytes, strings>(key);
      assert(encoded.bit_size == key.size() * 8);
      assert(equal(encoded.view(), sort_codec_detail::string_bits(key)));
      assert((typed_detail::profile_key_transport<bytes>::decode<strings>(encoded.view()) == key));
      typed_detail::dispatch_key<bytes>(encoded.view(), [&]<class S>(std::type_identity<S>, auto const & decoded) {
        static_assert(std::same_as<S, strings>); assert(decoded == key);
      });
      std::optional<std::string> value = key;
      auto payload = typed_detail::value<bytes, strings>(value);
      assert(payload.bit_size == (key.size() + 1) * 8 && payload.bytes[0] == std::byte{1});
      assert(equal(payload.view().subview(8, payload.bit_size - 8), sort_codec_detail::string_bits(key)));
      assert((typed_detail::value<bytes, strings>(payload.view()) == value));
      typed_detail::replacement_compose<bytes> compose;
      assert(!compose.is_tombstone(payload.view()));
      auto absent = typed_detail::value<bytes, strings>(std::nullopt);
      assert(absent.bit_size == 8 && absent.bytes[0] == std::byte{});
      assert(compose.is_tombstone(absent.view()));
      assert((!typed_detail::value<bytes, strings>(absent.view())));
    }
    auto a = raw("a"), aa = raw("aa");
    assert(compare_bits(a.view(), aa.view()) < 0);
    for (std::string malformed : {std::string{}, std::string(1, '\2'), std::string("\0x", 2)}) {
      auto input = raw(malformed);
      rejects([&] { (void)typed_detail::value<bytes, strings>(input.view()); });
      rejects([&] { (void)typed_detail::replacement_compose<bytes>{}.is_tombstone(input.view()); });
    }
    auto input = raw(std::string("\1x", 2));
    rejects([&] { (void)typed_detail::value<bytes, strings>(input.view().prefix(15)); });
    rejects([&] { (void)typed_detail::profile_key_transport<bytes>::decode<strings>(input.view().subview(1, 8)); });
    // The specialization respects a byte-aligned subview rather than assuming
    // that every value starts at its containing allocation's first byte.
    auto padded = raw(std::string("zz\1x\0", 5));
    assert((typed_detail::value<bytes, strings>(padded.view().subview(16, 24)) == std::string("x\0", 2)));
  }
  struct custom_strings : sort_semantics<strings> {
    using encoding = byte_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
  };
  void unchanged_codecs() {
    auto key = std::string("a\0b", 3);
    std::optional<std::string> value = std::string("v\0", 2);
    bit_string expected_key, expected_value;
    sort_bit_writer key_writer(expected_key), value_writer(expected_value);
    write_sort_code<string_registry, strings>(key_writer);
    sort_codec<strings>::key_codec::write_ordered(key_writer, key);
    sort_codec<strings>::value_codec::write(value_writer, value);
    auto actual_key = typed_detail::key<string_policy, strings>(key);
    assert(equal(expected_key.view(), actual_key.view()));
    auto actual_value = typed_detail::value<string_policy, strings>(value);
    assert(equal(expected_value.view(), actual_value.view()));
    using custom_policy = storage_policy<unsorted<std::optional<std::string>>>;
    bit_string custom_key, custom_value;
    sort_bit_writer custom_key_writer(custom_key), custom_value_writer(custom_value);
    custom_strings::key_codec::write_ordered(custom_key_writer, key);
    custom_strings::value_codec::write(custom_value_writer, value);
    if (auto tail = custom_value.bit_size & 7) custom_value_writer.write_bits(0, unsigned(8 - tail));
    auto actual_custom_value = typed_detail::value<custom_policy, custom_strings>(value);
    assert(equal(custom_value.view(), actual_custom_value.view()));
    assert((typed_detail::value<custom_policy, custom_strings>(custom_value.view()) == value));
    using custom_registry = sort_list<custom_strings>;
    using custom_bytes = storage_policy<custom_registry>;
    auto encoded = typed_detail::key<custom_bytes, custom_strings>(key);
    assert(equal(encoded.view().subview(8, encoded.bit_size - 8), custom_key.view()));
  }
  template <class P> void oracle() {
    using engine_type = typed_engine<P, wrapping_fingerprint_algebra, 256, redundant_runtime_family<P>>;
    engine_type engine;
    assert(engine.snapshot().metadata().schema_id == "everett.optional-string/tagless/byte-profile-v2");
    auto all = keys();
    std::map<std::string, std::string, decltype(&ordered_string_key::less)> values(&ordered_string_key::less);
    using snapshot = typename engine_type::world_type;
    std::vector<std::pair<snapshot, decltype(values)>> saved;
    auto verify = [&](snapshot const & world, auto const & expected) {
      for (auto const & key : all) {
        auto it = expected.find(key);
        assert(world.get(key) == (it == expected.end() ? std::nullopt : std::optional(it->second)));
      }
      std::uint64_t hash = 0;
      for (auto const & [key, value] : expected)
        hash += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, value);
      assert(world.signature() == hash && world.live_count() == expected.size());
      typed_scan<strings, snapshot> scan(world);
      auto it = expected.begin();
      while (auto row = scan.next()) {
        assert(it != expected.end() && row->key == it->first && row->value == it->second); ++it;
      }
      assert(it == expected.end());
    };
    auto batch = engine.snapshot().batch();
    for (auto const & key : all) { values[key] = key; batch.put(key, key); }
    engine.contribute(std::move(batch).finish());
    for (unsigned i = 0; i != 180; ++i) {
      auto const & key = all[(i * 37) % all.size()];
      if (i % 3 == 0 && values.contains(key)) {
        engine.contribute(engine.snapshot().erase(key)); values.erase(key);
      } else {
        auto value = std::string(i % 13, char(i));
        engine.contribute(engine.snapshot().put(key, value)); values[key] = value;
      }
      if (i % 29 == 0) { verify(engine.snapshot(), values); saved.emplace_back(engine.snapshot(), values); }
    }
    while (engine.pending()) engine.advance(4096);
    verify(engine.snapshot(), values);
    for (auto const & [world, expected] : saved) verify(world, expected);
    auto restarted = engine_type::from_snapshot(engine.snapshot());
    verify(restarted.snapshot(), values);
    auto last = engine.snapshot();
    auto metadata = last.metadata();
    rejects([&] { (void)snapshot::restore(last.runtime(), metadata, "everett.optional-string/tagless/v1"); });
  }
  void mixed_registry() {
    using registry = sort_list<strings, custom_strings, sort_undefined>;
    using policy = storage_policy<registry>;
    typed_engine<policy> engine("byte-strings-and-custom/v2");
    auto batch = engine.snapshot().batch();
    batch.put<strings>("", "builtin").put<custom_strings>("", "custom");
    auto state = engine.contribute(std::move(batch).finish());
    state = engine.contribute(state.put<strings>(std::string("\0", 1), "zero"));
    state = engine.contribute(state.erase<custom_strings>(""));
    while (engine.pending()) engine.advance(4096);
    state = engine.snapshot();
    assert(state.get<strings>("") == "builtin" && state.get<strings>(std::string("\0", 1)) == "zero");
    assert(!state.get<custom_strings>(""));
    typed_scan<strings, decltype(state)> scan(state);
    assert(scan.next()->key == "" && scan.next()->key == std::string("\0", 1) && !scan.next());
  }
}
int main() try {
  framing(); unchanged_codecs(); oracle<bytes>();
  oracle<storage_policy<strings, 7, exponential_golomb<0>, 16>>();
  mixed_registry();
  std::cout << "byte transport: canonical framing, snapshots, merges, scans and custom codec isolation passed\n";
} catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
