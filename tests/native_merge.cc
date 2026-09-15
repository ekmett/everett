/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/native_merge.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

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
    std::optional<std::string> previous;
    while (!cursor.done()) {
      auto item = cursor.peek();
      auto key = bits(item.key.prefix);
      require(!previous || *previous < key, "merge keys are not strictly increasing");
      previous = key;
      require(result.emplace(key, bits(item.value)).second, "merge emitted duplicate key");
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
  struct concatenate_values {
    bit_string operator()(bit_view older, bit_view newer) const {
      return bit_string::from_bits(bits(older) + bits(newer));
    }
  };
  struct throws_values {
    bit_view operator()(bit_view, bit_view) const { throw std::runtime_error("value composition failed"); }
  };
  struct key_value {
    bit_string operator()(bit_view key, bit_view, bit_view) const { return bit_string::copy(key); }
    bit_string operator()(bit_view, bit_view newer) const { return bit_string::copy(newer); }
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
  // Independent original-bit strings are the comparison oracle. Do not use
  // the production comparison/LCP helpers to compute expected answers.
  std::uint64_t lcp(std::string const & a, std::string const & b) {
    std::uint64_t count = 0;
    while (count < a.size() && count < b.size() && a[count] == b[count]) ++count;
    return count;
  }
  int order(std::string const & a, std::string const & b) { return a < b ? -1 : a > b ? 1 : 0; }
  template <class P> profile_record record(std::string key, unsigned tag, bool wide = false) {
    auto width = P::value_width.value_or(wide ? 2048 : 3) * P::bits_per_unit;
    std::string value(width, '0');
    for (std::size_t i = 0; i != value.size(); ++i) value[i] = (tag >> (i % 3)) & 1 ? '1' : '0';
    return {bit_string::from_bits(key), bit_string::from_bits(value)};
  }
  template <class P> std::vector<profile_record> prefix_records(bool duplicate = false) {
    std::set<std::string> keys;
    auto zero = std::string(P::bits_per_unit, '0'), one = zero;
    one.back() = '1';
    for (auto const & key : {std::string{}, zero, zero + zero, zero + one, one, one + zero, one + one})
      keys.insert(key);
    // Exercise proper prefixes, long common parts, every possible first
    // differing bit within one byte, and a final one-sided drain.
    auto prefix = std::string(129 * P::bits_per_unit, '0');
    keys.insert(prefix);
    for (unsigned n = 0; n != 130; ++n) {
      std::string suffix(16, '0');
      for (unsigned bit = 0; bit != 16; ++bit) suffix[bit] = (n >> (15 - bit)) & 1 ? '1' : '0';
      keys.insert(prefix + suffix);
      if (n % 7 == 0) keys.insert(prefix + suffix + zero);
    }
    auto longer = std::string(4096, '1');
    keys.insert(longer); keys.insert(longer + zero); keys.insert(longer + one);
    std::vector<profile_record> result;
    unsigned tag = 1;
    for (auto const & key : keys) {
      result.push_back(record<P>(key, tag++, true));
      if (duplicate && result.size() % 7 == 1) result.push_back(result.back());
    }
    return result;
  }
  template <class P> void cursor_comparisons() {
    auto records = prefix_records<P>(true);
    bool saw_redundant = false, saw_lp_restart = false;
    for (unsigned mode = 0; mode != 3; ++mode) {
      std::vector<std::uint64_t> ceilings;
      if (mode == 1) {
        for (std::size_t i = 0; i != records.size(); ++i) ceilings.push_back(i % 3);
      }
      auto source = profile_array<P>::build(records, ceilings, mode == 2 ? 3 : 0);
      auto cursor = source.view().cursor();
      for (std::size_t i = 0; i != records.size(); ++i) {
        auto expected = bits(records[i].key.view());
        auto current = cursor.peek();
        require(bits(current.key.prefix) == expected &&
                bits(current.value) == bits(records[i].value.view()), "cursor changed original record");
        auto comparison = cursor.advance_comparison();
        if (i + 1 == records.size()) {
          require(!comparison && cursor.done(), "cursor EOF comparison");
        } else {
          auto next = bits(records[i + 1].key.view());
          auto common = lcp(expected, next);
          require(comparison && comparison->common_bits == common && comparison->order == order(expected, next),
                  "cursor comparison disagrees with original bit strings");
          auto encoded = source.view().encoded_at(i + 1);
          if (encoded.retained < common / P::bits_per_unit) {
            saw_redundant = true;
            if (mode == 2) saw_lp_restart = true;
          }
        }
      }
      rejects([&] { (void)cursor.advance_comparison(); });
    }
    require(saw_redundant, "comparison fixture lacks a redundant retained prefix");
    if constexpr (!P::fixed_width)
      require(saw_lp_restart, "comparison fixture lacks an actual LPFC restart");
    auto empty = profile_array<P>::build({});
    auto cursor = empty.view().cursor();
    require(cursor.done(), "empty comparison cursor");
    rejects([&] { (void)cursor.advance_comparison(); });
  }
  template <class P> void check_output(profile_array<P> const & output, table const & expected) {
    require(decode(output) == expected, "frontier merge table oracle");
    std::string previous;
    std::uint64_t ordinal = 0;
    // The output must be canonical ordinary FC, not merely decode to the right
    // map: a wrong retained prefix could otherwise be hidden by repeated text.
    for (auto const & [key, value] : expected) {
      auto encoded = output.view().encoded_at(ordinal++);
      require(encoded.retained == lcp(previous, key) / P::bits_per_unit, "frontier output is not maximal FC");
      require(encoded.key_units == key.size() / P::bits_per_unit, "frontier output framing");
      require(bits(encoded.value) == value, "frontier output value");
      previous = key;
    }
    profile_native_writer<P> writer;
    for (auto const & [key, value] : expected) {
      auto k = bit_string::from_bits(key), v = bit_string::from_bits(value);
      writer.append(k.view(), v.view());
    }
    auto reference = writer.finish();
    require(std::ranges::equal(output.bytes(), reference.bytes()), "frontier output changed canonical encoded bytes");
    require(output.metadata().extent == reference.metadata().extent &&
            output.metadata().terminal_key_units == reference.metadata().terminal_key_units,
            "frontier output metadata");
    auto groups = output.group_offsets().view();
    auto expected_groups = reference.group_offsets().view();
    require(groups.size() == expected_groups.size(), "frontier output sample count");
    for (std::uint64_t i = 0; i != groups.size(); ++i)
      require(groups.select(i) == expected_groups.select(i), "frontier output offset sample");
  }
  template <class P> void frontier_cases() {
    auto all = prefix_records<P>();
    std::vector<profile_record> older, newer;
    for (std::size_t i = 0; i != all.size(); ++i) {
      if (i % 3 != 1) older.push_back(all[i]);
      if (i % 3 != 0) newer.push_back(record<P>(bits(all[i].key.view()), 7, true));
    }
    auto expected = dictionary(older);
    for (auto const & [key, value] : dictionary(newer)) expected[key] = value;
    for (unsigned mode = 0; mode != 3; ++mode) {
      std::vector<std::uint64_t> a_ceiling, b_ceiling;
      if (mode == 1) {
        a_ceiling.resize(older.size(), 0); b_ceiling.resize(newer.size(), 1);
      }
      auto a = profile_array<P>::build(older, a_ceiling, mode == 2 ? 3 : 0);
      auto b = profile_array<P>::build(newer, b_ceiling, mode == 2 ? 3 : 0);
      check_output(merge(a, b), expected);
      check_output(merge(profile_array<P>::build({}), a), dictionary(older));
      check_output(merge(b, profile_array<P>::build({})), dictionary(newer));
    }
  }
  template <class P> void partial_moves() {
    using source = std::shared_ptr<profile_array<P> const>;
    auto a = fixture<P>({1,3,5,7,9}, 1), b = fixture<P>({0,2,4,5,8,100,101}, 2);
    auto prefix = std::string(129 * P::bits_per_unit, '1');
    for (auto * input : {&a, &b}) for (auto & item : *input)
      item.key = bit_string::from_bits(prefix + bits(item.key.view()));
    auto expected = dictionary(a);
    for (auto const & [key, value] : dictionary(b)) expected[key] = value;
    source old = std::make_shared<profile_array<P> const>(profile_array<P>::build(a));
    source now = std::make_shared<profile_array<P> const>(profile_array<P>::build(b));
    source empty = std::make_shared<profile_array<P> const>(profile_array<P>::build({}));
    native_merge_builder<P> builder(old, now);
    builder.step(1); // Both live frontiers now share the nonempty prefix.
    auto moved = std::move(builder);
    rejects([&] { builder.step(); });
    moved.step(1);
    native_merge_builder<P> assigned(empty, empty);
    assigned = std::move(moved);
    rejects([&] { moved.step(); });
    while (!assigned.done()) assigned.step(3);
    check_output(assigned.finish(), expected);
  }
  template <class P> void malformed_input() {
    auto valid = fixture<P>({2,4,6}, 1);
    std::vector<std::uint64_t> ceilings(valid.size(), 0);
    for (bool duplicate : {false, true}) {
      auto records = valid;
      if (duplicate) records[2].key = records[1].key;
      auto bad = profile_array<P>::build(records, ceilings);
      if (!duplicate) {
        auto suffix = bad.view().encoded_at(2).suffix;
        auto replacement = valid[0].key.view();
        require(suffix.size() == replacement.size(), "descending mutation must preserve framing");
        auto * data = const_cast<std::byte *>(suffix.storage().data());
        // Mutate only this nonconst array's literal bits, preserving all count
        // fields, values, offsets and predecessor lengths.
        for (std::uint64_t i = 0; i != suffix.size(); ++i) {
          auto at = suffix.offset() + i;
          auto mask = 1u << (7 - at % 8);
          auto byte = std::to_integer<unsigned>(data[at / 8]);
          data[at / 8] = std::byte(replacement.at(i) ? byte | mask : byte & ~mask);
        }
      }
      auto cursor = bad.view().cursor();
      auto first = cursor.advance_comparison();
      require(first && first->order < 0, "malformed input valid prefix");
      auto broken = cursor.advance_comparison();
      require(broken && broken->order == (duplicate ? 0 : 1), "cursor missed malformed order");
      auto empty = std::make_shared<profile_array<P> const>(profile_array<P>::build({}));
      for (bool on_left : {false, true}) {
        auto owner = std::make_shared<profile_array<P> const>(bad);
        std::weak_ptr<profile_array<P> const> weak = owner;
        native_merge_builder<P> builder(on_left ? owner : empty, on_left ? empty : owner);
        owner.reset();
        rejects([&] { builder.step(64); });
        require(builder.failed() && !builder.done() && !weak.expired(), "malformed source failed retention");
        rejects([&] { (void)builder.finish(); });
        rejects([&] { builder.step(); });
      }
    }
  }
  template <class P> void late_callback_failure() {
    auto a = fixture<P>({0,2,4}, 1), b = fixture<P>({1,3,4}, 2);
    for (auto * input : {&a, &b}) for (auto & item : *input)
      item.key = bit_string::from_bits(std::string(512, '1') + bits(item.key.view()));
    auto old = std::make_shared<profile_array<P> const>(profile_array<P>::build(a));
    auto now = std::make_shared<profile_array<P> const>(profile_array<P>::build(b));
    std::weak_ptr<profile_array<P> const> old_pin = old, new_pin = now;
    native_merge_builder<P, profile_array<P>, throws> builder(old, now);
    old.reset(); now.reset();
    auto work = builder.step(4);
    require(work.keys == 4 && work.input_records == 4, "late callback prefix progress");
    auto moved = std::move(builder);
    rejects([&] { moved.step(); });
    require(moved.failed() && moved.progress().keys == 4 && !old_pin.expired() && !new_pin.expired(),
            "late callback failure lost frontier state or pins");
    rejects([&] { (void)moved.finish(); });
  }
  template <class P> void frontier_suite() {
    cursor_comparisons<P>(); frontier_cases<P>(); partial_moves<P>();
    malformed_input<P>(); late_callback_failure<P>();
  }
  template <class P> void fragmented_prefixes() {
    static_assert(native_merge_builder<P>::encoded_keys);
    static_assert(native_merge_builder<P, profile_array<P>, concatenate_values>::encoded_keys);
    static_assert(!native_merge_builder<P, profile_array<P>, concatenate>::encoded_keys);
    static_assert(!native_merge_builder<P, profile_array<P>, key_value>::encoded_keys);
    auto unit = std::string(P::bits_per_unit, '0');
    auto higher = unit; higher.back() = '1';
    std::set<std::string> keys{std::string{}};
    std::string prefix;
    // Each proper extension can add another retained literal span. The later
    // branches truncate through those old fragments, often before the current
    // frame's literal. The oracle owns the original complete bit strings.
    for (unsigned depth = 0; depth != 257; ++depth) {
      keys.insert(prefix + higher);
      prefix += unit;
      keys.insert(prefix);
    }
    std::vector<profile_record> older, newer;
    unsigned ordinal = 0;
    for (auto const & key : keys) {
      auto value = [&](unsigned tag) {
        if constexpr (!P::fixed_width)
          if (ordinal % 5 == 0) return bit_string{};
        return record<P>(key, tag).value;
      };
      if (ordinal % 3 != 1) older.push_back({bit_string::from_bits(key), value(1)});
      if (ordinal % 4 != 2) newer.push_back({bit_string::from_bits(key), value(2)});
      ++ordinal;
    }
    auto expected = dictionary(older), concatenated = expected, keyed = expected;
    for (auto const & [key, value] : dictionary(newer)) {
      expected[key] = value;
      concatenated[key] += value;
      if (keyed.contains(key)) keyed[key] = key;
      else keyed[key] = value;
    }
    for (unsigned mode = 0; mode != 3; ++mode) {
      std::vector<std::uint64_t> a_ceiling, b_ceiling;
      if (mode == 1) {
        for (std::size_t i = 0; i != older.size(); ++i) a_ceiling.push_back(i % 11);
        for (std::size_t i = 0; i != newer.size(); ++i) b_ceiling.push_back(i % 7);
      }
      auto a = profile_array<P>::build(older, a_ceiling, mode == 2 ? 3 : 0);
      auto b = profile_array<P>::build(newer, b_ceiling, mode == 2 ? 3 : 0);
      check_output(merge(a, b), expected);
      if constexpr (!P::fixed_width) {
        check_output(merge(a, b, concatenate_values{}), concatenated);
        check_output(merge(a, b, concatenate{}), concatenated);
        check_output(merge(a, b, key_value{}), keyed);
      }
    }
  }
  void value_callback_failure() {
    using P = storage_policy<profile_unit::bit>;
    auto records = prefix_records<P>();
    auto source = std::make_shared<profile_array<P> const>(profile_array<P>::build(records));
    std::weak_ptr<profile_array<P> const> retained = source;
    native_merge_builder<P, profile_array<P>, throws_values> builder(source, source);
    source.reset();
    rejects([&] { builder.step(); });
    require(builder.failed() && !retained.expired(), "value callback failure lost input pins");
    rejects([&] { builder.step(); });
    rejects([&] { (void)builder.finish(); });
  }
  template <class P> void malformed_fragment() {
    auto zero = std::string(P::bits_per_unit, '0'), one = zero;
    one.back() = '1';
    std::vector<profile_record> records;
    for (auto const & key : {zero, zero + zero, zero + zero + zero,
                            zero + zero + one, zero + one + zero})
      records.push_back(record<P>(key, 1));
    for (bool duplicate : {false, true}) {
      auto bad = profile_array<P>::build(records);
      auto prior = bad.view().encoded_at(3), changed = bad.view().encoded_at(4);
      require(changed.retained < prior.retained, "fragment mutation must reach inherited prefix");
      auto replacement = bit_string::from_bits(zero + (duplicate ? one : zero));
      require(changed.suffix.size() == replacement.bit_size, "fragment mutation framing");
      auto * bytes = const_cast<std::byte *>(changed.suffix.storage().data());
      for (std::uint64_t i = 0; i != replacement.bit_size; ++i) {
        auto at = changed.suffix.offset() + i;
        auto mask = 1u << (7 - (at & 7));
        auto old = std::to_integer<unsigned>(bytes[at >> 3]);
        bytes[at >> 3] = std::byte(replacement.view().at(i) ? old | mask : old & ~mask);
      }
      auto source = std::make_shared<profile_array<P> const>(std::move(bad));
      auto empty = std::make_shared<profile_array<P> const>(profile_array<P>::build({}));
      for (bool left : {false, true}) {
        native_merge_builder<P> builder(left ? source : empty, left ? empty : source);
        require(builder.step(3).keys == 3, "fragment valid-prefix progress");
        rejects([&] { builder.step(); });
        require(builder.failed(), "inherited-prefix order failure did not poison merge");
        rejects([&] { (void)builder.finish(); });
      }
    }
  }
  struct output_receipt { std::uint64_t records; };
  template <class P> struct output_state {
    profile_detail::native_output<P> output{P::value_width};
    std::optional<profile_array<P>> sealed;
    unsigned finish_failures = 0;
    bool irreversible = false, bad = false, append_failure = false;
    output_state() = default;
    output_state(output_state const &) = delete;
    output_state(output_state &&) = delete;
  };
  // A movable reference to a nonmovable sink. This deliberately returns a
  // receipt instead of an array; the sink retains the independently checked data.
  template <class P> struct output_reference {
    using policy_type = P;
    output_state<P> * state;
    std::uint64_t size() const noexcept { return state->output.size(); }
    bool finished() const noexcept { return state->output.finished(); }
    bool failed() const noexcept { return state->bad; }
    std::optional<std::uint64_t> common_value_width() const noexcept {
      return state->output.common_value_width();
    }
    void append(std::uint64_t retained, bit_view literal, bit_view value) {
      state->output.append(retained, literal, value);
      if (state->append_failure) {
        state->bad = true;
        throw std::runtime_error("simulated partial sink write");
      }
    }
    output_receipt finish() {
      if (state->finish_failures) {
        --state->finish_failures;
        state->bad = state->irreversible;
        throw std::bad_alloc();
      }
      state->sealed.emplace(state->output.finish());
      return {state->sealed->size()};
    }
  };
  template <class P, class Compose = replace_native_value> void alternate_output(Compose compose = {}) {
    auto old_records = fixture<P>({1, 3, 5}, 1), new_records = fixture<P>({2, 3, 4}, 2);
    auto older = std::make_shared<profile_array<P> const>(profile_array<P>::build(old_records));
    auto newer = std::make_shared<profile_array<P> const>(profile_array<P>::build(new_records));
    auto expected = dictionary(old_records);
    for (auto const & [key, value] : dictionary(new_records)) expected[key] = value;
    using builder_type = native_merge_builder<P, profile_array<P>, Compose, output_reference<P>>;
    output_state<P> sink, unused;
    builder_type builder(output_reference<P>{&sink}, older, newer, compose);
    builder.step(2);
    auto moved = std::move(builder);
    rejects([&] { builder.step(); });
    builder_type assigned(output_reference<P>{&unused}, older, newer, compose);
    assigned = std::move(moved);
    rejects([&] { moved.step(); });
    while (!assigned.done()) assigned.step(1);
    sink.finish_failures = 1;
    rejects([&] { (void)assigned.finish(); });
    require(!assigned.failed() && assigned.done() && !assigned.finished(), "retryable sink failure poisoned merge");
    auto receipt = assigned.finish();
    require(receipt.records == expected.size() && decode(*sink.sealed) == expected,
            "alternate sink receipt or output differs");
    require(assigned.finished() && !unused.output.size(), "moved sink output identity changed");
    rejects([&] { (void)assigned.finish(); });
    for (unsigned state = 0; state != 3; ++state) {
      output_state<P> invalid;
      if (state == 0) invalid.output.append(0, old_records[0].key.view(), old_records[0].value.view());
      if (state == 1) (void)invalid.output.finish();
      if (state == 2) invalid.bad = true;
      rejects([&] { builder_type rejected(output_reference<P>{&invalid}, older, newer, compose); });
    }
    for (bool during_append : {false, true}) {
      output_state<P> broken;
      broken.append_failure = during_append;
      broken.finish_failures = 1; broken.irreversible = true;
      builder_type failed(output_reference<P>{&broken}, older, newer, compose);
      if (during_append) rejects([&] { failed.step(); });
      else {
        while (!failed.done()) failed.step();
        rejects([&] { (void)failed.finish(); });
      }
      require(failed.failed() && !failed.done(), "irreversible sink failure remained active");
      rejects([&] { failed.step(); });
      rejects([&] { (void)failed.finish(); });
    }
  }
  void output_sink_tests() {
    alternate_output<storage_policy<profile_unit::byte>>();
    alternate_output<storage_policy<profile_unit::bit, fixed_values<13>>>();
    alternate_output<storage_policy<profile_unit::byte>>(
      [](bit_view, bit_view newer) { return newer; });
    alternate_output<storage_policy<profile_unit::bit>>(
      [](bit_view, bit_view, bit_view newer) { return newer; });
  }
#if defined(__unix__) || defined(__APPLE__)
  template <class P> struct guarded_native {
    using policy_type = P;
    explicit guarded_native(profile_array<P> source) : sections(std::move(source)) {
      auto page_size = ::sysconf(_SC_PAGESIZE);
      require(page_size > 0, "cannot obtain page size");
      auto page = static_cast<std::size_t>(page_size);
      auto size = sections.bytes().size();
      auto pages = (std::max(size, std::size_t{1}) + page - 1) / page;
      extent = (pages + 1) * page;
      mapping = ::mmap(nullptr, extent, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      require(mapping != MAP_FAILED, "cannot map guarded source");
      auto guard = static_cast<std::byte *>(mapping) + pages * page;
      if (::mprotect(guard, page, PROT_NONE)) {
        ::munmap(mapping, extent); mapping = MAP_FAILED;
        throw std::runtime_error("cannot protect source tail");
      }
      data = guard - size;
      if (size) std::memcpy(data, sections.bytes().data(), size);
    }
    ~guarded_native() { if (mapping != MAP_FAILED) ::munmap(mapping, extent); }
    guarded_native(guarded_native const &) = delete;
    guarded_native(guarded_native &&) = delete;
    profile_view<P> view() const {
      return {std::span<std::byte const>(data, sections.bytes().size()),
              sections.group_offsets().view(), sections.metadata()};
    }
    profile_array<P> sections;
    void * mapping = MAP_FAILED;
    std::byte * data = nullptr;
    std::size_t extent = 0;
  };
  template <class P> void guarded_fragments() {
    std::set<std::string> keys{std::string{}};
    std::string prefix, unit(P::bits_per_unit, '0'), higher = unit;
    higher.back() = '1';
    for (unsigned i = 0; i != 129; ++i) {
      keys.insert(prefix + higher); prefix += unit; keys.insert(prefix);
    }
    std::vector<profile_record> records;
    for (auto const & key : keys) records.push_back(record<P>(key, 1));
    for (bool redundant : {false, true}) {
      std::vector<std::uint64_t> ceilings;
      if (redundant) for (std::size_t i = 0; i != records.size(); ++i) ceilings.push_back(i % 13);
      // Find a physically unaligned start and, for bit policy, a partial final
      // byte. The guard starts immediately after the exact encoded byte extent.
      auto source = profile_array<P>::build(records, ceilings);
      for (unsigned extra = 0; extra != 16 &&
          (((source.bytes().size() & 7) == 0) ||
           (P::unit == profile_unit::bit && (source.metadata().extent & 7) == 0)); ++extra) {
        auto value = bits(records.back().value.view()) + unit;
        records.back().value = bit_string::from_bits(value);
        source = profile_array<P>::build(records, ceilings);
      }
      require((source.bytes().size() & 7) != 0, "guard fixture must start unaligned");
      if constexpr (P::unit == profile_unit::bit)
        require((source.metadata().extent & 7) != 0, "guard fixture must have a partial bit tail");
      auto input = std::make_shared<guarded_native<P> const>(std::move(source));
      require((reinterpret_cast<std::uintptr_t>(input->data) & 7) != 0, "mapped start unexpectedly aligned");
      std::weak_ptr<guarded_native<P> const> weak = input;
      native_merge_builder<P, guarded_native<P>> builder(input, input);
      input.reset(); builder.step(31);
      auto moved = std::move(builder);
      while (!moved.done()) moved.step(7);
      check_output(moved.finish(), dictionary(records));
      require(!weak.expired(), "descriptor source mapping lost after finish");
    }
  }
#endif
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
    output_sink_tests();
#if defined(__unix__) || defined(__APPLE__)
    guarded_fragments<storage_policy<profile_unit::byte>>();
    guarded_fragments<storage_policy<profile_unit::bit, variable_values, 7, golomb<3>, 16>>();
#endif
    frontier_suite<storage_policy<profile_unit::byte>>();
    frontier_suite<storage_policy<profile_unit::byte, fixed_values<3>, 7, exponential_golomb<0>, 16>>();
    frontier_suite<storage_policy<profile_unit::bit, fixed_values<0>, 3, golomb<3>, 7>>();
    frontier_suite<storage_policy<profile_unit::bit, fixed_values<3>, 15, exponential_golomb<3>, 16>>();
    frontier_suite<storage_policy<profile_unit::bit, variable_values, 7, golomb<3>, 16>>();
    fragmented_prefixes<storage_policy<profile_unit::byte>>();
    fragmented_prefixes<storage_policy<profile_unit::bit>>();
    fragmented_prefixes<storage_policy<profile_unit::bit, fixed_values<0>, 3, golomb<3>, 7>>();
    value_callback_failure();
    malformed_fragment<storage_policy<profile_unit::byte, variable_values, 7, exponential_golomb<0>, 4>>();
    malformed_fragment<storage_policy<profile_unit::bit, fixed_values<0>, 3, golomb<3>, 4>>();
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
