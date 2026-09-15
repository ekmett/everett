/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks direct sort records through typed redundant updates, scans and mapped restoration.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sort_runtime.h>
#include <diet/typed_scan.h>

#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>

namespace {
  using namespace diet;
  using strings = unsorted<std::optional<std::string>>;
  using family = sort_runtime_family<>;
  using engine = typed_engine<string_policy, wrapping_fingerprint_algebra, 256, family>;
  using cola = engine::cola_type;
  void check(bool value, char const * why) { if (!value) throw std::runtime_error(why); }
  template <class F> void rejects(F && fn) {
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected sort runtime rejection");
  }
  template <class E> void drain(E & value) { while (value.pending()) value.advance(4096); }
  template <class C> void verify(C const & state, std::map<std::string, std::string> const & entries) {
    std::uint64_t signature = 0;
    for (auto const & [key, value] : entries) {
      check(state.get(key) == value, "lookup differs from oracle");
      signature += sort_semantics<strings>::hash_key(key) *
        sort_semantics<strings>::hash_value(key, std::optional<std::string>(value));
    }
    check(state.signature() == signature && state.live_count() == entries.size(), "hash/count differs from oracle");
    auto scan = diet::scan(state); auto oracle = entries.begin();
    check(scan.step(0) == 0, "zero scan budget changed state");
    while (!scan.done()) {
      scan.step(1);
      if (scan.has_row()) {
        auto row = scan.take_row();
        check(oracle != entries.end() && row.key == oracle->first && row.value == oracle->second, "scan differs from oracle");
        ++oracle;
      }
    }
    check(oracle == entries.end() && !scan.failed(), "scan lost rows");
  }
  void replacements() {
    static_assert(std::same_as<engine::compose_type, replace_native_value>);
    engine active;
    check(active.snapshot().metadata().schema_id != typed_engine<>{}.snapshot().metadata().schema_id,
      "raw keys reused escaped-key schema identity");
    auto command = engine::put(std::string("a\0", 2), "v");
    check(command.records()[0].key.bit_size == 17, "string key gained escapes/terminator");
    engine replacement;
    replacement.contribute(engine::put("key", "old"));
    auto newer = engine::put("key", std::string("\0value", 6));
    replacement.contribute(newer); drain(replacement);
    auto replaced = replacement.snapshot();
    check(replaced.runtime().runs().size() == 1, "replacement fixture did not merge");
    auto wire = replaced.runtime().runs()[0]->native->view().encoded_at(0).value;
    check(!compare_bits(wire, newer.records()[0].value.view()), "replacement merge changed newer arrow encoding");
    engine old_schema("explicit/user-version");
    auto initial = active.snapshot(); auto metadata = initial.metadata();
    rejects([&] { cola::restore(initial.runtime(), metadata, "diet.optional-string/code0/v1"); });
    std::map<std::string, std::string> expected;
    std::vector<std::pair<cola, decltype(expected)>> saved;
    std::uint64_t random = 0x742da371;
    for (unsigned i = 0; i != 513; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      auto key = random % 5 ? std::string(31, 'p') + "/" + std::to_string(random % 73) :
        std::string(static_cast<std::size_t>(random % 7), '\0');
      if ((random & 7) == 0 && expected.contains(key)) {
        active.contribute(engine::erase(key)); expected.erase(key);
      } else {
        std::string value(static_cast<std::size_t>(random % 13), char(i));
        active.contribute(engine::put(key, value)); expected[key] = value;
      }
      if (i % 31 == 0) verify(active.snapshot(), expected);
      if (i % 79 == 0) saved.emplace_back(active.snapshot(), expected);
    }
    drain(active); verify(active.snapshot(), expected);
    auto restarted = engine::from_snapshot(active.snapshot());
    restarted.contribute(engine::put("after-restart", "yes"));
    expected["after-restart"] = "yes"; verify(restarted.snapshot(), expected);
    for (auto const & [snapshot, entries] : saved) verify(snapshot, entries);
    rejects([&] { active.contribute(engine::erase("not there")); });
    check(!active.failed(), "preflight rejection poisoned engine");
    auto current = active.snapshot();
    for (auto const & run : current.runtime().runs()) {
      auto view = run->native->view(); view.scan();
      check(view.metadata().version == 3, "runtime wrote opaque native framing");
    }
  }

  struct sums {
    using encoding = bit_encoding<fixed_values<64>>;
    using key_codec = unsigned_key<16>;
    using value_codec = unsigned_value<64>;
    using state_type = std::uint64_t;
    static state_type initial(std::uint64_t) { return 0; }
    static state_type apply(std::uint64_t, state_type before, state_type next) { return before + next; }
    static state_type compose(std::uint64_t key, state_type before, state_type next) { return apply(key, before, next); }
    static bool present(std::uint64_t, state_type value) { return value != 0; }
    static std::uint64_t hash_key(std::uint64_t key) { return key + 7; }
    static std::uint64_t hash_value(std::uint64_t, state_type value) { return value + 11; }
  };
  struct arrows {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = string_value<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type before, state_type const & next) { return before + next; }
    static state_type compose(std::string const & key, state_type before, state_type const & next) {
      check(key == "a" || key == std::string("a\0", 2), "composition decoded escaped/wrong key");
      return apply(key, std::move(before), next);
    }
    static bool present(std::string const &, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::string const & key) { return u64_table_hash{}.key(key); }
    static std::uint64_t hash_value(std::string const &, state_type const & value) { return u64_table_hash{}.key(value); }
  };
  using registry = bin<tip<strings>, bin<tip<sums>, tip<arrows>>>;
  using policy = storage_policy<registry, 3, golomb<3>, 16>;
  using mixed = typed_engine<policy, wrapping_fingerprint_algebra, 256, sort_runtime_family<policy>>;
  void heterogeneous() {
    mixed active("mixed/raw-key/1");
    std::map<std::string, std::string> text, sequence;
    std::map<std::uint64_t, std::uint64_t> counts;
    for (unsigned i = 0; i != 91; ++i) {
      auto key = i % 2 ? "a" : std::string("a\0", 2);
      auto delta = "[" + std::to_string(i) + "]";
      auto batch = mixed::batch();
      batch.put<strings>(std::to_string(i % 13), delta).change<sums>(i % 17, i + 1).change<arrows>(key, delta);
      active.contribute(std::move(batch).finish());
      text[std::to_string(i % 13)] = delta; counts[i % 17] += i + 1; sequence[key] += delta;
      check(active.snapshot().get<arrows>(key) == sequence[key], "noncommutative chronology");
    }
    auto before = active.snapshot(); drain(active); auto after = active.snapshot();
    check(before.metadata() == after.metadata(), "maintenance changed typed metadata");
    auto check_sort = [&]<class S>(auto const & values) {
      auto rows = scan<S>(after); auto oracle = values.begin();
      while (auto row = rows.next()) {
        check(oracle != values.end() && row->key == oracle->first && row->value == oracle->second, "mixed scan");
        check(after.template get<S>(row->key) == row->value, "mixed scan/lookup parity"); ++oracle;
      }
      check(oracle == values.end(), "mixed scan omitted rows");
    };
    check_sort.template operator()<strings>(text);
    check_sort.template operator()<sums>(counts);
    check_sort.template operator()<arrows>(sequence);
    auto record = mixed::change<sums>(3, 7).records()[0];
    auto singleton = sort_runtime_storage<policy>::singleton(record);
    check(singleton->view().encoded_at(0).value.size() == 64, "integer value gained length field");
    check(record.key.bit_size == 18, "integer key gained an opaque key envelope");
    rejects([&] { sort_key_transport<policy>::decode<sums>(record.key.view()); });
  }

  struct bits {
    using encoding = bit_encoding<fixed_values<3>>;
    using key_codec = fc_bit_key<golomb<2>>;
    using value_codec = unsigned_value<3>;
    using state_type = std::uint64_t;
    static state_type initial(bit_string const &) { return 0; }
    static state_type apply(bit_string const &, state_type before, state_type delta) { return (before + delta) & 7; }
    static state_type compose(bit_string const & key, state_type before, state_type delta) { return apply(key, before, delta); }
    static bool present(bit_string const &, state_type value) { return value != 0; }
    static std::uint64_t hash_key(bit_string const & key) { return key.bit_size + 101; }
    static std::uint64_t hash_value(bit_string const &, state_type value) { return value; }
  };
  struct bit_selector {
    template <class Input, class F> static decltype(auto) select(Input & input, F && fn) {
      if (input.read_bits(3) != 5) throw std::invalid_argument("custom selector code");
      return std::forward<F>(fn)(std::type_identity<bits>{}, input);
    }
    template <class S, class Output> static void write(Output & output) {
      static_assert(std::same_as<S, bits>); output.write_bits(5, 3);
    }
  };
  void partial_keys() {
    using p = storage_policy<bin<tip<bits>, sort_undefined>, 7, exponential_golomb<1>, 3>;
    using f = sort_runtime_family<p, bit_selector>;
    using e = typed_engine<p, wrapping_fingerprint_algebra, 256, f>;
    rejects([] { e needs_user_schema; });
    e active("application/nonconsecutive-schema/blue");
    std::vector<bit_string> keys;
    for (unsigned i = 0; i != 13; ++i) {
      bit_string key; sort_bit_writer out(key); out.write_bits(0, i); keys.push_back(std::move(key));
    }
    std::array<std::uint64_t, 13> values{};
    for (unsigned i = 0; i != 97; ++i) {
      auto at = i % keys.size(); auto delta = i % 7 + 1;
      active.contribute(e::change(keys[at], delta)); values[at] = (values[at] + delta) & 7;
      check(active.snapshot().get(keys[at]) == values[at], "partial-bit lookup");
    }
    drain(active);
    auto state = active.snapshot(); auto rows = scan(state); std::size_t at = 0;
    while (auto row = rows.next()) {
      while (at != values.size() && !values[at]) ++at;
      check(at != values.size() && !compare_bits(row->key.view(), keys[at].view()) && row->value == values[at], "custom selector partial scan"); ++at;
    }
    while (at != values.size() && !values[at]) ++at;
    check(at == values.size(), "partial-bit scan lost keys");
    for (auto const & run : state.runtime().runs()) {
      auto view = run->native->view(); view.scan();
      check(view.metadata().common_value_width == 3 && view.dictionary_size() == 1 && view.seeds().size() == 0,
        "fixed partial-bit framing or shared seed");
    }
  }

  void partition_and_tap() {
    engine start; auto base = start.snapshot();
    auto a = base.put("a", "one"), b = base.put("b", "two");
    auto left = engine::from_snapshot(base), right = engine::from_snapshot(base);
    left.contribute(a); auto ab = left.contribute(b);
    right.contribute(b); auto ba = right.contribute(a);
    check(ab.signature() == ba.signature() && ab.live_count() == ba.live_count(), "disjoint order changed signature");
    rejects([&] { left.contribute(base.put("a", "stale")); });
    check(!left.failed() && left.snapshot().get("a") == "one", "stale mutation changed state");
    tap<engine> active(engine{}, {16 * engine::admission_allowance, 1 << 20, 16, 4096});
    auto initial = active.snapshot();
    auto one = active.submit(engine::put("same", "one"));
    auto two = active.submit(engine::put("same", "two"));
    check(one.get()->cola.get("same") == "one" && two.get()->cola.get("same") == "two", "tap FIFO command semantics");
    auto bad = active.submit(engine::erase("missing")); rejects([&] { bad.get(); });
    check(active.apply(engine::put("next", "okay"))->cola.get("next") == "okay", "tap rejected healthy command");
    active.shutdown(); check(!active.failure() && !initial->cola.get("same"), "tap snapshots or failure");
  }

  // Test-only graph copier: exercises physical family seams without pretending
  // to provide catalog publication, identity allocation or crash recovery.
  struct mapped_copy {
    using node = family::node_type;
    using native = family::native_type;
    using pair = node::pair_type;
    using object = std::shared_ptr<family::object_type const>;
    using physical = mapped_sort_cola<string_policy>;
    std::filesystem::path root;
    unsigned serial = 0;
    struct saved_native { native::pointer facade; object_id id; };
    std::unordered_map<native const *, saved_native> natives;
    std::unordered_map<node const *, pair> pairs;
    std::unordered_map<family::object_type const *, object> objects;
    object_id id() { char value[33]; std::snprintf(value, sizeof(value), "%032x", ++serial); return object_id(value); }
    void write(std::filesystem::path const & path, std::span<std::byte const> bytes) {
      std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<char const *>(bytes.data()), bytes.size());
      if (!out) throw std::runtime_error("mapped fixture write");
    }
    saved_native save_native(std::shared_ptr<native const> const & value) {
      if (auto it = natives.find(value.get()); it != natives.end()) return it->second;
      auto identity = id(); auto path = root / (identity.hex() + ".kv");
      write(path, family::storage_type::encode_native(*value->owned()).materialize());
      auto mapped = std::make_shared<mapped_sort_profile<string_policy> const>(mapped_sort_profile<string_policy>::open(path));
      auto result = saved_native{native::from_mapped(mapped), identity};
      natives.emplace(value.get(), result); return result;
    }
    pair save_pair(pair const & value) {
      if (!value) return {};
      if (auto it = pairs.find(value.get()); it != pairs.end()) return it->second;
      auto main = save_pair(value->main_target());
      auto own = save_native(value->native_owner());
      auto side = value->secondary_target() ? std::optional(save_native(value->secondary_target())) : std::nullopt;
      auto identity = id(); auto path = root / (identity.hex() + ".index");
      auto bytes = encode_cola_sections(*value->built(), own.id,
        main ? std::optional(main->mapped()->identity()) : std::nullopt, side ? std::optional(side->id) : std::nullopt);
      write(path, bytes.materialize());
      auto mapped_index = std::make_shared<mapped_cola_index<string_policy> const>(mapped_cola_index<string_policy>::open(path));
      auto mapped = physical::bind({own.id, identity}, own.facade->mapped(), mapped_index,
        main ? main->mapped() : nullptr, side ? side->facade->mapped() : nullptr, side ? std::optional(side->id) : std::nullopt);
      auto result = node::from_mapped_parts(mapped, own.facade, main, side ? side->facade : nullptr);
      pairs.emplace(value.get(), result); return result;
    }
    family::routes_type routes(family::routes_type const & value) { return {save_object(value.main), save_object(value.secondary)}; }
    object save_object(object const & value) {
      if (!value) return {};
      if (auto it = objects.find(value.get()); it != objects.end()) return it->second;
      auto copy = *value; copy.native = save_native(value->native).facade; copy.pair = save_pair(value->pair); copy.next = routes(value->next);
      auto result = std::make_shared<family::object_type const>(std::move(copy)); objects.emplace(value.get(), result); return result;
    }
    family::snapshot_type save(family::snapshot_type const & value) {
      auto f = value.frontier(); f.root = routes(f.root);
      for (auto & level : f.levels) {
        for (auto & slot : level.slots) { slot.object = save_object(slot.object); slot.route = routes(slot.route); slot.carrier = save_pair(slot.carrier); }
        if (level.job) {
          auto & job = *level.job; job.destination_route = routes(job.destination_route);
          job.existing_main = save_object(job.existing_main); job.output = save_object(job.output); job.carrier = save_pair(job.carrier);
          if (job.merged) job.merged = save_native(job.merged).facade;
        }
      }
      return family::snapshot_type::restore(std::move(f), save_pair(value.query_root().head()));
    }
  };
  void mapped_restart(std::filesystem::path const & root) {
    using runtime = family::runtime_type<replace_native_value>;
    runtime source;
    engine expected;
    for (auto key : {"a", "b"}) {
      auto input = engine::put(key, "old"); expected.contribute(input);
      check(bool(source.try_contribute(input.records()[0])), "ready direct runtime admission");
    }
    check(source.pending(), "fixture omitted pending redundant job");
    // Stop after a native artifact has been completed and before its index.
    bool merged = false;
    for (unsigned i = 0; i != 32 && !merged; ++i) {
      source.advance(source.next_service_cost());
      auto state = source.checkpoint();
      for (auto const & level : state.frontier().levels) merged |= level.job && bool(level.job->merged);
    }
    check(merged, "fixture omitted completed hidden native artifact");
    mapped_copy copy{root, 0, {}, {}, {}};
    auto mapped = copy.save(source.checkpoint());
    auto saved = cola::restore(mapped, expected.snapshot().metadata(), expected.snapshot().metadata().schema_id);
    verify(saved, {{"a", "old"}, {"b", "old"}});
    for (auto const & entry : std::filesystem::directory_iterator(root)) std::filesystem::remove(entry.path());
    auto reopened = engine::from_snapshot(saved); drain(reopened);
    auto resumed = reopened.snapshot();
    check(resumed.runtime().query_root().head()->native_owner()->view().metadata().version == 3, "restart changed native family");
    reopened.contribute(engine::put("a", "new")); reopened.contribute(engine::put("c", "added"));
    drain(reopened); verify(reopened.snapshot(), {{"a", "new"}, {"b", "old"}, {"c", "added"}});
    verify(saved, {{"a", "old"}, {"b", "old"}});
  }
}
int main() {
  auto root = std::filesystem::temp_directory_path() / ("diet-sort-runtime-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  try {
    replacements(); heterogeneous(); partial_keys(); partition_and_tap(); mapped_restart(root);
    std::filesystem::remove_all(root); std::cout << "sort runtime tests passed\n";
  } catch (...) { std::filesystem::remove_all(root); throw; }
}
