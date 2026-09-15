/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include "cola_schedule_model.h"

#include <iostream>
#include <set>
#include <string>

namespace {
  using namespace cola_schedule_model;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::logic_error const &) { rejected = true; }
    check(rejected, "model accepted invalid transition");
  }
  struct interval { number first, last; identity native; };

  // Independent traversal: main descendants are older, then the leaf
  // secondary, then this smaller-level native interval. No model traversal
  // or visibility helper participates in this coverage oracle.
  void walk(scheduler const & s, identity id, std::set<identity> & seen,
      std::vector<interval> & history) {
    if (!id) return;
    check(id < s.nodes().size() && seen.insert(id).second, "repeated/missing exact query pair");
    auto p = s.nodes()[id];
    check(!p.secondary || (!p.next.main && !p.next.secondary), "recursive secondary");
    if (p.next.main) check(s.nodes()[p.next.main].level == p.level + 1, "main edge skips level");
    if (p.next.secondary) check(s.nodes()[p.next.secondary].level == p.level + 1 &&
      s.nodes()[p.next.secondary].secondary, "secondary edge role/level");
    walk(s, p.next.main, seen, history);
    walk(s, p.next.secondary, seen, history);
    auto const & n = s.natives().at(p.data);
    if (n.first != n.last) history.push_back({n.first, n.last, p.data});
  }
  std::vector<interval> coverage(scheduler const & s, snapshot root) {
    std::set<identity> seen;
    std::vector<interval> history;
    walk(s, root.root.main, seen, history);
    walk(s, root.root.secondary, seen, history);
    number next = 0;
    for (auto range : history) {
      check(range.first == next && range.last > range.first, "history gap, duplicate, or reversal");
      next = range.last;
    }
    check(next == root.admitted, "published root omitted an acknowledged admission");
    number augmented = 0, natives = 0, height = 0;
    for (auto id : seen) {
      auto const & n = s.nodes()[id];
      augmented += n.augmented;
      natives += s.natives()[n.data].keys.size();
      height = std::max(height, number(n.level) + 1);
    }
    if (height) check(augmented * (s.k() - 1) <= s.k() * natives +
      2 * (height - 1) * (s.k() - 1), "global two-target entry bound");
    return history;
  }
  void dictionary(scheduler const & s, snapshot root, std::vector<number> const & input) {
    auto history = coverage(s, root);
    for (number key : {number{0}, number{1}, number{2}, number{6}, number{31}, number{999999}}) {
      std::vector<number> expected, actual;
      for (number i = 0; i != root.admitted; ++i) if (input[i] == key) expected.push_back(i);
      for (auto range : history) {
        auto const & n = s.natives()[range.native];
        if (!std::binary_search(n.keys.begin(), n.keys.end(), key)) continue;
        // The values are noncommutative admission-token concatenations. The
        // count model represents their composition by a contiguous interval.
        for (auto i = range.first; i != range.last; ++i) if (input[i] == key) actual.push_back(i);
      }
      check(actual == expected, "per-key chronological composition differs");
    }
    for (auto range : history) {
      std::set<number> expected;
      for (auto i = range.first; i != range.last; ++i) expected.insert(input[i]);
      auto const & keys = s.natives()[range.native].keys;
      check(std::vector<number>(expected.begin(), expected.end()) == keys, "native key union differs");
    }
  }
  void visibility_history(scheduler const & s) {
    std::set<identity> observed;
    std::vector<identity> previous(s.levels().size());
    std::vector<bool> reserved(s.levels().size());
    for (auto const & e : s.events()) {
      if (e.kind == event_kind::publication) {
        std::set<identity> ids;
        std::vector<interval> intervals;
        walk(s, e.view.root.main, ids, intervals);
        walk(s, e.view.root.secondary, ids, intervals);
        number next = 0;
        for (auto span : intervals) {
          check(span.first == next, "intermediate publication changed history coverage");
          next = span.last;
        }
        check(next == e.view.admitted, "intermediate publication omitted history");
        observed.insert(ids.begin(), ids.end());
      } else if (e.kind == event_kind::reservation) {
        check(!reserved[e.level] && e.object == previous[e.level], "destination reservation history");
        check(e.object == 0 || observed.contains(e.object), "slot reused before independently observed visibility");
        reserved[e.level] = true;
      } else {
        check(reserved[e.level] && e.object != 0, "destination committed without reservation");
        previous[e.level] = e.object;
        reserved[e.level] = false;
      }
    }
    for (bool pending : reserved) check(!pending, "undrained destination reservation");
  }
  void state_check(scheduler const & s) {
    // Ignore saved snapshots here: this is the current cola's working set.
    // Walk exact dependencies independently of retained_node_ids(), including
    // private carrier routes that are not yet reachable from the root.
    std::set<identity> slots, live;
    std::vector<interval> unused;
    for (auto const & level : s.levels())
      for (auto const & slot : level.slots) if (slot.object) slots.insert(slot.object);
    auto add = [&](identity id) {
      std::set<identity> local;
      unused.clear();
      walk(s, id, local, unused);
      live.insert(local.begin(), local.end());
    };
    add(s.root().root.main);
    add(s.root().root.secondary);
    for (auto const & level : s.levels()) {
      for (auto const & slot : level.slots) {
        add(slot.object);
        add(slot.route.main);
        add(slot.route.secondary);
      }
      if (level.work) {
        auto const & job = *level.work;
        add(job.existing_main);
        add(job.output_node);
        add(job.destination_route.main);
        add(job.destination_route.secondary);
        for (auto input : job.inputs) {
          check(level.slots[input].status == state::active, "unfinished job lost an input slot");
          add(level.slots[input].object);
        }
      }
    }
    for (auto id : live) check(slots.contains(id), "live exact dependency escaped occupied slots");
    check(live.size() <= 3 * s.levels().size(), "working graph exceeds logical slot bound");
    for (unsigned i = 0; i < s.levels().size(); ++i) {
      unsigned active = 0, carrier = 0, staging = 0;
      for (auto const & slot : s.levels()[i].slots) {
        if (slot.status == state::active) {
          ++active;
          auto const & n = s.natives()[s.nodes().at(slot.object).data];
          check(n.last - n.first == (number{1} << i), "logical occupancy is not admission mass");
        }
        if (slot.status == state::carrier_ready || slot.status == state::root_carrier) ++carrier;
        if (slot.status == state::reserved || slot.status == state::carrier_building) ++staging;
        if (slot.status == state::empty) check(slot.object == 0 && !slot.route.main &&
          !slot.route.secondary, "empty slot retains logical ownership");
      }
      check(active <= 2 && carrier <= 1 && staging <= 1, "slot role bound");
      if (i + 1 < s.levels().size()) check(!(s.unsafe(i) && s.unsafe(i + 1)), "adjacent unsafe levels");
    }
  }
  void new_nodes(scheduler const & s, std::size_t & checked) {
    for (; checked < s.nodes().size(); ++checked) {
      auto p = s.nodes()[checked];
      if (checked == 0) continue;
      auto n = s.natives().at(p.data).keys.size();
      auto m = p.next.main ? s.nodes()[p.next.main].augmented : 0;
      auto t = p.next.secondary ? s.nodes()[p.next.secondary].augmented : 0;
      auto expected = n + (m + s.k() - 1) / s.k() + (t + s.k() - 1) / s.k();
      check(p.augmented == expected, "augmentation recurrence");
      auto capacity = number{1} << p.level;
      check(n <= capacity, "native exceeds nominal capacity");
      if (p.secondary) check(p.augmented == n, "secondary contains routing entries");
      else if (p.augmented > 2) check((p.augmented - 2) * (s.k() - 2) <=
        (s.k() + 2) * capacity, "two-target capacity theorem");
    }
  }
  number hash_node(node const & p) {
    number h = 1469598103934665603ULL;
    for (number x : {number(p.data), number(p.next.main), number(p.next.secondary),
        p.augmented, number(p.level), number(p.secondary)}) h = (h ^ x) * 1099511628211ULL;
    return h;
  }

  counters run(number k, number w, unsigned pattern, number length) {
    scheduler s(k, w);
    std::vector<number> input;
    std::vector<snapshot> saved;
    std::vector<identity> owners;
    std::vector<std::pair<identity, number>> immutable;
    std::size_t checked = 0;
    saved.push_back(s.root());
    owners.push_back(s.save_snapshot());
    for (number i = 0; i != length; ++i) {
      auto key = pattern == 0 ? i : pattern == 1 ? 0 : ((i * 13) ^ (i >> 3)) % 37;
      input.push_back(key);
      auto immediate = s.admit_one(key);
      state_check(s);
      coverage(s, s.root());
      auto budget = scheduler::service_budget(i + 1);
      check(immediate <= 128 + 2 * s.levels().size() + 4, "immediate admission charge");
      auto used = s.advance(budget - immediate);
      check(used <= budget - immediate, "advance exceeded budget");
      state_check(s);
      coverage(s, s.root());
      new_nodes(s, checked);
      if (i < 16 || (i & (i + 1)) == 0 || i % 127 == 0) {
        saved.push_back(s.root());
        owners.push_back(s.save_snapshot());
        immutable.emplace_back(s.root().root.main, hash_node(s.nodes()[s.root().root.main]));
        dictionary(s, s.root(), input);
      }
    }
    // Finishing local jobs need not force all hidden carriers to root visibility.
    while (s.pending()) {
      check(s.advance(257) != 0, "drain made no progress");
      state_check(s);
      coverage(s, s.root());
    }
    visibility_history(s);
    for (std::size_t i = 0; i != saved.size(); ++i) {
      auto old = s.saved_snapshot(owners[i]);
      check(old.root.main == saved[i].root.main && old.root.secondary == saved[i].root.secondary &&
        old.admitted == saved[i].admitted, "saved owner changed identities");
      dictionary(s, old, input);
    }
    auto retained = s.retained_node_ids();
    for (auto old : saved) {
      std::set<identity> expected;
      std::vector<interval> unused;
      walk(s, old.root.main, expected, unused);
      walk(s, old.root.secondary, expected, unused);
      for (auto id : expected) check(std::binary_search(retained.begin(), retained.end(), id),
        "snapshot lost an exact dependency pin");
    }
    auto duplicate_owner = s.save_snapshot();
    check(s.retained_node_ids() == retained, "shared snapshot counted a physical pair twice");
    s.drop_snapshot(duplicate_owner);
    for (auto owner : owners) s.drop_snapshot(owner);
    check(s.retained_node_ids().size() < retained.size(), "dropping historical owners did not release pins");
    rejects([&] { (void)s.saved_snapshot(owners.back()); });
    for (auto [id, digest] : immutable) check(hash_node(s.nodes()[id]) == digest, "mutated an old pair identity");
    check(s.counts().max_job_ratio <= 160, "local service bound");
    check(s.counts().merges != 0 && s.counts().delayed_carriers != 0, "no nontrivial merge coverage");
    return s.counts();
  }

  void pauses() {
    scheduler a(3, 1), b(3, 1);
    std::vector<number> input;
    std::array<bool, 3> stages{};
    for (number i = 0; i != 24; ++i) {
      auto key = i % 7;
      input.push_back(key);
      auto aa = a.admit_one(key), bb = b.admit_one(key);
      check(aa == bb, "partitioned immediate work");
      auto before = b.root();
      check(b.advance(0) == 0 && b.root().root.main == before.root.main &&
        b.root().root.secondary == before.root.secondary, "zero budget changed publication");
      auto budget = scheduler::service_budget(i + 1) - aa;
      auto used = a.advance(budget);
      number split_used = 0;
      while (split_used < budget && b.pending()) {
        auto old = b.root();
        auto one = b.advance(1);
        check(one == 1, "one event did not advance");
        ++split_used;
        for (auto const & level : b.levels()) if (level.work)
          stages[static_cast<unsigned>(level.work->stage)] = true;
        state_check(b);
        coverage(b, old);
        coverage(b, b.root());
      }
      check(used == split_used, "service depends on budget partition");
      check(a.root().root.main == b.root().root.main && a.root().root.secondary == b.root().root.secondary,
        "event partition changed immutable identities");
      dictionary(a, a.root(), input);
      dictionary(b, b.root(), input);
    }
    check(stages[0] && stages[1] && stages[2], "did not pause all stages");
    check(b.counts().hidden_inputs != 0 && b.counts().hidden_completed_merges != 0,
      "hidden inputs/finished carriers not exercised");
    check(a.counts().native_work == b.counts().native_work && a.counts().index_work == b.counts().index_work &&
      a.counts().carrier_work == b.counts().carrier_work, "partition changed work charges");
    visibility_history(a);
    visibility_history(b);
  }

  void insufficient_service() {
    scheduler s(3, 1);
    s.admit_one(1);
    s.admit_one(2);
    auto old = s.root();
    check(s.advance(1) == 1, "small service did not start merge");
    rejects([&] { s.admit_one(3); });
    check(s.size() == 2 && s.root().root.main == old.root.main &&
      s.root().root.secondary == old.root.secondary, "rejected admission changed state");
    coverage(s, old);
    rejects([] { scheduler invalid(2); });
    rejects([] { scheduler invalid(3, 0); });
  }
}

int main() {
  try {
    pauses();
    insufficient_service();
    number merges = 0, hidden = 0, worst = 0, levels = 0;
    for (number k : {number{3}, number{7}, number{15}, number{31}})
      for (number w : {number{1}, number{16}})
        for (unsigned pattern = 0; pattern != 3; ++pattern) {
          auto c = run(k, w, pattern, 4096);
          merges += c.merges; hidden += c.hidden_inputs;
          worst = std::max(worst, c.max_job_ratio);
          levels = std::max(levels, c.max_levels);
        }
    auto long_run = run(3, 1, 0, 32768);
    merges += long_run.merges; hidden += long_run.hidden_inputs;
    worst = std::max(worst, long_run.max_job_ratio);
    levels = std::max(levels, long_run.max_levels);
    check(hidden != 0, "no hidden merge inputs");
    std::cout << "COLA count model: " << merges << " merges; " << hidden
      << " hidden inputs; max local work/mass " << worst << "/160; " << levels << " levels\n";
  } catch (std::exception const & e) {
    std::cerr << "COLA count model failure: " << e.what() << '\n';
    return 1;
  }
}
