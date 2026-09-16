/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks complete slot-ordered checkpoint traversal and defensive wire decoding.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/redundant_checkpoint.h>

#include <cassert>
#include <iostream>
#include <map>
#include <set>

namespace {
  using namespace everett;
  template <class F> void rejects(F && action, std::string_view fragment) {
    try { action(); }
    catch (std::invalid_argument const & error) {
      assert(std::string_view(error.what()).find(fragment) != std::string_view::npos); return;
    }
    throw std::runtime_error("expected checkpoint rejection");
  }
  object_id identity(std::uint64_t value) {
    std::string text(32, '0');
    for (unsigned i = 0; i != 16; ++i) { text[31 - i] = "0123456789abcdef"[value & 15]; value >>= 4; }
    return object_id(std::move(text));
  }
  template <class P> struct resolver {
    using family = redundant_runtime_family<P>;
    using native_pointer = typename family::node_type::native_pointer;
    using pair_type = typename family::node_type::pair_type;
    std::uint64_t next = 1;
    std::map<void const *, object_id> natives;
    std::map<void const *, blob_identity> pairs;
    std::map<std::string, native_pointer> native_owners;
    std::map<std::string, pair_type> pair_owners;
    object_id native_id(native_pointer const & value) {
      if (auto found = natives.find(value.get()); found != natives.end()) return found->second;
      auto id = identity(next++);
      natives.emplace(value.get(), id); native_owners.emplace(id.hex(), value); return id;
    }
    blob_identity pair_id(pair_type const & value) {
      if (auto found = pairs.find(value.get()); found != pairs.end()) return found->second;
      blob_identity id{native_id(value->native_owner()), identity(next++)};
      pairs.emplace(value.get(), id); pair_owners.emplace(id.index.hex(), value); return id;
    }
    native_pointer native(object_id const & id) { return native_owners.at(id.hex()); }
    pair_type pair(blob_identity const & id) {
      auto result = pair_owners.at(id.index.hex());
      assert(native_id(result->native_owner()) == id.native); return result;
    }
  };
  struct descriptor { std::uint64_t id; std::size_t first, last; };
  std::vector<descriptor> descriptors(std::span<std::byte const> data) {
    catalog_detail::outcome_reader input{data.subspan(8)};
    for (unsigned i = 0; i != 3; ++i) (void)input.number();
    auto count = input.number();
    std::vector<descriptor> result;
    for (std::uint64_t i = 0; i != count; ++i) {
      auto start = data.size() - input.data.size(); auto id = input.number();
      for (unsigned j = 0; j != 3; ++j) (void)input.number();
      if (input.number()) (void)input.field();
      if (input.number()) { (void)input.field(); (void)input.field(); }
      (void)input.number(); (void)input.number();
      result.push_back({id, start, data.size() - input.data.size()});
    }
    return result;
  }
  void rewrite(std::vector<std::byte> & data, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) data[offset + i] = std::byte(value >> (i << 3));
  }
  template <class P> struct fixture {
    using family = redundant_runtime_family<P>;
    using codec = runtime_storage_codec<family>;
    using snapshot_type = typename family::snapshot_type;
    using object_pointer = typename snapshot_type::object_pointer;
    resolver<P> ids;
    unsigned stages = 0, roles = 0;
    bool hidden_output = false, existing_main = false, legacy_differs = false, corrupted = false;
    std::vector<std::byte> encode(snapshot_type const & snapshot) {
      constexpr std::array semantic{std::byte{'a'}, std::byte{0}, std::byte{'b'}};
      return codec::encode(snapshot, semantic, [&](auto const & pair) { return ids.pair_id(pair); },
        [&](auto const & native) { return ids.native_id(native); });
    }
    void audit(snapshot_type const & snapshot) {
      auto const & frontier = snapshot.frontier();
      auto raw = codec::objects(frontier);
      std::set<void const *> expected, seen;
      for (auto const & object : raw) expected.insert(object.get());
      unsigned last_level = 64;
      std::size_t count = 0;
      codec::for_each_object(snapshot, [&](object_pointer const & object) {
        assert(object->level <= last_level); last_level = object->level;
        assert((!object->next.main || seen.contains(object->next.main.get())) &&
          (!object->next.secondary || seen.contains(object->next.secondary.get())));
        assert(seen.insert(object.get()).second); ++count;
      });
      assert(seen == expected && count == codec::object_count(snapshot));
      std::set<void const *> pairs, natives, wanted_pairs, wanted_natives;
      auto add_pair = [](auto & set, auto const & pair) { if (pair) set.insert(pair.get()); };
      add_pair(wanted_pairs, snapshot.query_root().head());
      for (auto const & object : raw) { add_pair(wanted_pairs, object->pair); add_pair(wanted_natives, object->native); }
      for (auto const & level : frontier.levels) {
        for (auto const & slot : level.slots) { roles |= 1u << unsigned(slot.state); add_pair(wanted_pairs, slot.carrier); }
        if (level.job) {
          auto const & job = *level.job; stages |= 1u << unsigned(job.stage);
          hidden_output |= bool(job.output); existing_main |= bool(job.existing_main);
          add_pair(wanted_pairs, job.carrier); add_pair(wanted_natives, job.merged);
        }
      }
      codec::collect(snapshot, [&](auto const & value) { add_pair(pairs, value); },
        [&](auto const & value) { add_pair(natives, value); });
      assert(pairs == wanted_pairs && natives == wanted_natives);
      auto head = ids.pair_id(snapshot.query_root().head());
      auto encoded = encode(snapshot); assert(encoded == encode(snapshot));
      auto decoded = codec::decode(encoded, head, ids);
      assert(decoded.semantic == std::vector<std::byte>({std::byte{'a'}, std::byte{0}, std::byte{'b'}}));
      assert(encode(decoded.snapshot) == encoded);
      auto ranges = descriptors(encoded);
      assert(ranges.size() == count);
      if (ranges.empty()) return;
      // Keep the old DFS descriptor order as a wire compatibility fixture;
      // descriptor contents and the later slot table remain byte-for-byte exact.
      std::vector<std::byte> legacy(encoded.begin(), encoded.begin() + ranges.front().first);
      for (auto const & object : raw) {
        auto found = std::find_if(ranges.begin(), ranges.end(), [&](auto const & range) { return range.id == object->identity; });
        assert(found != ranges.end());
        legacy.insert(legacy.end(), encoded.begin() + found->first, encoded.begin() + found->last);
      }
      legacy.insert(legacy.end(), encoded.begin() + ranges.back().last, encoded.end());
      legacy_differs |= legacy != encoded;
      auto previous = codec::decode(legacy, head, ids);
      assert(encode(previous.snapshot) == encoded);
      if (corrupted || count < 2) return;
      auto invalid = encoded;
      rewrite(invalid, ranges.front().last - 16, ranges.back().id);
      rejects([&] { (void)codec::decode(invalid, head, ids); }, "preceding object");
      invalid = encoded; rewrite(invalid, ranges[1].first, ranges[0].id);
      rejects([&] { (void)codec::decode(invalid, head, ids); }, "duplicate checkpoint object");
      invalid.assign(encoded.begin(), encoded.begin() + ranges.back().last);
      auto extra = invalid.size();
      invalid.insert(invalid.end(), encoded.begin() + ranges.front().first, encoded.begin() + ranges.front().last);
      rewrite(invalid, extra, frontier.next_identity);
      invalid.insert(invalid.end(), encoded.begin() + ranges.back().last, encoded.end());
      rewrite(invalid, 32, count + 1);
      rejects([&] { (void)codec::decode(invalid, head, ids); }, "unreachable redundant checkpoint object");
      auto bad_frontier = frontier;
      auto mutable_object = std::make_shared<typename family::object_type>(*raw.front());
      mutable_object->next.main = mutable_object;
      bad_frontier.root = {mutable_object, {}};
      rejects([&] { (void)codec::objects(bad_frontier); }, "cyclic checkpoint object");
      mutable_object->next.main.reset(); // Break the intentionally malformed owning cycle.
      bad_frontier.root = {raw.front(), std::make_shared<typename family::object_type const>(*raw.front())};
      rejects([&] { (void)codec::objects(bad_frontier); }, "duplicate checkpoint object identity");
      corrupted = true;
    }
  };
  profile_record row(unsigned n) {
    return {bit_string::from_bytes("key/" + std::to_string(n % 17)), bit_string::from_bytes(std::to_string(n))};
  }
  template <class P> void scenario() {
    redundant_runtime<P> active; fixture<P> test;
    test.audit(active.snapshot());
    auto step = [&] {
      auto cost = active.next_service_cost(), credit = active.credit();
      active.advance(cost > credit ? cost - credit : 1);
      test.audit(active.checkpoint());
    };
    for (unsigned n = 0; n != 128; ++n) {
      while (!active.admission_ready()) step();
      assert(active.try_contribute(row(n), 0));
      test.audit(active.checkpoint());
      if (active.pending()) step();
    }
    while (active.pending()) step();
    test.audit(active.checkpoint());
    assert(test.stages == 15 && test.hidden_output && test.existing_main && test.legacy_differs && test.corrupted);
    assert(test.roles & (1u << unsigned(redundant_slot_state::root_carrier)));
    assert(test.roles & (1u << unsigned(redundant_slot_state::reserved)));
    assert(test.roles & (1u << unsigned(redundant_slot_state::consumed)));
  }
}
int main() {
  try {
    scenario<storage_policy<tip<encoded_sort<bit_encoding<>>>, 3>>();
    scenario<storage_policy<tip<encoded_sort<bit_encoding<>>>, 15>>();
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
