/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Diet's sequential sampling of pinned encoded blob pairs.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/sampling.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace diet;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  template <class F> void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid sampler operation accepted");
  }

  bool bit_at(bit_string const & key, std::uint64_t i) {
    return (std::to_integer<unsigned>(key.bytes[static_cast<std::size_t>(i / 8)]) >> (7 - i % 8)) & 1;
  }

  int key_order(bit_string const & a, bit_string const & b) {
    auto count = std::min(a.bit_size, b.bit_size);
    for (std::uint64_t i = 0; i != count; ++i) {
      auto x = bit_at(a, i), y = bit_at(b, i);
      if (x != y) return x ? 1 : -1;
    }
    return a.bit_size == b.bit_size ? 0 : (a.bit_size < b.bit_size ? -1 : 1);
  }

  void check_key(bit_view actual, bit_string const & expected) {
    require(actual.size() == expected.bit_size, "sample key length differs from oracle");
    for (std::uint64_t i = 0; i != actual.size(); ++i)
      require(actual.at(i) == bit_at(expected, i), "sample key differs from oracle");
  }

  template <class P> void check_code(profile_coded_sample<P> const & code, bit_string const & current,
                                     bit_string const & previous, std::uint64_t ordinal) {
    std::uint64_t prefix = 0;
    while (prefix != std::min(previous.bit_size, current.bit_size) &&
           bit_at(previous, prefix) == bit_at(current, prefix)) ++prefix;
    prefix -= prefix % P::bits_per_unit;
    require(code.backspace == (previous.bit_size - prefix) / P::bits_per_unit &&
            code.target_ordinal == ordinal, "coded sample header differs from independent oracle");
    require(code.suffix.bit_size == current.bit_size - prefix, "coded sample suffix length");
    for (std::uint64_t i = 0; i != code.suffix.bit_size; ++i)
      require(bit_at(code.suffix, i) == bit_at(current, prefix + i), "coded sample suffix contents");
  }

  struct occurrence {
    bit_string key;
    stream_role role;
    std::uint64_t source_ordinal;
  };

  std::vector<occurrence> flatten(std::vector<profile_record> const & native,
                                  std::vector<bit_string> const & borrowed) {
    std::vector<occurrence> result;
    for (std::size_t i = 0; i != native.size(); ++i)
      result.push_back({native[i].key, stream_role::native, i});
    for (std::size_t i = 0; i != borrowed.size(); ++i)
      result.push_back({borrowed[i], stream_role::borrowed, i});
    std::stable_sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      auto order = key_order(a.key, b.key);
      return order ? order < 0 : (a.role == stream_role::native && b.role == stream_role::borrowed);
    });
    return result;
  }

  template <class P> bit_string value_for(unsigned n) {
    auto units = P::value_width ? *P::value_width : n % 9;
    if constexpr (P::unit == profile_unit::byte)
      return bit_string::from_bytes(std::string(static_cast<std::size_t>(units), char(n)));
    else return bit_string::from_bits(std::string(static_cast<std::size_t>(units), n % 2 ? '1' : '0'));
  }

  template <class P> std::vector<bit_string> keys_for(unsigned count, unsigned prefix_length = 12) {
    std::vector<bit_string> result{bit_string{}, bit_string::from_bytes(std::string(1, '\0'))};
    for (unsigned n = 0; n != count; ++n) {
      if constexpr (P::unit == profile_unit::byte) {
        auto number = std::to_string(n);
        result.push_back(bit_string::from_bytes(std::string(prefix_length, 'a')
          + std::string(6 - number.size(), '0') + number));
      } else {
        std::string bits(prefix_length, '1');
        for (unsigned i = 0; i != 16; ++i) bits.push_back((n >> (15 - i)) & 1 ? '1' : '0');
        bits.append(n % 5, '0');
        result.push_back(bit_string::from_bits(bits));
      }
    }
    std::sort(result.begin(), result.end(), [](auto const & a, auto const & b) { return key_order(a, b) < 0; });
    return result;
  }

  template <class P> void check_fixture(std::vector<profile_record> const & native,
                                      std::vector<bit_string> const & borrowed) {
    auto expected = flatten(native, borrowed);
    auto source = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(native, borrowed));
    std::weak_ptr<profile_blob<P> const> weak = source;
    auto native_bytes = source->native().bytes();
    auto borrowed_bytes = source->borrowed().bytes();
    std::vector<std::byte> native_copy(native_bytes.begin(), native_bytes.end());
    std::vector<std::byte> borrowed_copy(borrowed_bytes.begin(), borrowed_bytes.end());
    auto original = source.get();
    auto cursor = std::make_unique<sample_cursor<P>>(source);
    source.reset();
    require(!weak.expired() && cursor->target().get() == original, "sampler must pin the exact source pair");
    require(cursor->counters().decoded_entries == std::uint64_t(!native.empty()) + std::uint64_t(!borrowed.empty()),
            "sampler constructor decodes at most one record per stream");

    std::uint64_t at = 0, native_at = 0, borrowed_at = 0;
    std::uint64_t comparisons = !native.empty() && !borrowed.empty();
    require(cursor->counters().key_comparisons == comparisons, "constructor comparison count");
    profile_sample_encoder<P> encoder;
    profile_sample_decoder<P> decoder;
    bit_string empty_key;
    while (!cursor->done()) {
      require(at < expected.size(), "sampler emitted an extra sample");
      auto const & entry = expected[static_cast<std::size_t>(at)];
      auto sample = cursor->peek();
      check_key(sample.key, entry.key);
      auto code = encoder.encode(sample.key, sample.target_ordinal);
      check_code(code, entry.key, at ? expected[static_cast<std::size_t>(at - P::group_size)].key : empty_key, at);
      check_key(decoder.accept(code), entry.key);
      check_key(encoder.key(), entry.key);
      require(encoder.size() == at / P::group_size + 1 && decoder.size() == encoder.size(),
              "coded stream progress");
      require(sample.target_ordinal == at && sample.source_role == entry.role
              && sample.source_ordinal == entry.source_ordinal, "sample tagged occurrence differs from oracle");
      auto before = cursor->counters();
      for (unsigned stall = 0; stall != 3; ++stall) {
        auto again = cursor->peek();
        require(again.key.storage().data() == sample.key.storage().data()
                && again.target_ordinal == at, "stalled peek changed key storage or position");
        check_key(again.key, entry.key);
      }
      require(cursor->counters().decoded_entries == before.decoded_entries
              && cursor->counters().key_comparisons == before.key_comparisons,
              "stalled peek must not repeat decoding or comparisons");

      // Destroy the moved-from object before using the destination, so member
      // pointers accidentally retained across the move cannot pass this check.
      auto moved = std::make_unique<sample_cursor<P>>(std::move(*cursor));
      cursor.reset();
      cursor = std::move(moved);
      check_key(cursor->peek().key, entry.key);
      require(!weak.expired(), "moving a sampler lost its source pin");

      auto next = std::min<std::uint64_t>(expected.size(), at + P::group_size);
      // A copied cursor has independent traversal state and decoded-key buffers.
      // Advance it without consuming or invalidating the original peek.
      {
        auto copied = *cursor;
        copied.advance();
        require(copied.done() == (next == expected.size()), "copied sampler completion");
        if (!copied.done()) {
          auto item = copied.peek();
          auto const & wanted = expected[static_cast<std::size_t>(next)];
          check_key(item.key, wanted.key);
          require(item.source_role == wanted.role && item.source_ordinal == wanted.source_ordinal &&
                  item.target_ordinal == next, "copied sampler occurrence differs from oracle");
        }
        require(cursor->counters().consumed_entries() == at, "copy advance consumed original sampler");
        check_key(cursor->peek().key, entry.key);
      }
      for (auto i = at; i != next; ++i) {
        if (expected[static_cast<std::size_t>(i)].role == stream_role::native) ++native_at;
        else ++borrowed_at;
        if (native_at != native.size() && borrowed_at != borrowed.size()) ++comparisons;
      }
      cursor->advance();
      auto after = cursor->counters();
      require(after.consumed_entries() - before.consumed_entries() == next - at,
              "advance must consume exactly one bounded group or its final tail");
      require(after.native_entries == native_at && after.borrowed_entries == borrowed_at,
              "source-consumption counts differ from the independent merge oracle");
      require(after.decoded_entries == next + std::uint64_t(native_at != native.size())
              + std::uint64_t(borrowed_at != borrowed.size()), "each source entry must decode exactly once");
      require(after.key_comparisons == comparisons,
              "key-comparison counter differs from the independent occurrence oracle");
      at = next;
    }
    require(at == expected.size() && cursor->counters().decoded_entries == expected.size(),
            "complete sampler must visit and decode every occurrence once");
    require(cursor->counters().key_comparisons <= expected.size(), "too many full-key comparisons");
    rejects([&] { (void)cursor->peek(); });
    rejects([&] { cursor->advance(); });
    require(cursor->target()->native().bytes().data() == native_bytes.data()
            && cursor->target()->borrowed().bytes().data() == borrowed_bytes.data(),
            "sampling replaced a source allocation");
    require(std::equal(native_copy.begin(), native_copy.end(), native_bytes.begin())
            && std::equal(borrowed_copy.begin(), borrowed_copy.end(), borrowed_bytes.begin()),
            "sampling modified source bytes");
    cursor.reset();
    require(weak.expired(), "sampler source pin leaked after destruction");
  }

  template <class P> void check_coded_codec() {
    profile_sample_encoder<P> encoder;
    profile_sample_decoder<P> decoder;
    require(encoder.size() == 0 && decoder.size() == 0 && encoder.key().empty() && decoder.key().empty(),
            "empty coded stream context");
    auto keys = keys_for<P>(70, 1025);
    std::uint64_t ordinal = 0, full_bits = 0, sent_bits = 0;
    bit_string previous;
    for (auto const & key : keys) for (unsigned duplicate = 0; duplicate != 2; ++duplicate) {
      auto encoder_address = encoder.key().storage().data();
      auto decoder_address = decoder.key().storage().data();
      auto code = encoder.encode(key.view(), ordinal);
      check_code(code, key, previous, ordinal);
      check_key(decoder.accept(code), key);
      if (duplicate) {
        require(code.backspace == 0 && code.suffix.bit_size == 0, "equal sample must carry no key suffix");
        require(encoder.key().storage().data() == encoder_address && decoder.key().storage().data() == decoder_address,
                "retaining a complete key must preserve context storage");
      }
      full_bits += key.bit_size;
      sent_bits += code.suffix.bit_size;
      previous = key;
      ordinal += P::group_size;
      // A pause and a move must preserve both independent link contexts.
      if (ordinal % (3 * P::group_size) == 0) {
        auto resumed_encoder = std::move(encoder);
        auto resumed_decoder = std::move(decoder);
        encoder = std::move(resumed_encoder);
        decoder = std::move(resumed_decoder);
      }
    }
    require(sent_bits < full_bits / 10, "long-prefix handoff still sends full keys");

    auto reject_decode = [&](profile_coded_sample<P> const & bad) {
      auto before = bit_string::copy(decoder.key());
      auto count = decoder.size();
      rejects([&] { (void)decoder.accept(bad); });
      check_key(decoder.key(), before);
      require(decoder.size() == count, "failed coded input changed decoder ordinal");
    };
    reject_decode({std::numeric_limits<std::uint64_t>::max(), {}, ordinal});
    reject_decode({0, {}, ordinal + P::group_size});
    reject_decode({previous.bit_size / P::bits_per_unit, {}, ordinal});
    bit_string malformed;
    malformed.bit_size = 1;
    reject_decode({0, malformed, ordinal});
    malformed = bit_string::from_bits("1");
    malformed.bytes[0] |= std::byte{1};
    reject_decode({0, malformed, ordinal});
    if constexpr (P::unit == profile_unit::byte)
      reject_decode({0, bit_string::from_bits("1"), ordinal});

    auto reject_encode = [&](bit_view bad, std::uint64_t at) {
      auto before = bit_string::copy(encoder.key());
      auto count = encoder.size();
      rejects([&] { (void)encoder.encode(bad, at); });
      check_key(encoder.key(), before);
      require(encoder.size() == count, "failed sample changed encoder ordinal");
    };
    reject_encode({}, ordinal);
    reject_encode(previous.view(), ordinal + P::group_size);
    if constexpr (P::unit == profile_unit::byte) {
      auto unaligned = bit_string::from_bits("1");
      reject_encode(unaligned.view(), ordinal);
    }
    // A failed event does not consume the expected ordinal.
    auto continued = encoder.encode(encoder.key(), ordinal);
    require(continued.backspace == 0 && continued.suffix.bit_size == 0, "aliased equal key coding");
    check_key(decoder.accept(continued), previous);

    // A sorted aliased subview may replace the key with a shorter one. Copying
    // the suffix before resizing avoids retaining a dangling source view.
    profile_sample_encoder<P> alias;
    profile_sample_decoder<P> alias_decoder;
    auto ab = bit_string::from_bytes("ab");
    auto b = bit_string::from_bytes("b");
    auto first = alias.encode(ab.view(), 0);
    require(first.backspace == 0, "first coded event must be literal");
    check_key(alias_decoder.accept(first), ab);
    auto second = alias.encode(alias.key().subview(8, 8), P::group_size);
    check_key(alias.key(), b);
    check_key(alias_decoder.accept(second), b);
    // Decoders also accept redundant backspacing and re-emission, provided
    // the reconstructed sorted key is valid.
    profile_coded_sample<P> redundant{b.bit_size / P::bits_per_unit, b, 2 * P::group_size};
    check_key(alias_decoder.accept(redundant), b);

    profile_sample_decoder<P> fresh;
    rejects([&] { (void)fresh.accept({1, ab, 0}); });
    rejects([&] { (void)fresh.accept({0, ab, P::group_size}); });
    require(fresh.size() == 0 && fresh.key().empty(), "failed first input published context");
    check_key(fresh.accept(first), ab);
    profile_sample_encoder<P> fresh_encoder;
    rejects([&] { (void)fresh_encoder.encode(ab.view(), P::group_size); });
    require(fresh_encoder.size() == 0 && fresh_encoder.key().empty(), "failed first output published context");
    rejects([] { sampling_detail::check_ordinal<P>(
      std::numeric_limits<std::uint64_t>::max() / P::group_size + 1, 0); });
  }

  template <class P> void check_policy() {
    check_coded_codec<P>();
    rejects([] { sample_cursor<P> cursor(nullptr); });
    check_fixture<P>({}, {});
    auto keys = keys_for<P>(unsigned(P::group_size * 2 + 11));

    // Every possible short tail, including empty native and borrowed streams.
    for (std::size_t length = 1; length <= P::group_size * 2; ++length) {
      std::vector<profile_record> native;
      std::vector<bit_string> borrowed;
      for (std::size_t i = 0; i != length; ++i) {
        native.push_back({keys[i], value_for<P>(unsigned(i))});
        borrowed.push_back(keys[i]);
      }
      check_fixture<P>(native, {});
      check_fixture<P>({}, borrowed);
    }

    // Runs of equal borrowed keys cross group cuts, including native ties.
    std::vector<profile_record> native;
    std::vector<bit_string> borrowed;
    for (std::size_t i = 0; i != keys.size(); ++i) {
      if (i % 3 != 1) native.push_back({keys[i], value_for<P>(unsigned(i))});
      for (std::size_t copy = 0; copy != (i % 5 == 0 ? P::group_size + 2 : i % 3); ++copy)
        borrowed.push_back(keys[i]);
    }
    check_fixture<P>(native, borrowed);

    std::mt19937_64 random(0xd1479a45 + P::group_size);
    for (unsigned trial = 0; trial != 8; ++trial) {
      native.clear(); borrowed.clear();
      for (std::size_t i = 0; i != keys.size(); ++i) {
        if (random() % 3) native.push_back({keys[i], value_for<P>(unsigned(i))});
        auto copies = random() % 4;
        for (std::uint64_t copy = 0; copy != copies; ++copy) borrowed.push_back(keys[i]);
      }
      check_fixture<P>(native, borrowed);
    }

    auto long_keys = keys_for<P>(70, 1025);
    native.clear(); borrowed.clear();
    for (std::size_t i = 0; i != long_keys.size(); ++i) {
      if (i % 2 == 0) native.push_back({long_keys[i], value_for<P>(unsigned(i))});
      if (i % 3 != 0) borrowed.push_back(long_keys[i]);
    }
    check_fixture<P>(native, borrowed);
  }

  template <class P> void prefix_and_tie_cases() {
    // Proper prefixes, empty keys, all within-byte mismatch positions and
    // repeated borrowed equals crossing several independently sized cuts.
    std::vector<bit_string> keys;
    auto unit = std::string(P::bits_per_unit, '0');
    auto one = unit; one.back() = '1';
    for (auto const & bits : {std::string{}, unit, unit + unit, unit + one,
                              one, one + unit, one + one})
      keys.push_back(bit_string::from_bits(bits));
    for (auto prefix : {std::string{}, std::string(4096, '1')}) {
      for (unsigned n = 0; n != 130; ++n) {
        std::string tail(8, '0');
        for (unsigned bit = 0; bit != 8; ++bit) tail[bit] = (n >> (7 - bit)) & 1 ? '1' : '0';
        keys.push_back(bit_string::from_bits(prefix + tail));
        if (n % 17 == 0) keys.push_back(bit_string::from_bits(prefix + tail + unit));
      }
    }
    std::sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) { return key_order(a, b) < 0; });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<profile_record> native;
    std::vector<bit_string> borrowed;
    for (std::size_t i = 0; i != keys.size(); ++i) {
      if (i % 3 != 1) native.push_back({keys[i], value_for<P>(unsigned(i))});
      auto copies = i % 11 == 0 ? 2 * P::group_size + 1 : i % 3;
      for (std::uint64_t copy = 0; copy != copies; ++copy) borrowed.push_back(keys[i]);
    }
    check_fixture<P>(native, borrowed);
    // Reverse which stream supplies disjoint keys and the one-sided suffix.
    native.clear(); borrowed.clear();
    for (std::size_t i = 0; i != keys.size(); ++i) {
      if (i % 3 == 1) native.push_back({keys[i], value_for<P>(unsigned(i))});
      else borrowed.push_back(keys[i]);
    }
    check_fixture<P>(native, borrowed);
  }

  template <std::uint64_t K> void check_groups() {
    check_policy<storage_policy<profile_unit::byte, variable_values, K>>();
    check_policy<storage_policy<profile_unit::byte, fixed_values<3>, K>>();
    check_policy<storage_policy<profile_unit::bit, variable_values, K>>();
    check_policy<storage_policy<profile_unit::bit, fixed_values<3>, K>>();
  }
}

int main() {
  try {
    check_groups<3>();
    check_groups<7>();
    check_groups<15>();
    check_groups<31>();
    prefix_and_tie_cases<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
    prefix_and_tie_cases<storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 15>>();
    prefix_and_tie_cases<storage_policy<profile_unit::bit, variable_values, 15, golomb<3>, 7>>();
    prefix_and_tie_cases<storage_policy<profile_unit::bit, fixed_values<3>, 31, exponential_golomb<3>, 16>>();
    std::cout << "sampling tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
