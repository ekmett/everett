/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks validated initial typed batches, clean generations and later chronological updates.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/replacement_rebuild.h>
#include <diet/sort_runtime.h>

#include <cassert>
#include <map>
#include <stdexcept>

namespace {
  using namespace diet;
  using strings = unsorted<std::optional<std::string>>;

  template <class P> struct observed_storage : sort_runtime_storage<P> {
    using base_type = sort_runtime_storage<P>;
    inline static unsigned initializations = 0;
    inline static bool fail = false;
    static auto sorted_native(std::span<profile_record const> records) {
      ++initializations;
      if (fail) throw std::runtime_error("initial native failure");
      return base_type::sorted_native(records);
    }
  };
  template <class P> using observed_family = sort_runtime_family<P,
    registry_selector<typename P::registry_type>, observed_storage<P>>;
  using typed = typed_engine<string_policy, wrapping_fingerprint_algebra, 256, observed_family<string_policy>>;
  using rebuilt = replacement_rebuild_engine<string_policy, wrapping_fingerprint_algebra, 256, observed_family<string_policy>>;

  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    assert(rejected);
  }
  template <class E> auto charge(E const & engine) {
    if constexpr (requires { engine.work().foreground_charged; })
      return engine.work().foreground_charged + engine.work().granted;
    else return engine.work().charged;
  }
  template <class E> auto contribute(E & engine, typename E::contribution_type input) {
    auto quote = E::reservation(input);
    auto prior = charge(engine);
    auto result = engine.contribute(std::move(input));
    assert(charge(engine) - prior <= quote.work);
    return result;
  }
  template <class E> void drain(E & engine) {
    unsigned steps = 0;
    while (engine.pending()) {
      engine.advance(1'000'000);
      assert(++steps < 10000);
    }
  }
  template <class C> void verify(C const & state, std::map<std::string, std::string> const & expected) {
    std::uint64_t signature = 0;
    for (auto const & [key, value] : expected) {
      assert(state.get(key) == value);
      signature += sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, value);
    }
    assert(state.live_count() == expected.size() && state.signature() == signature);
    auto rows = scan(state);
    auto next = expected.begin();
    while (auto row = rows.next()) {
      assert(next != expected.end() && row->key == next->first && row->value == next->second);
      ++next;
    }
    assert(next == expected.end());
  }
  template <class E> auto batch(unsigned size, std::map<std::string, std::string> & expected) {
    auto input = E::batch();
    for (unsigned i = size; i; --i) {
      auto key = i == 1 ? std::string{} : i == 2 ? std::string("k\0", 2) : "key/" + std::to_string(i);
      auto value = i % 3 ? std::string("v\0", 2) + std::to_string(i) : std::string{};
      input.put(key, value);
      expected.emplace(std::move(key), std::move(value));
    }
    return std::move(input).finish();
  }

  template <class E> void initial_batches() {
    for (auto size : {0u, 1u, 2u, 3u, 4u, 7u, 16u, 64u}) {
      observed_storage<string_policy>::initializations = 0;
      E engine;
      auto empty = engine.snapshot();
      std::map<std::string, std::string> expected;
      auto initial = contribute(engine, batch<E>(size, expected));
      bool direct = size >= 2 && std::has_single_bit(size);
      assert(observed_storage<string_policy>::initializations == (direct ? std::bit_width(size) : 0u));
      assert(initial.runtime().admissions() == size && !empty.live_count());
      verify(initial, expected);
      if (direct) {
        assert(charge(engine) > 0);
        if constexpr (requires { engine.work().charged; }) {
          auto work = engine.work();
          assert(work.charged <= work.granted && work.admissions == size);
          assert(work.native_inputs == 2 * size - 2 && work.native_outputs == 3 * size - 2);
          assert(work.merges == std::uint64_t(std::bit_width(size) - 1) && work.indexes && work.index_occurrences);
        }
        assert(!engine.pending() && engine.admission_ready());
        assert(initial.runtime().frontier().service_due == 0);
        std::uint64_t through = 0;
        for (auto const & run : initial.runtime().runs()) {
          assert(run->first == through);
          through = run->last;
        }
        assert(through == size);
      }
      if constexpr (requires { engine.status(); }) {
        assert(initial.metadata().clean_base == size && !initial.metadata().mutations);
        assert(engine.work().mutations == size && !engine.work().generations);
      }
      if (!size) continue;
      expected[""] = "changed";
      contribute(engine, E::put("", "changed"));
      contribute(engine, E::erase(""));
      expected.erase("");
      drain(engine);
      verify(engine.snapshot(), expected);
      std::map<std::string, std::string> original;
      (void)batch<E>(size, original);
      verify(initial, original);
      auto resumed = E::from_snapshot(engine.snapshot());
      contribute(resumed, E::put("after-restart", "value"));
      expected["after-restart"] = "value";
      verify(resumed.snapshot(), expected);
      assert(!engine.snapshot().get("after-restart"));
    }
  }

  template <class E> void preflight() {
    E engine;
    auto empty = engine.snapshot();
    auto prior = charge(engine);
    observed_storage<string_policy>::initializations = 0;
    auto invalid = E::batch();
    invalid.put("a-valid", "v").erase("z-missing");
    rejects([&] { engine.contribute(std::move(invalid).finish()); });
    auto rejected = engine.snapshot();
    assert(!engine.failed() && rejected.runtime().same_layout(empty.runtime()));
    assert(charge(engine) == prior && !observed_storage<string_policy>::initializations);

    E other;
    contribute(other, E::put("key", "old"));
    auto stale = other.snapshot().batch();
    stale.put("key", "new").put("new", "v");
    rejects([&] { engine.contribute(std::move(stale).finish()); });
    E foreign("another-schema");
    auto foreign_batch = foreign.snapshot().batch();
    foreign_batch.put("a", "one").put("b", "two");
    rejects([&] { engine.contribute(std::move(foreign_batch).finish()); });
    auto duplicate = E::batch();
    duplicate.put("same", "one").put("same", "two");
    rejects([&] { (void)std::move(duplicate).finish(); });
    assert(!engine.failed() && !observed_storage<string_policy>::initializations && charge(engine) == prior);

    std::map<std::string, std::string> expected;
    auto initial = contribute(engine, batch<E>(4, expected));
    assert(observed_storage<string_policy>::initializations == 3);
    auto second = initial.batch();
    second.put("later/a", "one").put("later/b", "two");
    contribute(engine, std::move(second).finish());
    assert(observed_storage<string_policy>::initializations == 3);
    expected["later/a"] = "one"; expected["later/b"] = "two";
    verify(engine.snapshot(), expected);
  }

  template <class E> void construction_failure() {
    E engine;
    auto old = engine.snapshot();
    observed_storage<string_policy>::fail = true;
    std::map<std::string, std::string> expected;
    rejects([&] { engine.contribute(batch<E>(4, expected)); });
    observed_storage<string_policy>::fail = false;
    auto failed = engine.snapshot();
    assert(engine.failed() && failed.runtime().same_layout(old.runtime()));
    assert(!old.live_count() && !old.get("key/2"));
    rejects([&] { engine.contribute(E::put("retry", "forbidden")); });
  }

  struct append_sort {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = string_value<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type old, state_type const & next) { return old + next; }
    static state_type compose(std::string const &, state_type old, state_type const & next) { return old + next; }
    static bool present(std::string const &, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::string const & key) { return u64_table_hash{}.key(key) + 23; }
    inline static bool fail_hash = false;
    static std::uint64_t hash_value(std::string const &, state_type const & value) {
      if (fail_hash && value == "X") throw std::runtime_error("hash preflight failure");
      return u64_table_hash{}.key(value) + 29;
    }
  };
  void mixed_chronology() {
    using P = storage_policy<bin<tip<strings>, tip<append_sort>>>;
    using E = typed_engine<P, wrapping_fingerprint_algebra, 256, observed_family<P>>;
    E engine("mixed-initial/1");
    auto empty = engine.snapshot();
    auto invalid = E::batch();
    invalid.put<strings>("same", "replacement").change<append_sort>("other", "X");
    append_sort::fail_hash = true;
    rejects([&] { engine.contribute(std::move(invalid).finish()); });
    append_sort::fail_hash = false;
    auto rejected = engine.snapshot();
    assert(!engine.failed() && !observed_storage<P>::initializations &&
      rejected.runtime().same_layout(empty.runtime()));
    auto input = E::batch();
    input.put<strings>("same", "replacement").put<strings>("other", "");
    input.change<append_sort>("same", "A").change<append_sort>("other", "X");
    auto initial = contribute(engine, std::move(input).finish());
    assert(observed_storage<P>::initializations == 3 && !engine.pending());
    auto expected_hash = sort_semantics<strings>::hash_key("same") * sort_semantics<strings>::hash_value("same", "replacement") +
      sort_semantics<strings>::hash_key("other") * sort_semantics<strings>::hash_value("other", "") +
      append_sort::hash_key("same") * append_sort::hash_value("same", "A") +
      append_sort::hash_key("other") * append_sort::hash_value("other", "X");
    assert(initial.signature() == expected_hash && initial.live_count() == 4);
    for (auto suffix : {"B", "C", "D"}) contribute(engine, E::change<append_sort>("same", suffix));
    drain(engine);
    assert(engine.snapshot().get<append_sort>("same") == "ABCD");
    assert(initial.get<append_sort>("same") == "A" && engine.snapshot().get<strings>("same") == "replacement");
    expected_hash += append_sort::hash_key("same") *
      (append_sort::hash_value("same", "ABCD") - append_sort::hash_value("same", "A"));
    auto final = engine.snapshot();
    assert(final.signature() == expected_hash && final.runtime().admissions() == 7);
  }

  struct custom_replacement : sort_semantics<strings> {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
    inline static unsigned cleans = 0;
    static state_type clean(std::string const &, state_type const & state) { ++cleans; return state; }
  };
  void wrapper_scope() {
    using P = storage_policy<tip<custom_replacement>>;
    using E = replacement_rebuild_engine<P, wrapping_fingerprint_algebra, 256, observed_family<P>>;
    E custom("custom-clean/1");
    auto input = E::batch(); input.put("a", "one").put("b", "two");
    contribute(custom, std::move(input).finish());
    assert(!observed_storage<P>::initializations && custom_replacement::cleans);
    assert(custom.snapshot().get("a") == "one" && custom.snapshot().get("b") == "two");

    rebuilt reused;
    contribute(reused, rebuilt::put("a", "old"));
    contribute(reused, rebuilt::erase("a"));
    auto cleared = reused.snapshot();
    assert(!cleared.runtime().admissions() && reused.work().mutations == 2);
    auto before = observed_storage<string_policy>::initializations;
    auto next = rebuilt::batch(); next.put("a", "one").put("b", "two");
    contribute(reused, std::move(next).finish());
    assert(observed_storage<string_policy>::initializations == before);
    assert(reused.snapshot().metadata().clean_base == 2 && reused.work().mutations == 4);
  }

  void opaque_initialization() {
    using E = typed_engine<string_policy, wrapping_fingerprint_algebra, 256, redundant_runtime_family<string_policy>>;
    E engine;
    std::map<std::string, std::string> expected;
    auto result = contribute(engine, batch<E>(8, expected));
    verify(result, expected);
    assert(!engine.pending() && engine.work().merges == 3 && engine.work().native_inputs == 14);
  }

  void binary_fallback() {
    typed_engine<> engine;
    std::map<std::string, std::string> expected;
    auto result = contribute(engine, batch<typed_engine<>>(4, expected));
    verify(result, expected);
    assert(result.runtime().admissions() == 4);
  }
}
int main() {
  initial_batches<typed>(); initial_batches<rebuilt>();
  preflight<typed>(); preflight<rebuilt>();
  construction_failure<typed>(); construction_failure<rebuilt>();
  mixed_chronology(); wrapper_scope(); opaque_initialization(); binary_fallback();
}
