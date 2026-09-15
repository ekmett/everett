/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

// A costed transition model, not a file scheduler or a bound on this test's
// execution time. Reference set unions and graph walks execute atomically;
// explicit counters model their record/index/metadata service obligations.
namespace cola_schedule_model {
  using number = std::uint64_t;
  using identity = std::size_t;

  inline void require(bool condition, char const * message) {
    if (!condition) throw std::logic_error(message);
  }
  inline number ceil_div(number n, number d) { return n / d + (n % d != 0); }

  struct native {
    number first = 0, last = 0;
    std::vector<number> keys;
  };
  struct targets { identity main = 0, secondary = 0; };
  // Main nodes represent exact native/index pairs. Secondary nodes are only
  // model handles for native files; they have no physical index object.
  struct node {
    identity data = 0;
    targets next;
    number augmented = 0;
    unsigned level = 0;
    bool secondary = false;
  };
  enum class state { empty, active, consumed, reserved, carrier_building, carrier_ready, root_carrier };
  enum class phase { native_merge, destination_index, carrier };
  struct slot {
    state status = state::empty;
    identity object = 0;
    targets route;
  };
  struct job {
    std::array<unsigned, 2> inputs{};
    unsigned destination = 0, carrier_slot = 0;
    targets destination_route;
    identity existing_main = 0, output_native = 0, output_node = 0;
    bool new_main = false;
    phase stage = phase::native_merge;
    number remaining = 0, total = 0;
    number visibility_allowance = 0;
  };
  struct level {
    std::array<slot, 3> slots{};
    std::optional<job> work;
    identity last_destination = 0;
  };
  struct counters {
    number native_work = 0, index_work = 0, carrier_work = 0;
    number immediate_work = 0, visibility_work = 0;
    number merges = 0, hidden_inputs = 0, delayed_carriers = 0;
    number hidden_completed_merges = 0, visibility_replacements = 0;
    number max_levels = 1, max_job_ratio = 0;
  };
  struct snapshot { targets root; number admitted = 0; };
  enum class event_kind { publication, reservation, local_commit };
  struct event {
    event_kind kind;
    unsigned level;
    identity object;
    snapshot view;
  };

  struct scheduler {
    explicit scheduler(number sample_interval, number physical_interval = 15)
      : k_(sample_interval), w_(physical_interval) {
      require(k_ >= 3 && w_ != 0, "invalid model policy");
      natives_.emplace_back(); nodes_.emplace_back(); seen_.push_back(false);
      levels_.emplace_back();
    }
    number size() const noexcept { return admitted_; }
    number k() const noexcept { return k_; }
    number w() const noexcept { return w_; }
    snapshot root() const noexcept { return {root_, admitted_}; }
    counters const & counts() const noexcept { return counts_; }
    std::vector<level> const & levels() const noexcept { return levels_; }
    std::vector<node> const & nodes() const noexcept { return nodes_; }
    std::vector<native> const & natives() const noexcept { return natives_; }
    std::vector<event> const & events() const noexcept { return events_; }
    identity save_snapshot() {
      saved_.push_back(root());
      return saved_.size();
    }
    snapshot saved_snapshot(identity owner) const {
      require(owner != 0 && owner <= saved_.size() && saved_[owner - 1].has_value(), "missing snapshot owner");
      return *saved_[owner - 1];
    }
    void drop_snapshot(identity owner) {
      (void)saved_snapshot(owner);
      saved_[owner - 1].reset();
    }
    // The test archive never erases immutable objects. This union models
    // which exact main pairs / native leaves remain owned, independently of
    // archive allocation. A secondary wrapper is not a stored .index file.
    std::vector<identity> retained_node_ids() const {
      std::vector<identity> result;
      auto add = [&](auto && self, identity id) -> void {
        if (!id || std::find(result.begin(), result.end(), id) != result.end()) return;
        result.push_back(id);
        self(self, nodes_[id].next.main);
        self(self, nodes_[id].next.secondary);
      };
      auto add_targets = [&](targets t) { add(add, t.main); add(add, t.secondary); };
      add_targets(root_);
      for (auto const & old : saved_) if (old) add_targets(old->root);
      for (auto const & l : levels_) for (auto const & s : l.slots) {
        add(add, s.object);
        add_targets(s.route);
      }
      std::sort(result.begin(), result.end());
      return result;
    }
    bool was_visible(identity id) const { return seen_.at(id); }
    bool is_visible(identity id) const {
      auto ids = reachable();
      return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
    bool unsafe(unsigned i) const {
      return levels_.at(i).work.has_value() || active_slots(i).size() == 2;
    }
    bool pending() const {
      for (unsigned i = 0; i < levels_.size(); ++i) if (unsafe(i)) return true;
      return false;
    }
    static number service_budget(number admitted) {
      unsigned h = 1;
      for (number n = admitted; n > 1; n >>= 1) ++h;
      // Job records/index events <=160*2^r, plus separately charged root
      // visibility and <=128 immediate admission events. This deliberately
      // generous service constant is derived in docs/cola-scheduling.md.
      return 512 * (h + 2);
    }

    number admit_one(number key) {
      require(admitted_ < (number{1} << 30), "model admission limit");
      require(!levels_[0].work && active_slots(0).size() < 2, "level zero needs service");
      native n{admitted_, admitted_ + 1, {key}};
      natives_.push_back(std::move(n));
      auto data = natives_.size() - 1;
      auto active = active_slots(0);
      unsigned position = 0;
      targets route;
      bool secondary = !active.empty();
      if (secondary) {
        position = empty_slot(0);
      } else {
        auto carrier = find_state(0, state::root_carrier);
        position = carrier ? *carrier : empty_slot(0);
        if (carrier) route = levels_[0].slots[position].route;
      }
      auto id = make_node(data, route, 0, secondary);
      levels_[0].slots[position] = {state::active, id, {}};
      if (secondary) root_.secondary = id;
      else root_ = {id, 0};
      ++admitted_;
      auto visibility = publish();
      auto cost = 1 + index_cost(1, route) + visibility;
      counts_.immediate_work += cost;
      return cost;
    }

    number advance(number budget) {
      number used = 0;
      while (used < budget) {
        std::optional<unsigned> selected;
        for (unsigned i = 0; i < levels_.size(); ++i) {
          if (unsafe(i)) { selected = i; break; }
        }
        if (!selected) break;
        auto i = *selected;
        if (!levels_[i].work) begin_job(i);
        auto & work = *levels_[i].work;
        auto amount = std::min(budget - used, work.remaining);
        require(amount != 0, "empty work stage");
        work.remaining -= amount;
        work.total += amount;
        used += amount;
        switch (work.stage) {
          case phase::native_merge: counts_.native_work += amount; break;
          case phase::destination_index:
            if (work.new_main) counts_.index_work += amount;
            else counts_.native_work += amount; // Terminal native EF completion.
            break;
          case phase::carrier: counts_.carrier_work += amount; break;
        }
        if (work.remaining == 0) finish_stage(i);
      }
      return used;
    }

    void insert(number key) {
      auto budget = service_budget(admitted_ + 1);
      auto immediate = admit_one(key);
      require(immediate <= budget, "admission exceeded service");
      advance(budget - immediate);
    }

  private:
    number k_, w_, admitted_ = 0;
    targets root_;
    std::vector<native> natives_;
    std::vector<node> nodes_;
    std::vector<bool> seen_;
    std::vector<level> levels_;
    std::vector<std::optional<snapshot>> saved_;
    std::vector<event> events_;
    counters counts_;

    std::vector<unsigned> active_slots(unsigned i) const {
      std::vector<unsigned> result;
      for (unsigned s = 0; s != 3; ++s)
        if (levels_[i].slots[s].status == state::active) result.push_back(s);
      std::sort(result.begin(), result.end(), [&](unsigned a, unsigned b) {
        return natives_[nodes_[levels_[i].slots[a].object].data].first <
          natives_[nodes_[levels_[i].slots[b].object].data].first;
      });
      return result;
    }
    std::optional<unsigned> find_state(unsigned i, state wanted) const {
      for (unsigned s = 0; s != 3; ++s)
        if (levels_[i].slots[s].status == wanted) return s;
      return std::nullopt;
    }
    unsigned empty_slot(unsigned i) const {
      auto result = find_state(i, state::empty);
      require(result.has_value(), "no free logical shadow slot");
      return *result;
    }
    number augmented(identity id) const { return id ? nodes_[id].augmented : 0; }
    number borrowed(targets t) const {
      return ceil_div(augmented(t.main), k_) + ceil_div(augmented(t.secondary), k_);
    }
    number index_cost(number native_count, targets t) const {
      auto a = ceil_div(augmented(t.main), k_);
      auto b = ceil_div(augmented(t.secondary), k_);
      auto total = native_count + a + b;
      // Target scanning; three-way merge; two classes/two cuts/one grouped
      // prefix event; sampled offsets plus EOF in each of the three streams.
      return augmented(t.main) + augmented(t.secondary) + total +
        5 * ceil_div(total, k_) + ceil_div(native_count, w_) +
        ceil_div(a, w_) + ceil_div(b, w_) + 3;
    }
    identity make_node(identity data, targets t, unsigned i, bool secondary) {
      require(!secondary || (t.main == 0 && t.secondary == 0), "secondary must be terminal");
      require(!t.secondary || t.main, "secondary target without main");
      if (t.main) require(nodes_[t.main].level == i + 1, "main target skips a level");
      if (t.secondary) require(nodes_[t.secondary].level == i + 1 &&
        nodes_[t.secondary].secondary, "invalid secondary target");
      nodes_.push_back({data, t, natives_[data].keys.size() + borrowed(t), i, secondary});
      seen_.push_back(false);
      return nodes_.size() - 1;
    }
    void append_reachable(identity id, std::vector<identity> & ids) const {
      if (!id) return;
      require(std::find(ids.begin(), ids.end(), id) == ids.end(), "repeated current pair");
      ids.push_back(id);
      auto p = nodes_[id];
      append_reachable(p.next.main, ids);
      append_reachable(p.next.secondary, ids);
    }
    std::vector<identity> reachable() const {
      std::vector<identity> result;
      append_reachable(root_.main, result);
      append_reachable(root_.secondary, result);
      return result;
    }
    number publish() {
      auto current = reachable();
      events_.push_back({event_kind::publication, 0, 0, root()});
      for (auto id : current) seen_[id] = true;
      for (auto & l : levels_) for (auto & s : l.slots) {
        if (s.status == state::consumed && seen_[s.object] &&
            std::find(current.begin(), current.end(), s.object) == current.end()) {
          s = {};
          ++counts_.visibility_replacements;
        }
      }
      // A production implementation can maintain roles incrementally; this
      // reference walk also checks the exact immutable graph.
      auto cost = 2 * levels_.size() + 4;
      counts_.visibility_work += cost;
      return cost;
    }
    void begin_job(unsigned i) {
      auto inputs = active_slots(i);
      require(inputs.size() == 2, "merge without two runs");
      if (i + 1 == levels_.size()) levels_.emplace_back();
      require(!unsafe(i + 1), "adjacent unsafe levels at destination reservation");
      auto previous = levels_[i + 1].last_destination;
      require(!previous || seen_[previous], "previous destination not visible before reuse");
      events_.push_back({event_kind::reservation, i + 1, previous, {}});
      auto carrier = find_state(i + 1, state::carrier_ready);
      auto dest = carrier ? *carrier : empty_slot(i + 1);
      auto active = active_slots(i + 1);
      require(active.size() <= 1, "destination occupancy");
      require(!carrier || active.empty(), "carrier with unmerged destination run");
      auto first_id = levels_[i].slots[inputs[0]].object;
      auto second_id = levels_[i].slots[inputs[1]].object;
      auto const & a = natives_[nodes_[first_id].data];
      auto const & b = natives_[nodes_[second_id].data];
      require(a.last == b.first && a.last - a.first == (number{1} << i) &&
        b.last - b.first == (number{1} << i), "nonadjacent native history merge");
      native output{a.first, b.last, {}};
      std::set_union(a.keys.begin(), a.keys.end(), b.keys.begin(), b.keys.end(),
        std::back_inserter(output.keys));
      auto native_cost = a.keys.size() + b.keys.size() + output.keys.size() + 1;
      natives_.push_back(std::move(output));
      job work;
      work.inputs = {inputs[0], inputs[1]};
      work.destination = dest;
      work.carrier_slot = empty_slot(i);
      work.destination_route = carrier ? levels_[i + 1].slots[dest].route : targets{};
      work.new_main = carrier.has_value() || active.empty();
      work.existing_main = active.empty() ? 0 : levels_[i + 1].slots[active[0]].object;
      work.output_native = natives_.size() - 1;
      work.remaining = native_cost;
      work.visibility_allowance = i == 0 ? 2 * levels_.size() + 4 : 0;
      if (!is_visible(first_id)) ++counts_.hidden_inputs;
      if (!is_visible(second_id)) ++counts_.hidden_inputs;
      levels_[i].slots[work.carrier_slot] = {state::carrier_building, 0, {}};
      levels_[i + 1].slots[dest].status = state::reserved;
      levels_[i].work = work;
      counts_.max_levels = std::max<number>(counts_.max_levels, levels_.size());
    }
    void finish_stage(unsigned i) {
      auto & work = *levels_[i].work;
      if (work.stage == phase::native_merge) {
        work.stage = phase::destination_index;
        work.remaining = work.new_main
          ? index_cost(natives_[work.output_native].keys.size(), work.destination_route)
          : ceil_div(natives_[work.output_native].keys.size(), w_) + 1;
        return;
      }
      if (work.stage == phase::destination_index) {
        work.output_node = make_node(work.output_native, work.destination_route, i + 1, !work.new_main);
        levels_[i + 1].slots[work.destination].object = work.output_node;
        auto t = work.new_main ? targets{work.output_node, 0} : targets{work.existing_main, work.output_node};
        levels_[i].slots[work.carrier_slot].route = t;
        work.stage = phase::carrier;
        work.remaining = index_cost(0, t) + work.visibility_allowance;
        return;
      }
      auto complete = work;
      require(complete.total - complete.visibility_allowance <= 160 * (number{1} << i),
        "derived local work bound exceeded");
      counts_.max_job_ratio = std::max(counts_.max_job_ratio,
        ceil_div(complete.total - complete.visibility_allowance, number{1} << i));
      for (auto input : complete.inputs) levels_[i].slots[input].status = state::consumed;
      auto t = levels_[i].slots[complete.carrier_slot].route;
      levels_[i].slots[complete.carrier_slot].status = state::carrier_ready;
      levels_[i + 1].slots[complete.destination] = {state::active, complete.output_node, {}};
      levels_[i + 1].last_destination = complete.output_node;
      events_.push_back({event_kind::local_commit, i + 1, complete.output_node, {}});
      levels_[i].work.reset();
      ++counts_.merges;
      if (i == 0) {
        auto id = make_node(0, t, 0, false);
        levels_[0].slots[complete.carrier_slot] = {state::root_carrier, id, t};
        root_ = {id, 0};
        auto cost = publish();
        require(cost <= complete.visibility_allowance, "visibility allowance exceeded");
      } else {
        ++counts_.delayed_carriers;
        if (!is_visible(complete.output_node)) ++counts_.hidden_completed_merges;
      }
    }
  };
}
