/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/index_builder.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace everett;

  void require(bool condition, char const * reason) {
    if (!condition) throw std::runtime_error(reason);
  }
  template <class F> void rejects(F && operation) {
    bool rejected = false;
    try { operation(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid builder operation accepted");
  }
  template <class T> bool same(std::span<T const> a, std::span<T const> b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }

  std::uint64_t oracle_common(bit_view a, bit_view b) {
    std::uint64_t i = 0;
    while (i < std::min(a.size(), b.size()) && a.at(i) == b.at(i)) ++i;
    return i;
  }
  int oracle_order(bit_view a, bit_view b) {
    auto i = oracle_common(a, b);
    if (i < std::min(a.size(), b.size())) return a.at(i) ? 1 : -1;
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
  }

  template <class P> bit_string value(unsigned seed) {
    auto units = P::value_width.value_or(seed % 7);
    std::string bits(units * P::bits_per_unit, '0');
    for (std::size_t i = 0; i != bits.size(); ++i) bits[i] = ((i + seed) % 3) ? '0' : '1';
    return bit_string::from_bits(bits);
  }
  template <class P> std::vector<bit_string> keys() {
    std::vector<bit_string> result;
    if constexpr (P::unit == profile_unit::byte) {
      result.push_back(bit_string::from_bytes(""));
      result.push_back(bit_string::from_bytes(std::string(1, '\0')));
      for (unsigned i = 0; i != 130; ++i) {
        auto digits = std::to_string(i);
        result.push_back(bit_string::from_bytes("shared/prefix/" + std::string(4 - digits.size(), '0') + digits));
      }
      result.push_back(bit_string::from_bytes(std::string(1, char(255))));
    } else {
      for (unsigned length = 0; length != 8; ++length) {
        for (unsigned i = 0; i != (1u << length); ++i) {
          std::string bits(length, '0');
          for (unsigned j = 0; j != length; ++j) bits[j] = (i >> (length - j - 1)) & 1 ? '1' : '0';
          result.push_back(bit_string::from_bits(bits));
        }
      }
    }
    std::sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      return oracle_order(a.view(), b.view()) < 0;
    });
    return result;
  }

  struct occurrence {
    bit_string key;
    bool borrowed;
  };
  std::vector<occurrence> merge_order(std::span<profile_record const> native,
                                      std::span<bit_string const> borrowed) {
    std::vector<occurrence> result;
    for (auto const & entry : native) result.push_back({entry.key, false});
    for (auto const & key : borrowed) result.push_back({key, true});
    std::stable_sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      auto order = oracle_order(a.key.view(), b.key.view());
      return order ? order < 0 : a.borrowed < b.borrowed;
    });
    return result;
  }
  template <class P> std::vector<bit_string> oracle_samples(std::span<profile_record const> native,
                                                          std::span<bit_string const> borrowed) {
    auto order = merge_order(native, borrowed);
    std::vector<bit_string> result;
    for (std::size_t i = 0; i < order.size(); i += P::group_size) result.push_back(order[i].key);
    return result;
  }

  template <std::uint64_t K> void check_offsets(select_groups<K> const & actual, select_groups<K> const & expected) {
    auto samples_equal = std::equal(actual.samples.begin(), actual.samples.end(), expected.samples.begin(), expected.samples.end(),
      [](auto const & a, auto const & b) { return a.first == b.first && a.sparse == b.sparse; });
    require(actual.low == expected.low && actual.high == expected.high && samples_equal &&
      actual.sparse == expected.sparse && actual.record_count == expected.record_count &&
      actual.universe == expected.universe && actual.low_width == expected.low_width, "exact sparse offset encoding");
  }

  template <class P> void check_result(profile_blob<P> const & actual, profile_blob<P> const & expected,
                                       profile_blob<P> const & source,
                                       std::span<profile_record const> native,
                                       std::span<bit_string const> borrowed,
                                       std::span<bit_string const> queries) {
    require(&actual.native() == &source.native(), "builder shares original native allocation");
    require(same(actual.native().bytes(), source.native().bytes()), "native encoding unchanged");
    require(same(actual.borrowed().bytes(), expected.borrowed().bytes()), "exact batch borrowed encoding");
    auto const & a_meta = actual.borrowed().metadata();
    auto const & b_meta = expected.borrowed().metadata();
    require(a_meta.version == b_meta.version && a_meta.key_unit == b_meta.key_unit && a_meta.value_unit == b_meta.value_unit &&
      a_meta.count_unit == b_meta.count_unit && a_meta.offset_unit == b_meta.offset_unit && a_meta.count_code == b_meta.count_code &&
      a_meta.bit_order == b_meta.bit_order && a_meta.role == b_meta.role && a_meta.policy_fixed_values == b_meta.policy_fixed_values &&
      a_meta.policy_value_width == b_meta.policy_value_width && a_meta.common_value_width == b_meta.common_value_width &&
      a_meta.group_size == b_meta.group_size && a_meta.codec_block_size == b_meta.codec_block_size &&
      a_meta.terminal_key_units == b_meta.terminal_key_units && a_meta.record_count == b_meta.record_count && a_meta.extent == b_meta.extent,
      "exact borrowed metadata");
    require(same(actual.false_borrow_bits(), expected.false_borrow_bits()), "exact false-borrow flags");
    require(same(actual.cut_lcps(), expected.cut_lcps()), "exact batch/incremental cut LCPs");
    check_offsets(actual.native().group_offsets(), source.native().group_offsets());
    check_offsets(actual.borrowed().group_offsets(), expected.borrowed().group_offsets());
    auto const & rank = actual.interleave();
    auto const & reference = expected.interleave();
    require(rank.classes == reference.classes && rank.checkpoints == reference.checkpoints &&
      rank.virtual_count == reference.virtual_count && rank.view().count() == reference.view().count(), "exact rank encoding");
    auto catalog = merge_order(native, borrowed);
    require(actual.cut_lcps().size() == actual.group_count(), "one LCP for every emitted cut");
    bit_view frontier;
    bool has_frontier = false;
    for (std::size_t i = 0; i != catalog.size(); ++i) {
      if (i % P::group_size == 0) {
        auto lcp = has_frontier ? oracle_common(frontier, catalog[i].key.view()) : 0;
        require(actual.cut_lcps()[i / P::group_size] == lcp, "independent cut LCP oracle");
      }
      if (catalog[i].borrowed) { frontier = catalog[i].key.view(); has_frontier = true; }
    }
    for (auto const & query : queries) {
      if (catalog.empty()) break;
      auto found = std::upper_bound(catalog.begin(), catalog.end(), query, [](auto const & q, auto const & item) {
        return oracle_order(q.view(), item.key.view()) < 0;
      });
      auto ordinal = found == catalog.begin() ? 0 : std::uint64_t(found - catalog.begin() - 1);
      auto group = ordinal / P::group_size;
      auto const & boundary = catalog[group * P::group_size].key;
      profile_query_context<P> anchor(query.view());
      if (found != catalog.begin()) anchor = anchor.with_key(boundary.view());
      auto a = actual.search_window(group, anchor);
      auto b = expected.search_window(group, anchor);
      require(bool(a.native) == bool(b.native), "lookup native presence");
      if (a.native) require(a.native->ordinal == b.native->ordinal && a.native->value == b.native->value, "lookup native payload");
      require(bool(a.borrowed_predecessor) == bool(b.borrowed_predecessor), "lookup outgoing presence");
      if (a.borrowed_predecessor) {
        auto const & x = *a.borrowed_predecessor;
        auto const & y = *b.borrowed_predecessor;
        require(x.ordinal == y.ordinal && x.target_ordinal == y.target_ordinal && x.comparison.common_bits() == y.comparison.common_bits() &&
          x.comparison.full_units() == y.comparison.full_units() && x.comparison.order() == y.comparison.order() &&
          x.false_borrow == y.false_borrow,
          "lookup outgoing comparison and exact ordinal");
        auto const & key = borrowed[x.ordinal];
        require(x.comparison.common_bits() == oracle_common(key.view(), query.view()) &&
          x.comparison.order() == oracle_order(key.view(), query.view()) &&
          x.comparison.full_units() == key.bit_size / P::bits_per_unit,
          "independent outgoing comparison oracle");
      }
    }
  }

  template <class P> void exercise(std::span<profile_record const> native,
                                   std::span<profile_record const> target_native,
                                   std::span<bit_string const> target_borrowed,
                                   std::span<bit_string const> queries) {
    auto source = profile_blob<P>::build(native, target_borrowed);
    auto old_index = std::vector<std::byte>(source.borrowed().bytes().begin(), source.borrowed().bytes().end());
    auto target = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(target_native, target_borrowed));
    auto incoming = oracle_samples<P>(target_native, target_borrowed);
    auto expected = source.reindex(incoming);
    auto output = oracle_samples<P>(native, incoming);
    for (bool coded : {false, true}) for (std::uint64_t budget : {std::uint64_t{1}, std::uint64_t{2}, P::group_size - 1,
                                 P::group_size, P::group_size + 1, std::numeric_limits<std::uint64_t>::max()}) {
      index_builder<P> builder(source);
      profile_sample_encoder<P> input_encoder;
      profile_sample_decoder<P> output_decoder;
      std::size_t next_in = 0;
      std::size_t next_out = 0;
      std::uint64_t processed = 0;
      rejects([&] { builder.finish(target); });
      require(builder.step(0) == 0 && builder.step(budget) == 0, "no input is not EOF");
      while (!builder.done()) {
        if (builder.needs_input()) {
          require(builder.step(budget) == 0, "missing lookahead blocks native progress");
          if (next_in != incoming.size()) {
            auto scratch = incoming[next_in];
            if (coded) {
              auto frame = input_encoder.encode(scratch.view(), next_in * P::group_size);
              builder.push(frame);
            } else builder.push(scratch.view(), next_in * P::group_size);
            scratch = {};
            ++next_in;
            if (next_in == incoming.size() && budget % 2 == 0) builder.close_input();
          } else builder.close_input();
        }
        require(builder.step(0) == 0, "zero entry budget");
        auto used = builder.step(budget);
        require(used <= budget, "step respects entry budget");
        processed += used;
        if (builder.has_output()) {
          require(builder.step(budget) == 0, "outgoing backpressure blocks progress");
          rejects([&] { builder.finish(target); });
          if (coded) {
            auto frame = builder.take_coded_output();
            auto previous = next_out ? output[next_out - 1].view() : bit_view{};
            auto retained = oracle_common(previous, output[next_out].view()) / P::bits_per_unit;
            auto suffix = output[next_out].view().subview(retained * P::bits_per_unit,
              output[next_out].bit_size - retained * P::bits_per_unit);
            require(frame.backspace == previous.size() / P::bits_per_unit - retained &&
              oracle_order(frame.suffix.view(), suffix) == 0, "outgoing frame contains exact backspace and suffix");
            auto decoded = output_decoder.accept(frame);
            require(frame.target_ordinal == next_out * P::group_size &&
              oracle_order(decoded, output[next_out].view()) == 0, "coded sample follows augmented tagged ordering");
          } else {
            auto sample = builder.take_output();
            require(sample.target_ordinal == next_out * P::group_size && sample.key == output[next_out],
                    "emitted sample follows augmented tagged ordering");
          }
          ++next_out;
        }
      }
      require(next_in == incoming.size() && next_out == output.size() &&
        processed == native.size() + incoming.size() && builder.size() == processed, "all occurrences processed exactly once");
      if (!incoming.empty()) rejects([&] { builder.finish(); });
      auto result = builder.finish(target);
      require(builder.finished() && result.target() == target, "finished pair retains exact target");
      check_result(result, expected, source, native, incoming, queries);
      require(same(source.borrowed().bytes(), std::span<std::byte const>(old_index)), "old source index remains unchanged");
      rejects([&] { builder.finish(target); });
      rejects([&] { builder.step(1); });
      rejects([&] { builder.take_output(); });
      rejects([&] { builder.take_coded_output(); });
    }
  }

  template <class P> void coded_pipeline(std::span<profile_record const> native,
                                         std::span<profile_record const> target_native,
                                         std::span<bit_string const> target_borrowed,
                                         std::span<bit_string const> queries) {
    auto target = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(target_native, target_borrowed));
    auto source = profile_blob<P>::build(native);
    auto incoming = oracle_samples<P>(target_native, target_borrowed);
    auto middle_samples = oracle_samples<P>(native, incoming);
    auto final_samples = oracle_samples<P>(native, middle_samples);
    auto expected_first = source.reindex(incoming);
    auto expected_second = source.reindex(middle_samples);
    for (std::uint64_t budget : {std::uint64_t{1}, std::uint64_t{2}, P::group_size - 1,
                                 P::group_size, P::group_size + 1, std::numeric_limits<std::uint64_t>::max()}) {
      sample_cursor<P> input(target);
      profile_sample_encoder<P> encoder;
      profile_sample_decoder<P> decoder;
      index_builder<P> first(source);
      index_builder<P> second(source);
      bool first_closed = false;
      bool second_closed = false;
      std::size_t emitted = 0;
      std::size_t iterations = 0;
      while (!first.done() || !second.done()) {
        require(++iterations < 100000, "coded pipeline must make progress");
        if (first.needs_input()) {
          if (input.done()) { first.close_input(); first_closed = true; }
          else {
            auto sample = input.peek();
            first.push(encoder.encode(sample.key, sample.target_ordinal));
            input.advance();
          }
        }
        first.step(budget);
        if (first.has_output() && second.needs_input()) second.push(first.take_coded_output());
        if (first.done() && !second_closed) { second.close_input(); second_closed = true; }
        second.step(budget);
        if (second.has_output()) {
          auto frame = second.take_coded_output();
          require(frame.target_ordinal == emitted * P::group_size &&
            oracle_order(decoder.accept(frame), final_samples[emitted].view()) == 0,
            "two stages exchange coded frames without full-key queue materialization");
          ++emitted;
        }
      }
      require(first_closed && second_closed && emitted == final_samples.size(), "coded pipeline reaches exact EOF");
      auto middle = std::make_shared<profile_blob<P> const>(first.finish(target));
      auto head = second.finish(middle);
      require(head.target() == middle && middle->target() == target, "coded pipeline retains exact target chain");
      check_result(*middle, expected_first, source, native, incoming, queries);
      check_result(head, expected_second, source, native, middle_samples, queries);
    }
  }

  template <class P> void suite() {
    auto all = keys<P>();
    std::vector<profile_record> native;
    std::vector<profile_record> target_native;
    std::vector<bit_string> target_borrowed;
    for (std::size_t i = 0; i != all.size(); ++i) {
      if (i % 4 == 0) native.push_back({all[i], value<P>(unsigned(i))});
      if (i % 3 == 0) target_native.push_back({all[i], value<P>(unsigned(i + 1))});
      if (i % 2 == 0) target_borrowed.push_back(all[i]);
      if (i % 14 == 0) target_borrowed.push_back(all[i]);
    }
    exercise<P>(native, target_native, target_borrowed, all);
    exercise<P>({}, target_native, target_borrowed, all);
    exercise<P>(native, {}, {}, all);
    exercise<P>({}, {}, {}, all);
    std::array<profile_record, 1> one{{{all[all.size() / 2], value<P>(7)}}};
    std::vector<bit_string> repeated(P::group_size * 70, one[0].key);
    exercise<P>(one, {}, repeated, all);
    coded_pipeline<P>(native, target_native, target_borrowed, all);
    coded_pipeline<P>(one, {}, repeated, all);

    auto source = profile_blob<P>::build(one);
    index_builder<P> invalid(source);
    rejects([&] { invalid.push(all[0].view(), 1); });
    require(invalid.needs_input() && invalid.received_samples() == 0, "bad ordinal preserves input state");
    invalid.push(all.back().view(), 0);
    rejects([&] { invalid.push(all.back().view(), P::group_size); });
    while (!invalid.needs_input()) {
      invalid.step(1);
      if (invalid.has_output()) invalid.take_output();
    }
    rejects([&] { invalid.push(all.front().view(), P::group_size); });
    require(invalid.needs_input() && invalid.received_samples() == 1, "unsorted sample preserves input state");
    profile_coded_sample<P> mixed{0, all.back(), P::group_size};
    rejects([&] { invalid.push(mixed); });
    require(invalid.needs_input() && invalid.received_samples() == 1, "coded input cannot enter a full-key stream");
    invalid.close_input();
    rejects([&] { invalid.close_input(); });
    rejects([&] { invalid.push(all.back().view(), P::group_size); });
    if constexpr (P::unit == profile_unit::byte) {
      index_builder<P> alignment(source);
      auto unaligned = bit_string::from_bits("1");
      rejects([&] { alignment.push(unaligned.view(), 0); });
    }

    index_builder<P> coded(source);
    profile_sample_encoder<P> encoder;
    auto frame = encoder.encode(all.back().view(), 0);
    auto bad = frame;
    bad.backspace = 1;
    rejects([&] { coded.push(bad); });
    require(coded.needs_input() && coded.received_samples() == 0, "bad coded input preserves decoder state");
    coded.push(frame);
    while (!coded.needs_input()) {
      coded.step(1);
      if (coded.has_output()) coded.take_coded_output();
    }
    rejects([&] { coded.push(all.back().view(), P::group_size); });
    require(coded.needs_input() && coded.received_samples() == 1, "full keys cannot enter a coded stream");
    auto duplicate = encoder.encode(all.back().view(), P::group_size);
    coded.push(duplicate);
    coded.close_input();
    while (!coded.done()) {
      coded.step(1);
      if (coded.has_output()) coded.take_coded_output();
    }

    // The original wrapper can die, and moving a partly built stage preserves
    // both decoding context and the actual shared native allocation.
    auto ephemeral = std::make_unique<profile_blob<P>>(profile_blob<P>::build(one));
    auto native_address = &ephemeral->native();
    index_builder<P> original(*ephemeral);
    ephemeral.reset();
    original.close_input();
    original.step(1);
    index_builder<P> moved(std::move(original));
    if (moved.has_output()) moved.take_output();
    while (!moved.done()) {
      moved.step(1);
      if (moved.has_output()) moved.take_output();
    }
    auto result = moved.finish();
    require(&result.native() == native_address, "moved builder retains native storage");

    std::weak_ptr<profile_blob<P> const> weak;
    {
      auto target = std::make_shared<profile_blob<P> const>(profile_blob<P>::build({}));
      weak = target;
      index_builder<P> empty(profile_blob<P>::build({}));
      empty.close_input();
      auto retained = empty.finish(target);
      target.reset();
      require(!weak.expired() && retained.target(), "finished pair pins empty target identity too");
    }
    require(weak.expired(), "target released after final pair owner");

    auto old_target = std::make_shared<profile_blob<P> const>(profile_blob<P>::build({}));
    auto new_target = std::make_shared<profile_blob<P> const>(profile_blob<P>::build({}));
    index_builder<P> first(profile_blob<P>::build({}));
    first.close_input();
    auto old_pair = first.finish(old_target);
    index_builder<P> fork(old_pair);
    fork.close_input();
    auto new_pair = fork.finish(new_target);
    require(old_pair.target() == old_target && new_pair.target() == new_target &&
      &old_pair.native() == &new_pair.native(), "fork changes exact target without changing old pair or native storage");
  }
}

int main() {
  try {
    [&]<std::size_t... K>(std::index_sequence<K...>) {
      (suite<storage_policy<profile_unit::byte, variable_values, K>>(), ...);
      (suite<storage_policy<profile_unit::byte, fixed_values<3>, K>>(), ...);
      (suite<storage_policy<profile_unit::bit, variable_values, K>>(), ...);
      (suite<storage_policy<profile_unit::bit, fixed_values<5>, K>>(), ...);
    }(std::index_sequence<3, 7, 15, 31>{});
    std::cout << "Incremental index encoding, backpressure, target pins and lookup oracles passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's incremental fractional-index builder.
 */
