/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/cola_local_merge.h>
#include <diet/cola_query.h>
#include <diet/cola_sections.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
  using namespace diet;
  using table = std::map<std::string, std::string>;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class Exception = std::logic_error, class F> void rejects(F && f) {
    bool caught = false;
    try { f(); } catch (Exception const &) { caught = true; }
    check(caught, "expected rejection");
  }
  std::string bits(bit_view view) {
    std::string out;
    for (std::uint64_t i = 0; i != view.size(); ++i) out += view.at(i) ? '1' : '0';
    return out;
  }
  template <class P> std::string key(unsigned number) {
    std::string out(33 * 8, '0');
    for (unsigned i = 0; i != 16; ++i) out += (number >> (15 - i)) & 1 ? '1' : '0';
    if constexpr (P::unit == profile_unit::bit)
      for (unsigned i = 0; i != 3; ++i) out += (number >> i) & 1 ? '1' : '0';
    return out;
  }
  template <class P> std::string value(unsigned number, unsigned tag) {
    auto units = P::value_width.value_or(1 + number % 3);
    std::string out(units * P::bits_per_unit, '0');
    for (std::size_t i = 0; i != out.size(); ++i)
      out[i] = ((number + tag * 5) >> (i % 4)) & 1 ? '1' : '0';
    return out;
  }
  template <class P> table fixture(unsigned stride, unsigned offset, unsigned tag) {
    table out;
    for (unsigned i = 0; i != 4 * P::group_size + 7; ++i) {
      auto n = stride * i + offset;
      out.emplace(key<P>(n), value<P>(n, tag));
    }
    out.emplace("", value<P>(0, tag));
    out.emplace(std::string(P::bits_per_unit, '0'), value<P>(1, tag));
    return out;
  }
  std::vector<profile_record> records(table const & input) {
    std::vector<profile_record> out;
    for (auto const & [k, v] : input)
      out.push_back({bit_string::from_bits(k), bit_string::from_bits(v)});
    return out;
  }
  template <class P> typename cola_index<P>::native_pointer native(table const & input) {
    return std::make_shared<profile_array<P> const>(profile_array<P>::build(records(input)));
  }
  template <class P> table decoded(profile_array<P> const & input) {
    table out;
    auto cursor = input.view().cursor();
    std::string previous;
    bool first = true;
    while (!cursor.done()) {
      auto item = cursor.peek();
      auto k = bits(item.key.prefix);
      check(first || previous < k, "output keys not strictly sorted");
      check(out.emplace(k, bits(item.value)).second, "duplicate output key");
      first = false; previous = std::move(k);
      cursor.advance();
    }
    return out;
  }
  template <class P> std::vector<std::byte> wire(cola_index<P> const & input) {
    object_id native_id("00000000000000000000000000000001");
    std::optional<blob_identity> main;
    std::optional<object_id> secondary;
    if (input.main_target()) main.emplace(object_id("00000000000000000000000000000002"),
                                          object_id("00000000000000000000000000000003"));
    if (input.secondary_target()) secondary.emplace("00000000000000000000000000000004");
    return encode_cola_sections(input, native_id, main, secondary).materialize();
  }
  struct concatenate {
    std::shared_ptr<unsigned> moves;
    explicit concatenate(std::shared_ptr<unsigned> count) : moves(std::move(count)) {}
    concatenate(concatenate && source) : moves(std::move(source.moves)) { if (moves) ++*moves; }
    concatenate & operator=(concatenate &&) = delete;
    bit_string operator()(bit_view older, bit_view newer) const {
      return bit_string::from_bits(bits(older) + bits(newer));
    }
  };
  struct key_concatenate {
    bit_string operator()(bit_view key_bits, bit_view older, bit_view newer) const {
      return bit_string::from_bits(bits(key_bits) + bits(older) + bits(newer));
    }
  };
  struct throwing_compose {
    std::shared_ptr<unsigned> calls;
    bit_view operator()(bit_view, bit_view newer) const {
      if (++*calls == 2) throw std::runtime_error("intentional composition failure");
      return newer;
    }
  };

  template <class P> void query_result(cola_local_merge_result<P> const & result,
      std::map<void const *, table> const & tables) {
    auto root = cola_query_root<P>::build(result.carrier);
    std::set<std::string> queries{key<P>(65535)};
    for (auto const & [owner, input] : tables) {
      (void)owner;
      for (auto const & [k, v] : input) { (void)v; queries.insert(k); }
    }
    for (auto const & query : queries) {
      auto encoded = bit_string::from_bits(query);
      auto cursor = root.cursor(encoded.view());
      std::set<void const *> actual;
      while (!cursor.done()) {
        check(cursor.step(1) <= 1, "query budget exceeded");
        if (!cursor.has_match()) continue;
        auto match = cursor.take_match();
        auto owner = match.secondary ? match.source->secondary_target() : match.source->native_owner();
        auto table_at = tables.find(owner.get());
        check(table_at != tables.end(), "query returned unrelated source");
        auto expected = table_at->second.find(query);
        check(expected != table_at->second.end(), "query returned absent key");
        check(match.ordinal == std::uint64_t(std::distance(table_at->second.begin(), expected)) &&
              bits(match.value.view()) == expected->second, "query original-table oracle");
        check(actual.insert(owner.get()).second, "query duplicated a source match");
      }
      for (auto const & [owner, input] : tables)
        check(actual.contains(owner) == input.contains(query), "query omitted source match");
    }
  }

  template <class P, class Compose = replace_native_value> void scenario(bool secondary_plan,
      Compose compose = {}, bool concatenating = false, std::shared_ptr<unsigned> moves = {}) {
    using node = cola_index<P>;
    using job_type = cola_local_merge_job<P, Compose>;
    static_assert(std::is_nothrow_move_constructible_v<job_type>);
    static_assert(std::is_nothrow_move_assignable_v<job_type>);
    auto a = fixture<P>(2, 0, 1), b = fixture<P>(3, 0, 2);
    auto deeper_table = fixture<P>(5, 1, 3), side_table = fixture<P>(7, 2, 4);
    auto expected = a;
    for (auto const & [k, v] : b) {
      if (concatenating && expected.contains(k)) {
        if constexpr (std::is_same_v<Compose, key_concatenate>) expected[k] = k + expected[k] + v;
        else expected[k] += v;
      }
      else expected[k] = v;
    }
    if (concatenating) {
      auto reverse = b;
      for (auto const & [k, v] : a) reverse[k] += v;
      check(reverse != expected, "composition fixture must be noncommutative");
    }
    auto older = native<P>(a), newer = native<P>(b), side = native<P>(side_table);
    auto deeper = std::make_shared<node const>(node::build(records(deeper_table)));
    std::weak_ptr<profile_array<P> const> old_pin = older, new_pin = newer, side_pin = side;
    std::weak_ptr<node const> deeper_pin = deeper;
    auto old_bytes = std::vector<std::byte>(older->bytes().begin(), older->bytes().end());
    auto new_bytes = std::vector<std::byte>(newer->bytes().begin(), newer->bytes().end());
    auto plan = secondary_plan ? cola_destination_plan<P>::for_secondary(deeper)
      : cola_destination_plan<P>::for_main(deeper, side);
    cola_local_merge_result<P> result;
    std::array<std::uint64_t, 3> charged{};
    {
      job_type job(older, newer, std::move(plan), std::move(compose));
      older.reset(); newer.reset(); deeper.reset(); side.reset();
      check(!old_pin.expired() && !new_pin.expired() && !deeper_pin.expired(), "job lost input pins");
      if (!secondary_plan) check(!side_pin.expired(), "job lost destination leaf pin");
      unsigned stages = 0;
      while (!job.done()) {
        auto phase = job.phase();
        check(phase == cola_merge_phase::native_merge || phase == cola_merge_phase::destination_index ||
              phase == cola_merge_phase::carrier_index, "unexpected executable phase");
        check(job.step(0) == 0 && job.phase() == phase, "zero budget changed stage");
        rejects([&] { (void)job.finish(); });
        if (!job.stage_done()) rejects([&] { job.finish_stage(); });
        check(!job.failed(), "rejected precondition poisoned job");
        auto move_count = moves ? *moves : 0;
        auto moved = std::move(job);
        check(job.phase() == cola_merge_phase::inactive && !job.done() && !job.stage_done(), "moved source state");
        rejects([&] { job.step(0); }); rejects([&] { job.finish_stage(); }); rejects([&] { (void)job.finish(); });
        job = std::move(moved);
        check(moved.phase() == cola_merge_phase::inactive, "move-assignment source state");
        check(!moves || *moves == move_count, "job move touched callback state");
        std::uint64_t total = 0;
        unsigned turn = 0;
        while (!job.stage_done()) {
          auto budget = (++turn % 3) ? 1 : 2 * P::group_size + 3;
          auto work = job.step(budget);
          check(work && work <= budget && job.phase() == phase, "stage work budget or implicit finalization");
          total += work;
        }
        charged[static_cast<unsigned>(phase)] = total;
        check(job.step(1000) == 0 && job.phase() == phase, "completed stage advanced implicitly");
        job.finish_stage();
        check(job.phase() != phase, "stage finalization did not advance");
        ++stages;
      }
      check(stages == (secondary_plan ? 2u : 3u), "secondary plan did not skip destination index");
      check(job.phase() == cola_merge_phase::ready && job.stage_done() && !job.finished(), "ready state");
      check(job.step(1) == 0, "ready job consumed work");
      rejects([&] { job.finish_stage(); }); check(!job.failed(), "ready rejection poisoned job");
      result = job.finish();
      check(job.finished() && !job.done() && !job.stage_done() && job.phase() == cola_merge_phase::taken, "taken state");
      rejects([&] { job.step(); }); rejects([&] { job.finish_stage(); }); rejects([&] { (void)job.finish(); });
      check(!old_pin.expired() && !new_pin.expired(), "finished job released documented source pins");
      check(std::vector<std::byte>(old_pin.lock()->bytes().begin(), old_pin.lock()->bytes().end()) == old_bytes &&
            std::vector<std::byte>(new_pin.lock()->bytes().begin(), new_pin.lock()->bytes().end()) == new_bytes,
            "local job changed source encoding");
    }
    check(old_pin.expired() && new_pin.expired(), "result retained obsolete native input owners");
    check(result.merged_native && decoded(*result.merged_native) == expected, "merged original-table oracle");
    check(result.carrier && result.carrier->native().size() == 0, "carrier must be native-empty");
    check(result.carrier->main_target() == result.main && result.carrier->secondary_target() == result.secondary,
          "carrier targets not exact result handles");
    check(charged[0] == expected.size() && charged[2] == result.carrier->virtual_size(), "stage accounting");
    auto expected_native = native<P>(expected);
    typename node::pair_type expected_main;
    typename node::native_pointer expected_secondary;
    std::map<void const *, table> tables{{result.merged_native.get(), expected}};
    if (secondary_plan) {
      check(result.main == deeper_pin.lock() && result.secondary == result.merged_native, "secondary destination topology");
      check(charged[1] == 0, "secondary destination charged an index stage");
      expected_main = result.main; expected_secondary = expected_native;
      tables.emplace(result.main->native_owner().get(), deeper_table);
    } else {
      check(result.main->native_owner() == result.merged_native && !result.secondary, "main destination topology");
      check(result.main->main_target() == deeper_pin.lock() && result.main->secondary_target() == side_pin.lock(),
            "main destination lost exact deeper targets");
      check(charged[1] == result.main->virtual_size(), "destination stage accounting");
      expected_main = std::make_shared<node const>(node::adopt_native(expected_native, deeper_pin.lock(), side_pin.lock()));
      check(wire(*result.main) == wire(*expected_main), "destination wire differs from batch oracle");
      tables.emplace(deeper_pin.lock()->native_owner().get(), deeper_table);
      tables.emplace(side_pin.lock().get(), side_table);
    }
    auto expected_carrier = node::build({}, expected_main, expected_secondary);
    check(wire(*result.carrier) == wire(expected_carrier), "carrier wire differs from batch oracle");
    query_result(result, tables);
  }

  template <class P> void empty_and_invalid() {
    using node = cola_index<P>;
    using job = cola_local_merge_job<P>;
    auto empty = native<P>({});
    rejects<std::invalid_argument>([&] { job bad({}, empty, cola_destination_plan<P>::for_main()); });
    rejects<std::invalid_argument>([&] { job bad(empty, {}, cola_destination_plan<P>::for_main()); });
    rejects<std::invalid_argument>([&] { (void)cola_destination_plan<P>::for_main({}, empty); });
    rejects<std::invalid_argument>([&] { (void)cola_destination_plan<P>::for_secondary({}); });
    auto main = std::make_shared<node const>(node::build({}));
    auto plan = cola_destination_plan<P>::for_secondary(main);
    auto kept = std::move(plan);
    rejects<std::invalid_argument>([&] { job bad(empty, empty, std::move(plan)); });
    for (bool secondary : {false, true}) {
      job work(empty, empty, secondary ? std::move(kept) : cola_destination_plan<P>::for_main());
      unsigned stages = 0;
      while (!work.done()) {
        check(work.stage_done() && work.step(1) == 0, "empty stage charged records");
        work.finish_stage(); ++stages;
      }
      check(stages == (secondary ? 2u : 3u), "empty phase sequence");
      auto result = work.finish();
      check(!result.merged_native->size() && !result.carrier->virtual_size(), "empty output not empty");
    }
  }
  void poisoned() {
    using P = storage_policy<profile_unit::byte, variable_values, 3>;
    auto input = fixture<P>(1, 0, 1);
    auto a = native<P>(input), b = native<P>(input);
    std::weak_ptr<profile_array<P> const> weak_a = a, weak_b = b;
    auto calls = std::make_shared<unsigned>(0);
    {
      cola_local_merge_job<P, throwing_compose> job(a, b, cola_destination_plan<P>::for_main(), {calls});
      a.reset(); b.reset();
      check(job.step(1) == 1, "composition failure fixture first step");
      rejects<std::runtime_error>([&] { job.step(1); });
      check(job.failed() && !job.done() && !job.stage_done(), "execution failure did not poison");
      rejects([&] { job.step(0); }); rejects([&] { job.finish_stage(); }); rejects([&] { (void)job.finish(); });
      check(!weak_a.expired() && !weak_b.expired(), "failed job lost source pins");
    }
    check(weak_a.expired() && weak_b.expired(), "destroyed failed job retained source pins");
  }
  template <class P> void policy() {
    scenario<P>(false); scenario<P>(true); empty_and_invalid<P>();
    if constexpr (!P::fixed_width) {
      for (bool secondary : {false, true}) {
        auto moves = std::make_shared<unsigned>(0);
        scenario<P>(secondary, concatenate(moves), true, moves);
      }
    }
  }
}

int main() try {
  policy<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 16>>();
  policy<storage_policy<profile_unit::byte, variable_values, 15>>();
  policy<storage_policy<profile_unit::bit, variable_values, 3, golomb<3>, 16>>();
  policy<storage_policy<profile_unit::bit, variable_values, 15>>();
  policy<storage_policy<profile_unit::byte, fixed_values<0>, 3>>();
  policy<storage_policy<profile_unit::bit, fixed_values<5>, 15>>();
  scenario<storage_policy<profile_unit::byte, variable_values, 3>>(false, key_concatenate{}, true);
  poisoned();
  std::cout << "COLA local merge tests passed\n";
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks local COLA merge stages against original tables and batch wire oracles.
 */
