/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Encodes the full redundant frontier, including hidden completed artifacts and restart recipes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/redundant_runtime.h>
#include <diet/runtime_checkpoint.h>

namespace diet {
  template <class P, class Storage> struct runtime_storage_codec<redundant_runtime_family<P, Storage>> {
    using family_type = redundant_runtime_family<P, Storage>;
    using snapshot_type = typename family_type::snapshot_type;
    using frontier_type = typename family_type::frontier_type;
    using object_type = typename family_type::object_type;
    using routes_type = typename family_type::routes_type;
    using object_pointer = typename snapshot_type::object_pointer;
    using pair_type = typename family_type::node_type::pair_type;
    using native_pointer = typename family_type::node_type::native_pointer;
    inline static constexpr std::array<std::byte, 8> magic{
      std::byte{'D'}, std::byte{'I'}, std::byte{'E'}, std::byte{'T'},
      std::byte{'R'}, std::byte{'F'}, std::byte{0}, std::byte{1}};

    // Children precede their parents. Decoding can reject forward references
    // before constructing a cycle of owning shared pointers.
    static std::vector<object_pointer> objects(frontier_type const & frontier) {
      std::vector<object_pointer> result;
      std::unordered_map<std::uint64_t, object_pointer> seen;
      std::unordered_set<std::uint64_t> active;
      auto visit = [&](auto && self, object_pointer const & value) -> void {
        if (!value) return;
        if (auto found = seen.find(value->identity); found != seen.end()) {
          if (found->second != value) throw std::invalid_argument("duplicate checkpoint object identity");
          return;
        }
        if (!value->identity || !active.insert(value->identity).second)
          throw std::invalid_argument("cyclic checkpoint object routes");
        self(self, value->next.main); self(self, value->next.secondary);
        active.erase(value->identity); seen.emplace(value->identity, value); result.push_back(value);
      };
      auto routes = [&](auto const & value) { visit(visit, value.main); visit(visit, value.secondary); };
      routes(frontier.root);
      for (auto const & level : frontier.levels) {
        for (auto const & slot : level.slots) { visit(visit, slot.object); routes(slot.route); }
        if (level.job) {
          auto const & job = *level.job;
          routes(job.destination_route); visit(visit, job.existing_main); visit(visit, job.output);
        }
      }
      return result;
    }
    // A validated snapshot owns every object in exactly one level slot.
    // Its routes point to the next level; job outputs occupy destination slots.
    // Descending levels therefore visit children before parents without a
    // closure allocation. Raw frontiers still use the defensive walk above.
    template <class F> static void for_each_object(snapshot_type const & source, F && visit) {
      auto const & levels = source.frontier().levels;
      for (auto level = levels.rbegin(); level != levels.rend(); ++level)
        for (auto const & slot : level->slots) if (slot.object) visit(slot.object);
    }
    static std::size_t object_count(snapshot_type const & source) noexcept {
      std::size_t count = 0;
      for_each_object(source, [&](auto const &) { ++count; });
      return count;
    }
    template <class Pair, class Native> static void collect(snapshot_type const & source, Pair pair, Native native) {
      pair(source.query_root().head());
      for_each_object(source, [&](auto const & object) { pair(object->pair); native(object->native); });
      for (auto const & level : source.frontier().levels) {
        for (auto const & slot : level.slots) pair(slot.carrier);
        if (level.job) { pair(level.job->carrier); native(level.job->merged); }
      }
    }
    template <class Pair, class Native> static std::vector<std::byte> encode(snapshot_type const & source,
        std::span<std::byte const> semantic, Pair pair_id, Native native_id) {
      std::vector<std::byte> out(magic.begin(), magic.end());
      auto number = [&](std::uint64_t value) { catalog_detail::number(out, value); };
      auto object = [&](object_pointer const & value) { number(value ? value->identity : 0); };
      auto routes = [&](auto const & value) { object(value.main); object(value.secondary); };
      auto pair = [&](pair_type const & value) { number(bool(value)); if (value) catalog_detail::pair(out, pair_id(value)); };
      auto native = [&](native_pointer const & value) { number(bool(value)); if (value) catalog_detail::identity(out, native_id(value)); };
      auto const & f = source.frontier();
      number(f.admissions); number(f.next_identity); number(f.service_due);
      number(object_count(source));
      for_each_object(source, [&](auto const & value) {
        number(value->identity); number(value->first); number(value->last); number(value->level);
        native(value->native); pair(value->pair); routes(value->next);
      });
      routes(f.root); number(f.levels.size());
      for (auto const & level : f.levels) {
        number(level.last_destination); number(level.last_destination_visible);
        for (auto const & slot : level.slots) {
          number(static_cast<unsigned>(slot.state)); object(slot.object); routes(slot.route);
          pair(slot.carrier); number(slot.ever_visible);
        }
        number(bool(level.job));
        if (level.job) {
          auto const & job = *level.job;
          for (auto input : job.inputs) number(input);
          number(job.destination); number(job.carrier_slot); number(job.new_main);
          routes(job.destination_route); object(job.existing_main); object(job.output);
          native(job.merged); pair(job.carrier); number(static_cast<unsigned>(job.stage));
        }
      }
      catalog_detail::binary(out, semantic); return out;
    }
    template <class Resolver> static decoded_runtime_checkpoint<snapshot_type> decode(
        std::span<std::byte const> encoded, blob_identity const & head, Resolver & resolver) {
      if (encoded.size() < magic.size() || !std::equal(magic.begin(), magic.end(), encoded.begin()))
        throw std::invalid_argument("unsupported redundant Diet checkpoint");
      catalog_detail::outcome_reader input{encoded.subspan(magic.size())};
      auto small = [&](unsigned maximum) {
        auto n = input.number();
        if (n > maximum) throw std::invalid_argument("invalid redundant checkpoint field");
        return static_cast<unsigned>(n);
      };
      auto flag = [&] { return small(1) != 0; };
      auto pair = [&]() -> pair_type {
        if (!flag()) return {};
        auto native = input.field(), index = input.field();
        return resolver.pair({object_id(native), object_id(index)});
      };
      auto native = [&]() -> native_pointer { return flag() ? resolver.native(object_id(input.field())) : native_pointer{}; };
      std::unordered_map<std::uint64_t, object_pointer> objects;
      auto object = [&]() -> object_pointer {
        auto id = input.number();
        if (!id) return {};
        auto found = objects.find(id);
        if (found == objects.end()) throw std::invalid_argument("checkpoint route is not a preceding object");
        return found->second;
      };
      auto routes = [&] { auto main = object(), secondary = object(); return routes_type{std::move(main), std::move(secondary)}; };
      frontier_type f;
      f.admissions = input.number(); f.next_identity = input.number(); f.service_due = input.number();
      auto count = input.number();
      if (count > input.data.size() / 64) throw std::invalid_argument("invalid checkpoint object count");
      for (std::uint64_t i = 0; i != count; ++i) {
        object_type value;
        value.identity = input.number(); value.first = input.number(); value.last = input.number(); value.level = small(63);
        value.native = native(); value.pair = pair(); value.next = routes();
        auto id = value.identity;
        if (!id || !objects.emplace(id, std::make_shared<object_type const>(std::move(value))).second)
          throw std::invalid_argument("duplicate checkpoint object identity");
      }
      f.root = routes();
      auto levels = small(64); f.levels.resize(levels);
      for (auto & level : f.levels) {
        level.last_destination = input.number(); level.last_destination_visible = flag();
        for (auto & slot : level.slots) {
          slot.state = static_cast<redundant_slot_state>(small(static_cast<unsigned>(redundant_slot_state::root_carrier)));
          slot.object = object(); slot.route = routes(); slot.carrier = pair(); slot.ever_visible = flag();
        }
        if (flag()) {
          auto & job = level.job.emplace();
          for (auto & slot : job.inputs) slot = small(2);
          job.destination = small(2); job.carrier_slot = small(2); job.new_main = flag();
          job.destination_route = routes(); job.existing_main = object(); job.output = object();
          job.merged = native(); job.carrier = pair();
          job.stage = static_cast<redundant_stage>(small(static_cast<unsigned>(redundant_stage::commit)));
        }
      }
      auto size = input.number();
      if (size != input.data.size()) throw std::invalid_argument("invalid redundant semantic extent");
      std::vector<std::byte> semantic(input.data.begin(), input.data.end());
      auto restored = snapshot_type::restore(std::move(f), resolver.pair(head));
      if (runtime_storage_codec::object_count(restored) != objects.size())
        throw std::invalid_argument("unreachable redundant checkpoint object");
      return {std::move(restored), std::move(semantic)};
    }
  };
}
