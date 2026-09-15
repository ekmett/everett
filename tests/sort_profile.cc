/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks mixed native framing, mapped cascades and prefix-preserving merges.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sort_profile_file.h>
#include <diet/sort_profile_merge.h>

#include <fstream>
#include <iostream>
#include <map>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  using namespace diet;
  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class F> void rejects(F && f) {
    try { f(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected sort profile rejection");
  }
  struct strings {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
  };
  struct integers {
    using encoding = bit_encoding<fixed_values<64>>;
    using key_codec = unsigned_key<64>;
    using value_codec = unsigned_value<64>;
  };
  using registry = bin<tip<strings>, tip<integers>>;
  using policy = storage_policy<registry>;
  using array = sort_profile_array<policy>;
  using native = mapped_sort_profile<policy>;
  using node = mapped_sort_cola<policy>;
  using native_ptr = std::shared_ptr<native const>;
  using node_ptr = node::pair_type;
  using string_map = std::map<std::string, std::optional<std::string>>;
  using integer_map = std::map<std::uint64_t, std::uint64_t>;

  std::filesystem::path root;
  unsigned serial = 0;
  void write(std::filesystem::path const & path, std::span<std::byte const> data) {
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<char const *>(data.data()), data.size());
    if (!out) throw std::runtime_error("writing fixture failed");
  }
  object_id identity() {
    char text[33]; std::snprintf(text, sizeof(text), "%032x", ++serial); return object_id(text);
  }
  array build(string_map const & s, integer_map const & u) {
    sort_profile_writer<policy> writer;
    for (auto const & [key, value] : s) writer.append<strings>(key, value);
    for (auto const & [key, value] : u) writer.append<integers>(key, value);
    return writer.finish();
  }
  std::pair<native_ptr, object_id> save(array const & a) {
    auto id = identity(); auto path = root / (id.hex() + ".kv");
    write(path, encoded_sort_sections<policy>::from(a).materialize());
    return {std::make_shared<native const>(native::open(path)), id};
  }
  node_ptr index(native_ptr a, object_id id, node_ptr main = {}, native_ptr side = {}, std::optional<object_id> side_id = {}) {
    using builder = cola_index<policy, native, node>;
    auto built = builder::adopt_native(a, main, side);
    auto index_id = identity(); auto path = root / (index_id.hex() + ".index");
    auto encoded = encode_cola_sections(built, id, main ? std::optional(main->identity()) : std::nullopt, side_id);
    write(path, encoded.materialize());
    auto mapped = std::make_shared<mapped_cola_index<policy> const>(mapped_cola_index<policy>::open(path));
    return node::bind({id, index_id}, a, mapped, main, side, side_id);
  }
  node_ptr bounded(node_ptr n) {
    auto [empty, id] = save(build({}, {}));
    while (n->virtual_size() > policy::group_size) n = index(empty, id, n);
    return n;
  }
  template <class S> auto matches(node_ptr n, typename sort_codec<S>::key_codec::value_type const & key) {
    auto query = sort_profile_query<policy, S>(key);
    auto cursor = cola_query_root<policy, node>::adopt_prepared(n).cursor(query.view());
    std::vector<typename sort_codec<S>::value_codec::value_type> result;
    while (!cursor.done()) {
      cursor.step(1);
      while (cursor.has_match()) {
        auto item = cursor.take_match(); sort_bit_reader in(item.value.view());
        result.push_back(sort_codec<S>::value_codec::read(in)); check(in.empty(), "outer value framing leaked");
      }
    }
    return result;
  }
  void framing() {
    string_map s{{"", "empty"}, {"a", "a"}, {std::string("a\0", 2), "zero"}, {"aa", "longer"}};
    integer_map u{{0, 9}, {1, 10}, {std::numeric_limits<std::uint64_t>::max(), 11}};
    auto a = build(s, u); a.view().scan();
    check(a.view().dictionary_size() == 2 && a.seeds().bit_size == 1, "shared selector seed shape");
    auto [mapped, id] = save(a); auto searchable = bounded(index(mapped, id));
    for (auto const & [key, value] : s)
      check(matches<strings>(searchable, key) == std::vector{value}, "proper-prefix mapped query");
    check(matches<strings>(searchable, "a1").empty(), "proper-prefix mapped miss");
    auto cursor = a.view().cursor();
    for (auto const & [key, value] : s) {
      auto expected = sort_profile_query<policy, strings>(key);
      auto item = cursor.peek(); check(!compare_bits(item.key.prefix, expected.view()), "proper-prefix string ordering");
      sort_bit_reader in(item.value); check(strings::value_codec::read(in) == value && in.empty(), "single value grammar");
      cursor.advance();
    }
    for (auto const & [key, value] : u) {
      auto expected = sort_profile_query<policy, integers>(key);
      auto item = cursor.peek(); check(!compare_bits(item.key.prefix, expected.view()), "integer order bits");
      check(item.value.size() == 64, "fixed value width changed");
      sort_bit_reader in(item.value); check(integers::value_codec::read(in) == value, "integer value"); cursor.advance();
    }
    check(cursor.done(), "cursor has trailing record");
    auto integer_only = build({}, {{0, 1}, {1, 2}});
    auto first = integer_only.view().encoded_at(0), second = integer_only.view().encoded_at(1);
    check(second.next_offset - first.next_offset == 129, "raw integer acquired key lengths or a backspace64 code");
    check(integer_only.seeds().bit_size == 0, "one sort paid per-block seed bits");
    auto string_only = build({{"a", "x"}, {"ab", "y"}}, {});
    auto sf = string_only.view().encoded_at(0), ss = string_only.view().encoded_at(1);
    // expG(0) + expG(8) + eight suffix bits + tag + expG(1) + eight value bits.
    check(ss.next_offset - sf.next_offset == 1 + 7 + 8 + 1 + 3 + 8, "duplicate tree/key backspace or outer value length");
  }
  void cascade_and_merge() {
    string_map a_s, b_s; integer_map a_u, b_u;
    for (unsigned i = 0; i != 180; ++i) {
      auto key = std::string(257, 'p') + "/" + std::to_string(1000 + i);
      if (i % 2 == 0) { a_s[key] = "old" + std::to_string(i); a_u[i] = i + 100; }
      if (i % 3 == 0) { b_s[key] = i % 9 ? std::optional("new" + std::to_string(i)) : std::nullopt; b_u[i] = i + 200; }
    }
    auto old = build(a_s, a_u), fresh = build(b_s, b_u);
    auto [am, aid] = save(old); auto [bm, bid] = save(fresh);
    auto old_node = index(am, aid);
    auto current = bounded(index(bm, bid, old_node));
    auto side = bounded(index(bm, bid, old_node, am, aid));
    am->scan(); bm->scan();
    std::uint64_t false_flags = 0, recovery_cases = 0;
    for (auto n = current; n; n = n->main_target()) {
      auto view = n->view();
      for (std::uint64_t group = 0; group != view.group_count(); ++group) {
        auto window = view.project(group);
        for (unsigned route = 0; route != 2; ++route)
          for (auto j = window.borrowed_first[route]; j != window.borrowed_last[route]; ++j)
            if (view.false_borrow(route, j)) {
              ++false_flags;
              auto borrowed = view.borrowed(route).reconstruct_at(j);
              auto cursor = view.native().cursor();
              while (!cursor.done()) {
                auto item = cursor.peek();
                if (!compare_bits(item.key.prefix, borrowed.prefix.view())) {
                  recovery_cases += item.ordinal < window.native_first; break;
                }
                cursor.advance();
              }
            }
      }
    }
    check(false_flags != 0 && recovery_cases != 0, "fixture missed false-borrow predecessor recovery");
    for (unsigned i = 0; i != 181; ++i) {
      auto key = std::string(257, 'p') + "/" + std::to_string(1000 + i);
      auto sm = matches<strings>(current, key);
      std::vector<std::optional<std::string>> expected;
      if (b_s.contains(key)) expected.push_back(b_s.at(key));
      if (a_s.contains(key)) expected.push_back(a_s.at(key));
      check(sm == expected, "mixed cascade string hit/miss or chronology");
      auto um = matches<integers>(current, i); std::vector<std::uint64_t> ue;
      if (b_u.contains(i)) ue.push_back(b_u.at(i));
      if (a_u.contains(i)) ue.push_back(a_u.at(i));
      check(um == ue, "mixed cascade integer hit/miss");
      auto secondary = matches<integers>(side, i);
      check(secondary.size() == ue.size() + a_u.contains(i), "terminal secondary lost a hit");
    }
    sort_profile_merge_builder<policy, native> merge(am, bm);
    while (!merge.done()) merge.step(7);
    check(merge.materialized_keys() == 0, "replacement merge reconstructed whole keys");
    auto result = merge.finish(); result.view().scan();
    for (auto const & [key, value] : b_s) a_s[key] = value;
    for (auto const & [key, value] : b_u) a_u[key] = value;
    auto [rm, rid] = save(result); auto merged = bounded(index(rm, rid));
    for (auto const & [key, value] : a_s) check(matches<strings>(merged, key) == std::vector{value}, "merged string result");
    for (auto const & [key, value] : a_u) check(matches<integers>(merged, key) == std::vector{value}, "merged integer result");
    struct compose {
      bit_view operator()(bit_view key, bit_view, bit_view newer) const { check(key.size() != 0, "missing compose key"); return newer; }
    };
    sort_profile_merge_builder<policy, native, compose> key_aware(am, bm);
    while (!key_aware.done()) key_aware.step(19);
    check(key_aware.materialized_keys() == 60, "key-aware merge materialized noncolliding keys");
    auto aware = key_aware.finish(); check(aware.data() == result.data(), "key-aware merge differs");
    // Exact physical owners outlive directory entries.
    std::filesystem::remove_all(root); std::filesystem::create_directory(root);
    check(matches<integers>(current, 30) == std::vector<std::uint64_t>{230, 130}, "mapping pin lost after unlink");
  }
  struct deep_selector {
    static inline unsigned calls = 0;
    template <class S, class Out> static void write(Out & out) {
      for (unsigned i = 0; i != 511; ++i) out.write_bits(0, 1);
      out.write_bits(std::same_as<S, integers>, 1);
    }
    template <class In, class F> static void select(In & in, F && visitor) {
      ++calls;
      for (unsigned i = 0; i != 511; ++i) if (in.read_bits(1)) throw std::invalid_argument("deep selector prefix");
      if (in.read_bits(1)) visitor(std::type_identity<integers>{}, in);
      else visitor(std::type_identity<strings>{}, in);
    }
  };
  void partial_bit_keys() {
    struct packed {
      using encoding = bit_encoding<fixed_values<3>>;
      using key_codec = fc_bit_key<exponential_golomb<2>>;
      using value_codec = unsigned_value<3>;
    };
    using r = bin<tip<strings>, bin<tip<integers>, tip<packed>>>;
    using p = storage_policy<r>;
    sort_profile_writer<p> writer;
    writer.append<strings>("a", "value"); writer.append<integers>(17, 42);
    std::vector<bit_string> keys;
    for (auto text : {"", "0", "00", "01", "1", "10", "101"}) keys.push_back(bit_string::from_bits(text));
    for (unsigned i = 0; i != keys.size(); ++i) writer.append<packed>(keys[i], i);
    auto a = std::make_shared<sort_profile_array<p> const>(writer.finish()); a->view().scan();
    using blob = cola_index<p, sort_profile_array<p>>;
    auto n = std::make_shared<blob const>(blob::adopt_native(a));
    auto root = cola_query_root<p, blob>::adopt_prepared(n);
    for (unsigned i = 0; i != keys.size(); ++i) {
      auto q = sort_profile_query<p, packed>(keys[i]); auto cursor = root.cursor(q.view());
      cursor.step(); check(cursor.has_match(), "partial-bit key query");
      auto hit = cursor.take_match(); sort_bit_reader value(hit.value.view());
      check(packed::value_codec::read(value) == i && value.empty(), "partial-bit value framing");
    }
  }
  void policy_and_corruption() {
    using alternate = storage_policy<registry, 3, golomb<3>, 16>;
    sort_profile_writer<alternate> writer;
    for (unsigned i = 0; i != 41; ++i) writer.append<strings>("p/" + std::to_string(100 + i), "value");
    auto a = writer.finish(); a.view().scan();
    auto query = sort_profile_query<alternate, strings>("p/124");
    auto context = profile_query_context<alternate>(query.view()).with_key(query.view());
    bool hit = false;
    a.view().compare_window(24, 27, context, [&](profile_comparison_item<alternate> item) {
      hit = !item.comparison.order(); return false;
    });
    check(hit, "alternate sampling/backspace policy");
    for (unsigned n = 0; n != 256; ++n) {
      auto data = a.data();
      auto at = (std::uint64_t(n) * 167 + 23) % data.bit_size;
      data.bytes[at >> 3] ^= std::byte(1u << (7 - (at & 7)));
      try {
        sort_profile_view<alternate> mutated(data.view(), a.group_offsets().view(), a.metadata(),
          a.dictionary().view(), word_view(a.dictionary_offsets()), a.seeds().view());
        mutated.scan();
      } catch (std::exception const &) {}
    }
  }
  void shared_selector_seed() {
    sort_profile_writer<policy, deep_selector> writer;
    for (unsigned i = 0; i != 61; ++i) writer.append<strings>("same-prefix/" + std::to_string(100 + i), "value");
    auto a = writer.finish();
    check(a.dictionary().bit_size == 512 && a.seeds().bit_size == 0, "deep selector repeated per block");
    deep_selector::calls = 0;
    auto cursor = a.view().cursor(); while (!cursor.done()) cursor.advance();
    check(deep_selector::calls == 1, "selector redispatched inside same-sort run");
    auto source = std::make_shared<sort_profile_array<policy, deep_selector> const>(std::move(a));
    sort_profile_merge_builder<policy, sort_profile_array<policy, deep_selector>> merge(source, source);
    while (!merge.done()) merge.step(9);
    auto merged = merge.finish(); merged.view().scan();
    check(merged.dictionary().bit_size == 512 && merge.materialized_keys() == 0, "merge lost custom selector");
  }
  void no_inherited_key_reads() {
#if defined(__unix__) || defined(__APPLE__)
    auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    auto prefix = std::string(6 * page, 'p');
    sort_profile_writer<policy> writer;
    for (unsigned i = 0; i != 45; ++i) writer.append<strings>(prefix + std::to_string(100 + i), "v");
    auto a = writer.finish();
    auto query = sort_profile_query<policy, strings>(prefix + "115");
    auto context = profile_query_context<policy>(query.view()).with_key(query.view());
    auto begin = reinterpret_cast<std::uintptr_t>(a.data().bytes.data());
    auto first = (begin + 2 * page - 1) & ~std::uintptr_t(page - 1);
    auto last = (begin + prefix.size() - page) & ~std::uintptr_t(page - 1);
    check(first < last && !::mprotect(reinterpret_cast<void *>(first), last - first, PROT_NONE), "protect inherited prefix");
    bool found = false;
    try {
      a.view().compare_window(15, 30, context, [&](profile_comparison_item<policy> item) {
        found = !item.comparison.order(); return false;
      });
    } catch (...) { ::mprotect(reinterpret_cast<void *>(first), last - first, PROT_READ | PROT_WRITE); throw; }
    check(!::mprotect(reinterpret_cast<void *>(first), last - first, PROT_READ | PROT_WRITE), "restore inherited prefix");
    check(found, "comparison-only window lost inherited key");
#endif
  }
  void metadata_only() {
    auto a = build({{"key", std::string(100'000, 'x')}}, {});
    auto encoded = encoded_sort_sections<policy>::from(a);
    auto moved = std::move(encoded);
    rejects([&] { (void)encoded.chunks(); });
    auto bytes = moved.materialize();
    auto path = root / "protected.kv"; write(path, bytes);
#if defined(__unix__) || defined(__APPLE__)
    auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    auto displacement = page - file_detail::header_bytes - sort_profile_file_detail::directory_bytes;
    std::vector<std::byte> physical(displacement, std::byte{0xa5});
    physical.insert(physical.end(), bytes.begin(), bytes.end());
    auto guarded_path = root / "guarded.kv"; write(guarded_path, physical);
    auto mapped = mapped_file::open(guarded_path); auto whole = mapped.slice(0, mapped.size());
    auto slice = mapped.slice(displacement, bytes.size());
    auto base = const_cast<std::byte *>(whole.bytes().data());
    auto protected_size = ((whole.size() + page - 1) / page - 1) * page;
    check(!::mprotect(base + page, protected_size, PROT_NONE), "protect payload");
    try {
      auto view = native::from_slice(slice);
      check(view.size() == 1, "metadata-only count");
    } catch (...) { ::mprotect(base + page, protected_size, PROT_READ); throw; }
    check(!::mprotect(base + page, protected_size, PROT_READ), "restore payload");
#endif
    auto m = native::open(path); m.scan();
    if constexpr (posix_object_ops::supported) {
      auto id = identity();
      auto receipt = encoded_sort_sections<policy>::from(a).seal(root, id, object_attempt_id(identity().hex()));
      auto sealed = native::open(receipt.path); sealed.scan();
      check(sealed.size() == 1 && receipt.bytes == bytes.size(), "sealed sort profile receipt");
    }
    auto damaged = bytes; damaged[96] = std::byte('X');
    auto damaged_path = root / "bad.kv"; write(damaged_path, damaged);
    rejects([&] { (void)native::open(damaged_path); });
  }
}
int main() try {
  root = std::filesystem::temp_directory_path() / ("diet-sort-profile-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  framing(); partial_bit_keys(); policy_and_corruption(); shared_selector_seed(); cascade_and_merge(); no_inherited_key_reads(); metadata_only(); std::filesystem::remove_all(root);
  std::cout << "sort profiles: framing, mmap queries, merge spans and protected-payload opening passed\n";
} catch (std::exception const & e) { std::cerr << e.what() << '\n'; if (!root.empty()) std::filesystem::remove_all(root); return 1; }
