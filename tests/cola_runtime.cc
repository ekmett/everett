/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks charged runtime carries, chronology, snapshots and failures.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/cola_runtime.h>
#include <iostream>
#include <cstdio>
#include <filesystem>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <map>
#include <string>

namespace {
  using namespace diet;
  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class E = std::logic_error, class F> void rejects(F && f) {
    bool caught = false; try { f(); } catch (E const &) { caught = true; }
    check(caught, "expected rejection");
  }
  std::string bits(bit_view v) {
    std::string out; for (std::uint64_t i = 0; i != v.size(); ++i) out += v.at(i) ? '1' : '0'; return out;
  }
  template <class P> std::string key(unsigned n) {
    std::string result;
    for (unsigned i = 0; i != (P::unit == profile_unit::bit ? 11u : 16u); ++i)
      result += (n >> ((P::unit == profile_unit::bit ? 11u : 16u) - i - 1)) & 1 ? '1' : '0';
    return result;
  }
  template <class P> profile_record record(unsigned k, unsigned v) {
    auto width = P::value_width.value_or(1) * P::bits_per_unit;
    std::string value(width, '0');
    for (std::size_t i = 0; i != value.size(); ++i) value[i] = (v >> (i % 7)) & 1 ? '1' : '0';
    return {bit_string::from_bits(key<P>(k)), bit_string::from_bits(value)};
  }
  template <class P> std::vector<std::string> matches(cola_runtime_snapshot<P> const & s, std::string const & key) {
    auto encoded = bit_string::from_bits(key); auto cursor = s.cursor(encoded.view());
    std::vector<std::string> out;
    unsigned steps = 0;
    while (!cursor.done()) {
      check(++steps < 10000, "query did not terminate"); cursor.step(1);
      if (cursor.has_match()) { auto match = cursor.take_match(); out.push_back(bits(match.value.view())); }
    }
    return out;
  }
  template <class P> void replacement(cola_runtime_snapshot<P> const & s, std::map<unsigned, std::string> const & table) {
    for (unsigned i = 0; i != 24; ++i) {
      auto found = matches(s, key<P>(i)); auto expected = table.find(i);
      check(found.empty() == (expected == table.end()), "replacement presence");
      if (!found.empty()) check(found.front() == expected->second, "replacement newest first");
    }
  }
  template <class R> void finish(R & runtime, std::uint64_t budget) {
    unsigned steps = 0;
    while (runtime.pending()) {
      check(++steps < 1000000, "runtime failed to settle");
      runtime.advance(budget);
    }
  }
  void accounting(cola_runtime_work w) {
    check(w.charged == w.setup + w.native_allowance + w.index_occurrences + w.target_scan_allowance +
      w.navigation_allowance + w.directory_entries + w.publication, "charge categories");
    check(w.granted >= w.charged, "unfunded work");
  }
  template <class P> void scenario() {
    cola_runtime<P> runtime;
    std::map<unsigned, std::string> oracle;
    auto original = runtime.snapshot();
    check(original.admissions() == 0 && original.settled(), "empty runtime");
    std::vector<std::pair<cola_runtime_snapshot<P>, std::map<unsigned, std::string>>> history;
    for (unsigned i = 0; i != 80; ++i) {
      auto r = record<P>(i % 19, i);
      if (i % 3 == 0) finish(runtime, 1);
      if (runtime.admission_ready()) {
        auto expected = runtime.admission_cost(), before = runtime.work().charged;
        check(runtime.try_contribute(r).has_value(), "ready refusal");
        check(runtime.work().charged - before == expected, "admission quote differs from charged work");
      } else runtime.contribute(r, 0);
      oracle[i % 19] = bits(r.value.view());
      replacement(runtime.snapshot(), oracle);
      if (runtime.pending()) {
        auto before = runtime.snapshot(); auto w = runtime.work().charged;
        check(!runtime.try_contribute(record<P>(99, 1)), "pending admission accepted");
        check(before.same_layout(runtime.snapshot()) && w == runtime.work().charged, "refused admission mutated");
        check(before.same_layout(runtime.advance(0)), "zero service changed layout");
        auto forked = cola_runtime<P>::from_snapshot(before);
        auto split = cola_runtime<P>::from_snapshot(before);
        finish(forked, 1); finish(split, 100000);
        replacement(forked.snapshot(), oracle); replacement(split.snapshot(), oracle);
        check(forked.work().charged == split.work().charged, "budget partition changed charged work");
        check(forked.snapshot().settled(), "settled fork");
      }
      if (i % 13 == 0) history.emplace_back(runtime.snapshot(), oracle);
      accounting(runtime.work());
    }
    auto before = runtime.snapshot(); finish(runtime, 7);
    replacement(runtime.snapshot(), oracle); replacement(before, oracle);
    check(runtime.work().native_merges > 50 && runtime.work().indexes > 80 && runtime.work().carriers,
      "runtime did not execute native/index/carrier work");
    auto settled = runtime.snapshot();
    check(settled.admissions() == 80 && settled.runs().size() == 2, "binary mass frontier");
    for (auto const & [snapshot, expected] : history) replacement(snapshot, expected);
    replacement(original, {});
    auto old_oracle = oracle;
    auto moved = std::move(runtime); rejects([&] { (void)runtime.snapshot(); });
    auto added = record<P>(22, 3); moved.contribute(added); oracle[22] = bits(added.value.view());
    replacement(moved.snapshot(), oracle);
    std::vector<cola_runtime_interval> intervals;
    for (auto const & run : before.runs()) intervals.push_back({run.first, run.last});
    auto restored = cola_runtime_snapshot<P>::restore(before.query_root().head(), intervals);
    replacement(restored, old_oracle);
  }
  struct append_value {
    bit_string operator()(bit_view, bit_view older, bit_view newer) const {
      return bit_string::from_bits(bits(older) + bits(newer));
    }
  };
  template <class P> void composition() {
    cola_runtime<P, append_value> runtime;
    std::map<unsigned, std::string> oracle;
    for (unsigned i = 0; i != 40; ++i) {
      auto r = record<P>(i % 3, i); runtime.contribute(r, 0); oracle[i % 3] += bits(r.value.view());
      for (auto const & [k, expected] : oracle) {
        auto found = matches(runtime.snapshot(), key<P>(k)); std::string folded;
        for (auto p = found.rbegin(); p != found.rend(); ++p) folded += *p;
        check(folded == expected, "noncommutative chronology");
      }
    }
    finish(runtime, 1);
    check(runtime.snapshot().admissions() == 40, "duplicate mass lost");
    for (auto const & [k, expected] : oracle) {
      auto found = matches(runtime.snapshot(), key<P>(k)); std::string folded;
      for (auto p = found.rbegin(); p != found.rend(); ++p) folded += *p;
      check(folded == expected, "merged noncommutative chronology");
    }
  }
  template <class P> void batch() {
    cola_runtime<P, append_value> runtime;
    std::vector<profile_record> input;
    std::map<unsigned, std::string> oracle;
    for (unsigned i = 0; i != 24; ++i) {
      input.push_back(record<P>(i / 3, i)); oracle[i / 3] += bits(input.back().value.view());
    }
    auto before = runtime.snapshot(); auto after = runtime.contribute(input, 0);
    check(after.admissions() == input.size() && before.admissions() == 0, "batch admission mass");
    finish(runtime, 3);
    for (auto const & [k, expected] : oracle) {
      auto found = matches(runtime.snapshot(), key<P>(k)); std::string folded;
      for (auto p = found.rbegin(); p != found.rend(); ++p) folded += *p;
      check(folded == expected, "batch chronology");
    }
  }
  struct failing {
    std::shared_ptr<bool> armed;
    bit_string operator()(bit_view, bit_view newer) const {
      if (*armed) throw std::runtime_error("intentional compose failure");
      return bit_string::from_bits(bits(newer));
    }
  };
  template <class P> void failures() {
    auto armed = std::make_shared<bool>(false); cola_runtime<P, failing> runtime(failing{armed});
    runtime.contribute(record<P>(1, 0), 0); auto before = runtime.contribute(record<P>(1, 1), 0);
    *armed = true;
    rejects<std::runtime_error>([&] { finish(runtime, 1000); });
    check(runtime.failed() && before.same_layout(runtime.snapshot()), "failure lost published snapshot");
    rejects([&] { runtime.advance(1); });
    auto fork = cola_runtime<P>::from_snapshot(before); finish(fork, 1);
    auto latest = record<P>(1, 1);
    check(matches(fork.snapshot(), key<P>(1)).front() == bits(latest.value.view()), "failure fork recovery");
    cola_runtime<P> valid; auto empty = valid.snapshot();
    std::vector<profile_record> bad{record<P>(2, 1), record<P>(1, 1)};
    rejects<std::invalid_argument>([&] { valid.contribute(bad); });
    check(!valid.failed() && empty.same_layout(valid.snapshot()), "invalid batch changed runtime");
    cola_runtime<P, failing> transactional(failing{armed});
    auto transaction_before = transactional.snapshot();
    std::vector<profile_record> collision{record<P>(0, 0), record<P>(0, 1), record<P>(1, 0)};
    rejects<std::runtime_error>([&] { transactional.contribute(collision, 0); });
    check(transactional.failed() && transaction_before.same_layout(transactional.snapshot()), "batch failure published a prefix");
    std::vector<cola_runtime_interval> invalid{{1, 2}, {2, 3}};
    rejects<std::invalid_argument>([&] { (void)cola_runtime_snapshot<P>::restore(before.query_root().head(), invalid); });
  }
#if defined(__APPLE__) || defined(__linux__)
  struct temporary {
    std::filesystem::path root;
    unsigned serial = 1;
    object_attempt_id attempt{std::string(32, 'e')};
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-runtime-XXXXXX").string();
      auto made = ::mkdtemp(name.data()); if (!made) throw std::runtime_error("mkdtemp"); root = made;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    object_id fresh() { char text[33]; std::snprintf(text, sizeof text, "%032x", serial++); return object_id(text); }
  };
  template <class P> std::shared_ptr<mapped_cola_blob<P> const> persist(temporary & files, cola_runtime_snapshot<P> const & snapshot) {
    std::vector<typename cola_runtime_node<P>::pair_type> chain;
    for (auto p = snapshot.query_root().head(); p; p = p->main_target()) chain.push_back(p);
    std::shared_ptr<mapped_cola_blob<P> const> target;
    for (auto i = chain.rbegin(); i != chain.rend(); ++i) {
      auto native_id = files.fresh(), index_id = files.fresh();
      auto native = (*i)->native_owner()->owned(); check(bool(native), "fixture requires owned source");
      encode_native_sections(*native).seal(files.root, native_id, files.attempt);
      std::optional<blob_identity> target_id;
      if (target) target_id = target->identity();
      encode_cola_sections(*(*i)->built(), native_id, target_id).seal(files.root, index_id, files.attempt);
      auto mapped_native = std::make_shared<diet::mapped_native<P> const>(diet::mapped_native<P>::open(
        files.root / object_path(native_id, file_kind::native_blob)));
      auto index = std::make_shared<mapped_cola_index<P> const>(mapped_cola_index<P>::open(
        files.root / object_path(index_id, file_kind::fractional_index)));
      target = mapped_cola_blob<P>::bind({native_id, index_id}, std::move(mapped_native), std::move(index), std::move(target));
    }
    return open_mapped_cola_query<P>(files.root, target->identity()).head();
  }
  struct protected_payloads {
    std::vector<std::pair<void *, std::size_t>> regions;
    void add(std::span<std::byte const> bytes) {
      auto page = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
      auto first = reinterpret_cast<std::uintptr_t>(bytes.data()), end = first + bytes.size();
      first = (first + page - 1) / page * page; end = end / page * page;
      if (end <= first) return;
      auto address = reinterpret_cast<void *>(first); auto count = static_cast<std::size_t>(end - first);
      check(::mprotect(address, count, PROT_NONE) == 0, "protect mapped payload"); regions.emplace_back(address, count);
    }
    ~protected_payloads() { for (auto [address, count] : regions) if (::mprotect(address, count, PROT_READ)) std::terminate(); }
  };
  template <class P> void mapped_restore() {
    temporary files; cola_runtime<P> source;
    std::map<std::string, std::string> oracle;
    auto make = [](unsigned i) {
      auto r = record<P>(i % 7, i);
      r.key = bit_string::from_bits(std::string(static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)) * 24, '1') + bits(r.key.view()));
      return r;
    };
    for (unsigned i = 0; i != 18; ++i) {
      auto r = make(i); source.contribute(r, 0); oracle[bits(r.key.view())] = bits(r.value.view());
    }
    auto snapshot = source.snapshot(); check(!snapshot.settled(), "restore pending fixture");
    std::vector<cola_runtime_interval> intervals;
    for (auto const & run : snapshot.runs()) intervals.push_back({run.first, run.last});
    auto mapped = persist(files, snapshot); mapped->scan();
    std::weak_ptr<mapped_cola_blob<P> const> pin = mapped;
    std::optional<cola_runtime<P>> restored;
    {
      protected_payloads guarded;
      for (auto p = mapped; p; p = p->main_target()) {
        guarded.add(p->native_object()->view().bytes());
        guarded.add(p->view().borrowed(0).bytes());
      }
      check(!guarded.regions.empty(), "payload guard fixture empty");
      auto head = cola_runtime_node<P>::from_mapped(mapped);
      auto state = cola_runtime_snapshot<P>::restore(std::move(head), intervals);
      restored.emplace(cola_runtime<P>::from_snapshot(std::move(state)));
      check(restored->pending() && restored->snapshot().admissions() == 18, "restored debt frontier");
      check(restored->advance(0).admissions() == 18, "restore zero service");
    }
    mapped.reset(); std::filesystem::remove_all(files.root);
    check(!pin.expired(), "restored runtime lost mapped pins");
    auto verify = [&](auto const & state) {
      for (auto const & [k, expected] : oracle) {
        auto found = matches(state, k); check(!found.empty() && found.front() == expected, "mapped restore query");
      }
    };
    verify(restored->snapshot()); finish(*restored, 1); verify(restored->snapshot());
    auto r = make(19); restored->contribute(r, 0); oracle[bits(r.key.view())] = bits(r.value.view());
    verify(restored->snapshot()); finish(*restored, 100); verify(restored->snapshot());
  }
#endif

}
int main() {
  using bits = diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 3, diet::golomb<3>, 5>;
  using bytes = diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 7, diet::exponential_golomb<0>, 4>;
  scenario<bits>(); scenario<bytes>(); composition<bits>(); composition<bytes>(); failures<bits>(); failures<bytes>(); batch<bits>(); batch<bytes>();
  using fixed = diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<diet::fixed_values<3>>>>, 15, diet::exponential_golomb<0>, 2>;
  scenario<fixed>();
#if defined(__APPLE__) || defined(__linux__)
  mapped_restore<bits>(); mapped_restore<bytes>();
#endif
  std::cout << "cola runtime tests passed\n";
}
