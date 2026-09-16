/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Executes redundant COLA slots with charged jobs and immutable frontiers.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/cola_runtime.h>
#include <array>
#include <unordered_map>

namespace diet {
  // A storage family changes native framing without changing the scheduler.
  // Borrowed streams and query navigation follow native_type::stream_family.
  template <class P> struct profile_runtime_storage {
    using native_type = cola_runtime_native<P>;
    using mapped_pair_type = mapped_cola_blob<P>;
    using mapped_native_type = mapped_native<P>;
    static auto encode_native(profile_array<P> const & value) { return encode_native_sections(value); }
    template <class Compose> using merge_type = native_merge_builder<P, native_type, Compose>;
    template <class Compose> static auto make_merge(std::shared_ptr<native_type const> older,
        std::shared_ptr<native_type const> newer, Compose compose) {
      return std::make_unique<merge_type<Compose>>(std::move(older), std::move(newer), std::move(compose));
    }
    template <class Merge> static auto finish_merge(Merge & merge) { return native_type::from_owned(merge.finish()); }
    template <class Node> using index_type = cola_index_builder<P, native_type, Node>;
    template <class Node> static auto make_index(std::shared_ptr<native_type const> native,
        typename Node::pair_type main = {}, std::shared_ptr<native_type const> secondary = {}) {
      return std::make_unique<index_type<Node>>(std::move(native), std::move(main), std::move(secondary));
    }
    template <class Node> static auto finish_index(index_type<Node> & index) { return Node::from_built(index.finish()); }
    static auto empty() { return native_type::from_owned(profile_array<P>::build({})); }
    static auto singleton(profile_record const & record) {
      profile_native_writer<P> writer; writer.append(record); return native_type::from_owned(writer.finish());
    }
  };

  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_node {
    using policy_type = P;
    using storage_type = Storage;
    using native_type = typename Storage::native_type;
    using native_pointer = std::shared_ptr<native_type const>;
    using pair_type = std::shared_ptr<redundant_node const>;
    using built_type = cola_index<P, native_type, redundant_node>;
    static pair_type from_built(built_type value) {
      return pair_type(new redundant_node(std::make_shared<built_type const>(std::move(value))));
    }
    static pair_type from_mapped(std::shared_ptr<typename Storage::mapped_pair_type const> head) {
      if (!head) error_detail::raise<std::invalid_argument>("null redundant mapped head");
      std::vector<std::shared_ptr<typename Storage::mapped_pair_type const>> chain;
      std::unordered_set<typename Storage::mapped_pair_type const *> seen;
      for (auto p = head; p; p = p->main_target()) {
        if (!seen.insert(p.get()).second) error_detail::raise<std::invalid_argument>("cyclic redundant main chain");
        chain.push_back(p);
      }
      pair_type result;
      for (auto i = chain.rbegin(); i != chain.rend(); ++i) result = pair_type(new redundant_node(*i, std::move(result)));
      return result;
    }
    // The loader interns mapped identities and passes the corresponding
    // facades, preserving exact target/native sharing across hidden roots.
    static pair_type from_mapped_parts(std::shared_ptr<typename Storage::mapped_pair_type const> value,
        native_pointer native, pair_type main = {}, native_pointer secondary = {}) {
      if (!value || !native || native->mapped() != value->native_object() ||
          bool(main) != bool(value->main_target()) || (main && main->mapped() != value->main_target()) ||
          bool(secondary) != bool(value->secondary_target()) || (secondary && secondary->mapped() != value->secondary_target()))
        error_detail::raise<std::invalid_argument>("inexact mapped redundant parts");
      return pair_type(new redundant_node(std::move(value), std::move(native), std::move(main), std::move(secondary)));
    }
    typename built_type::view_type view() const { return built_ ? built_->view() : mapped_->view(); }
    native_pointer native_owner() const noexcept { return native_; }
    native_pointer secondary_target() const noexcept { return secondary_; }
    pair_type main_target() const noexcept { return main_; }
    std::uint64_t virtual_size() const { return view().virtual_size(); }
    std::uint64_t group_count() const { return view().group_count(); }
    std::uint64_t depth() const noexcept { return depth_; }
    std::shared_ptr<built_type const> built() const noexcept { return built_; }
    std::shared_ptr<typename Storage::mapped_pair_type const> mapped() const noexcept { return mapped_; }
    bool canonical_mapped() const noexcept { return canonical_mapped_; }
  private:
    template <class, class, class, class> friend struct runtime_store;
    template <class, class, class, class> friend struct runtime_store_detail::graph_sealer;
    template <class, class, class, class, class> friend struct sort_runtime_context;
    // Only the execution context constructs an index from acknowledged exact
    // owner bindings. Original facades may still own small native arrays; the
    // physical pair owns their canonical mapped counterparts without a tail walk.
    static pair_type from_sealed_parts(std::shared_ptr<pair_binding<typename Storage::mapped_pair_type> const> binding,
        std::filesystem::path const & root, native_pointer native, pair_type main, native_pointer secondary) {
      if (!binding || !binding->mapped || !native || binding->mapped->identity() != binding->identity ||
          native->size() != binding->mapped->native_object()->size() ||
          bool(main) != bool(binding->mapped->main_target()) || bool(secondary) != bool(binding->mapped->secondary_target()))
        throw std::invalid_argument("inexact sealed redundant parts");
      auto result = std::shared_ptr<redundant_node>(new redundant_node(binding->mapped,
        std::move(native), std::move(main), std::move(secondary)));
      result->bindings_.get_or_create(binding->catalog, root, [&] { return binding; });
      return result;
    }
    catalog_bindings<pair_binding<typename Storage::mapped_pair_type>> bindings_;
    catalog_bindings<redundant_node> mapped_owners_;
    native_pointer native_, secondary_;
    pair_type main_;
    std::shared_ptr<built_type const> built_;
    std::shared_ptr<typename Storage::mapped_pair_type const> mapped_;
    std::uint64_t depth_;
    bool canonical_mapped_ = false;
    explicit redundant_node(std::shared_ptr<built_type const> value)
      : native_(value->native_owner()), secondary_(value->secondary_target()), main_(value->main_target()), built_(std::move(value)),
        depth_(profile_detail::add(main_ ? main_->depth() : 0, 1)) {}
    redundant_node(std::shared_ptr<typename Storage::mapped_pair_type const> value, native_pointer native, pair_type main, native_pointer secondary)
      : native_(std::move(native)), secondary_(std::move(secondary)), main_(std::move(main)), mapped_(std::move(value)),
        depth_(profile_detail::add(main_ ? main_->depth() : 0, 1)),
        canonical_mapped_(native_->mapped() && (!main_ || main_->canonical_mapped()) && (!secondary_ || secondary_->mapped())) {}
    redundant_node(std::shared_ptr<typename Storage::mapped_pair_type const> value, pair_type main)
      : native_(native_type::from_mapped(value->native_object())),
        secondary_(value->secondary_target() ? native_type::from_mapped(value->secondary_target()) : native_pointer{}),
        main_(std::move(main)), mapped_(std::move(value)),
        depth_(profile_detail::add(main_ ? main_->depth() : 0, 1)), canonical_mapped_(!main_ || main_->canonical_mapped()) {}
  };

  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_object;
  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_routes {
    std::shared_ptr<redundant_object<P, Storage> const> main, secondary;
  };
  template <class P, class Storage> struct redundant_object {
    using native_pointer = typename redundant_node<P, Storage>::native_pointer;
    using pair_type = typename redundant_node<P, Storage>::pair_type;
    std::uint64_t identity = 0, first = 0, last = 0;
    unsigned level = 0;
    native_pointer native;
    pair_type pair; // Absent only for a terminal secondary.
    redundant_routes<P, Storage> next;
    bool secondary() const noexcept { return !pair; }
    std::uint64_t mass() const noexcept { return last - first; }
    std::uint64_t augmented() const { return pair ? pair->virtual_size() : native->size(); }
  };
  enum class redundant_slot_state { empty, active, consumed, reserved, carrier_building, carrier_ready, root_carrier };
  enum class redundant_stage { native_merge, destination_index, carrier_index, commit };
  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_slot {
    redundant_slot_state state = redundant_slot_state::empty;
    std::shared_ptr<redundant_object<P, Storage> const> object;
    redundant_routes<P, Storage> route;
    typename redundant_node<P, Storage>::pair_type carrier;
    bool ever_visible = false;
  };
  // A restart recipe owns completed artifacts and exact targets, never a
  // partially written stream or mutable cursor. Replaying partial work costs
  // new service and puts the restored executor behind a recovery barrier.
  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_job_recipe {
    std::array<unsigned, 2> inputs{};
    unsigned destination = 0, carrier_slot = 0;
    bool new_main = false;
    redundant_routes<P, Storage> destination_route;
    std::shared_ptr<redundant_object<P, Storage> const> existing_main, output;
    typename redundant_node<P, Storage>::native_pointer merged;
    typename redundant_node<P, Storage>::pair_type carrier;
    redundant_stage stage = redundant_stage::native_merge;
  };
  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_level {
    std::array<redundant_slot<P, Storage>, 3> slots{};
    std::optional<redundant_job_recipe<P, Storage>> job;
    std::uint64_t last_destination = 0;
    bool last_destination_visible = false;
  };
  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_frontier {
    std::uint64_t admissions = 0, next_identity = 1, service_due = 0;
    redundant_routes<P, Storage> root;
    std::vector<redundant_level<P, Storage>> levels;
  };
  struct redundant_work {
    std::uint64_t granted = 0, charged = 0;
    std::uint64_t native_work = 0, index_work = 0, carrier_work = 0;
    std::uint64_t metadata_work = 0, root_work = 0;
    std::uint64_t native_inputs = 0, native_outputs = 0, index_occurrences = 0;
    std::uint64_t admissions = 0, merges = 0, indexes = 0, carriers = 0, checkpoints = 0;
    std::uint64_t max_job_charge_per_mass = 0;
  };
  template <class P, class Compose, class Storage> struct redundant_runtime;
  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_snapshot {
    using policy_type = P;
    using storage_type = Storage;
    using query_type = cola_query_root<P, redundant_node<P, Storage>>;
    using object_pointer = std::shared_ptr<redundant_object<P, Storage> const>;
    std::uint64_t admissions() const noexcept { return state_->frontier.admissions; }
    redundant_frontier<P, Storage> const & frontier() const & noexcept { return state_->frontier; }
    redundant_frontier<P, Storage> const & frontier() const && = delete;
    std::span<object_pointer const> runs() const & noexcept { return state_->runs; }
    std::span<object_pointer const> runs() const && = delete;
    query_type const & query_root() const & noexcept { return state_->query; }
    query_type const & query_root() const && = delete;
    auto cursor(bit_view key) const { return state_->query.cursor(key); }
    auto cursor_owned(bit_string key) const { return state_->query.cursor_owned(std::move(key)); }
    bool same_layout(redundant_snapshot const & other) const noexcept { return state_ == other.state_; }
    // Metadata admission only. The native/index payloads must separately be
    // admitted or trusted. Exact pointer relationships are deliberate: a
    // persistent loader interns each immutable identity before calling this.
    static redundant_snapshot restore(redundant_frontier<P, Storage> frontier, typename redundant_node<P, Storage>::pair_type head) {
      auto reject = [](bool value, char const * why) { if (!value) error_detail::raise<std::invalid_argument>(why); };
      auto height = frontier.levels.size();
      reject(height && height <= 64 && frontier.next_identity, "invalid redundant frontier header");
      std::unordered_map<std::uint64_t, object_pointer> objects;
      auto target = [&](redundant_routes<P, Storage> const & route, unsigned level) {
        reject(!route.secondary || bool(route.main), "secondary without main");
        if (route.main) reject(route.main->level == level && bool(route.main->pair) && bool(route.main->native), "invalid main level/role");
        if (route.secondary) reject(route.secondary->level == level && route.secondary->secondary() && bool(route.secondary->native), "invalid secondary level/role");
      };
      auto pair_targets = [&](auto const & pair, redundant_routes<P, Storage> const & route) {
        reject(bool(pair), "missing redundant pair");
        reject(pair->main_target() == (route.main ? route.main->pair : typename redundant_node<P, Storage>::pair_type{}) &&
          pair->secondary_target() == (route.secondary ? route.secondary->native : typename redundant_node<P, Storage>::native_pointer{}), "inexact redundant pair targets");
        auto view = pair->view();
        auto count = route.secondary ? route.secondary->native->size() : 0;
        reject(view.borrowed(0).size() == (route.main ? route.main->pair->group_count() : 0) &&
          view.borrowed(1).size() == count / P::group_size + (count % P::group_size != 0), "redundant target cardinality");
      };
      std::uint64_t unsafe = 0;
      for (unsigned i = 0; i != height; ++i) {
        auto const & level = frontier.levels[i]; unsigned active = 0, carriers = 0, staging = 0;
        reject(level.last_destination < frontier.next_identity, "future redundant destination identity");
        for (unsigned position = 0; position != 3; ++position) {
          auto const & slot = level.slots[position];
          if (slot.state == redundant_slot_state::carrier_building)
            reject(level.job && level.job->carrier_slot == position, "orphan building carrier");
          if (slot.state == redundant_slot_state::reserved)
            reject(i && frontier.levels[i - 1].job && frontier.levels[i - 1].job->destination == position, "orphan destination reservation");
          active += slot.state == redundant_slot_state::active;
          carriers += slot.state == redundant_slot_state::carrier_ready || slot.state == redundant_slot_state::root_carrier;
          staging += slot.state == redundant_slot_state::reserved || slot.state == redundant_slot_state::carrier_building;
          target(slot.route, i + 1);
          if (slot.object) {
            auto const & object = slot.object;
            reject(object->identity && object->identity < frontier.next_identity && object->level == i && bool(object->native) &&
              objects.emplace(object->identity, object).second, "invalid/repeated redundant object");
            reject(object->last >= object->first && object->last <= frontier.admissions, "invalid redundant interval");
            if (slot.state == redundant_slot_state::root_carrier)
              reject(!i && object->first == 0 && object->last == 0 && !object->native->size() && bool(object->pair), "invalid root carrier");
            else reject(object->mass() == (std::uint64_t{1} << i) && object->native->size() <= object->mass(), "invalid redundant object mass");
            target(object->next, i + 1);
            if (object->pair) { reject(object->pair->native_owner() == object->native, "inexact redundant native owner"); pair_targets(object->pair, object->next); }
            else reject(!object->next.main && !object->next.secondary, "recursive redundant secondary");
          }
          switch (slot.state) {
            case redundant_slot_state::empty:
              reject(!slot.object && !slot.route.main && !slot.route.secondary && !slot.carrier && !slot.ever_visible, "nonempty vacant slot"); break;
            case redundant_slot_state::active: case redundant_slot_state::consumed:
              reject(bool(slot.object) && !slot.route.main && !slot.route.secondary && !slot.carrier, "invalid occupied slot"); break;
            case redundant_slot_state::carrier_building:
              reject(!slot.object && !slot.route.main && !slot.route.secondary && !slot.carrier, "invalid building carrier"); break;
            case redundant_slot_state::carrier_ready:
              reject(!slot.object && bool(slot.route.main), "invalid ready carrier");
              pair_targets(slot.carrier, slot.route); reject(!slot.carrier->native_owner()->size(), "carrier has natives"); break;
            case redundant_slot_state::root_carrier:
              reject(bool(slot.object) && !slot.carrier, "invalid root carrier slot");
              pair_targets(slot.object->pair, slot.route); break;
            case redundant_slot_state::reserved: break;
            default: reject(false, "unknown redundant slot state");
          }
        }
        reject(active <= 2 && carriers <= 1 && staging <= 1, "invalid redundant slot populations");
        if (active == 2 || level.job) unsafe |= std::uint64_t{1} << i;
        if (level.job) {
          auto const & job = *level.job;
          reject(i + 1 < height && job.inputs[0] < 3 && job.inputs[1] < 3 && job.inputs[0] != job.inputs[1] &&
            job.destination < 3 && job.carrier_slot < 3, "invalid redundant job slots");
          auto const & a = level.slots[job.inputs[0]], & b = level.slots[job.inputs[1]];
          reject(a.state == redundant_slot_state::active && b.state == redundant_slot_state::active && a.object->last == b.object->first &&
            level.slots[job.carrier_slot].state == redundant_slot_state::carrier_building &&
            frontier.levels[i + 1].slots[job.destination].state == redundant_slot_state::reserved, "invalid redundant job reservation");
          target(job.destination_route, i + 2);
          reject(job.new_main ? !job.existing_main : (job.existing_main && job.existing_main->level == i + 1 && bool(job.existing_main->pair)), "invalid destination main");
          auto const & destination = frontier.levels[i + 1].slots[job.destination];
          reject(destination.route.main == job.destination_route.main && destination.route.secondary == job.destination_route.secondary,
            "inexact destination reservation route");
          if (job.stage == redundant_stage::native_merge || job.stage == redundant_stage::destination_index)
            reject(!destination.object, "premature reserved output");
          if (job.stage != redundant_stage::native_merge)
            reject(job.merged && job.merged->size() <= b.object->last - a.object->first, "invalid merged native");
          if (job.stage == redundant_stage::native_merge || job.stage == redundant_stage::destination_index)
            reject(!job.output && !job.carrier && (job.stage != redundant_stage::destination_index || job.new_main), "premature job artifact");
          else {
            reject(job.output && job.output == frontier.levels[i + 1].slots[job.destination].object &&
              job.output->native == job.merged && job.output->first == a.object->first && job.output->last == b.object->last &&
              job.output->secondary() != job.new_main, "invalid destination artifact");
            if (job.stage == redundant_stage::commit) {
              auto route = job.new_main ? redundant_routes<P, Storage>{job.output, {}} : redundant_routes<P, Storage>{job.existing_main, job.output};
              pair_targets(job.carrier, route); reject(!job.carrier->native_owner()->size(), "nonempty lookahead carrier");
            } else reject(job.stage == redundant_stage::carrier_index && !job.carrier, "invalid job phase");
          }
        }
      }
      reject(!(unsafe & (unsafe << 1)), "adjacent unsafe checkpoint levels");
      auto bound = profile_detail::add(profile_detail::multiply(profile_detail::add(P::group_size, 6), 32), 2048);
      auto allowance = profile_detail::multiply(profile_detail::multiply(bound, 8),
        std::bit_width(frontier.admissions) + 2);
      reject(frontier.service_due <= allowance && (unsafe || !frontier.service_due), "invalid checkpoint service obligation");
      auto owned = [&](object_pointer const & value) { if (value) reject(objects.contains(value->identity) && objects.at(value->identity) == value, "dependency escaped redundant slots"); };
      for (auto const & [id, value] : objects) { (void)id; owned(value->next.main); owned(value->next.secondary); }
      for (auto const & level : frontier.levels) {
        for (auto const & slot : level.slots) { owned(slot.route.main); owned(slot.route.secondary); }
        if (level.job) { auto const & job = *level.job; owned(job.existing_main); owned(job.output); owned(job.destination_route.main); owned(job.destination_route.secondary); }
      }
      target(frontier.root, 0); owned(frontier.root.main); owned(frontier.root.secondary);
      auto query = query_type::adopt_prepared(head);
      if (!frontier.root.main) reject(!frontier.admissions && !head->virtual_size() && !head->main_target() && !head->secondary_target(), "nonempty unrooted checkpoint");
      else {
        auto p = head;
        if (!frontier.root.secondary) {
          while (p != frontier.root.main->pair) { reject(p && !p->native_owner()->size() && !p->secondary_target(), "checkpoint root differs from frontier"); p = p->main_target(); }
        } else {
          while (p && !(p->main_target() == frontier.root.main->pair && p->secondary_target() == frontier.root.secondary->native)) {
            reject(!p->native_owner()->size() && !p->secondary_target(), "checkpoint root differs from frontier"); p = p->main_target();
          }
          reject(p && !p->native_owner()->size(), "missing two-root entry carrier");
        }
      }
      std::vector<object_pointer> runs; std::unordered_set<std::uint64_t> visible;
      auto walk = [&](auto && self, object_pointer const & value) -> void {
        if (!value) return;
        reject(visible.insert(value->identity).second, "repeated visible checkpoint object");
        self(self, value->next.main); self(self, value->next.secondary);
        if (value->mass()) runs.push_back(value);
      };
      walk(walk, frontier.root.main); walk(walk, frontier.root.secondary);
      std::uint64_t next = 0;
      for (auto const & run : runs) { reject(run->first == next, "checkpoint chronology gap"); next = run->last; }
      reject(next == frontier.admissions, "checkpoint chronology extent");
      for (auto const & level : frontier.levels) for (auto const & slot : level.slots)
        if (slot.object && visible.contains(slot.object->identity)) reject(slot.ever_visible, "visible object lacks visibility history");
      return redundant_snapshot(std::make_shared<state const>(state{std::move(frontier), std::move(query), std::move(runs)}));
    }
  private:
    template <class, class, class> friend struct redundant_runtime;
    struct state {
      redundant_frontier<P, Storage> frontier;
      query_type query;
      std::vector<object_pointer> runs;
    };
    std::shared_ptr<state const> state_;
    explicit redundant_snapshot(std::shared_ptr<state const> value) : state_(std::move(value)) {}
  };

  // The scheduler uses admission mass, not surviving key count. At most three
  // logical slots occupy each level; historical snapshots retain their own
  // exact graphs. Only main routes recurse. All composition is oldest first.
  // Service charges record/directory/metadata operations, not key bytes or
  // elapsed time. Existing EF finalizers remain atomic after their allowance
  // has been funded. This is not a hard per-call latency or I/O bound.
  template <class P, class Compose = replace_native_value, class Storage = profile_runtime_storage<P>> struct redundant_runtime {
    using policy_type = P;
    using storage_type = Storage;
    using node_type = redundant_node<P, Storage>;
    using native_type = typename node_type::native_type;
    using native_pointer = typename node_type::native_pointer;
    using pair_type = typename node_type::pair_type;
    using snapshot_type = redundant_snapshot<P, Storage>;
    using query_type = typename snapshot_type::query_type;
    using object_type = redundant_object<P, Storage>;
    using object_pointer = std::shared_ptr<object_type const>;
    using routes = redundant_routes<P, Storage>;
    using level_type = redundant_level<P, Storage>;
    static constexpr unsigned maximum_levels = 64;
    static_assert(P::group_size <= (std::numeric_limits<std::uint64_t>::max() - 2240) / 32, "redundant policy charge is not representable");
    static constexpr std::uint64_t local_charge_bound = 32 * (P::group_size + 6) + 2048;
    static std::uint64_t service_budget(std::uint64_t admissions) {
      return profile_detail::multiply(profile_detail::multiply(local_charge_bound, 8),
        std::bit_width(admissions) + 2);
    }
    explicit redundant_runtime(Compose compose = {}) : redundant_runtime(Storage{}, std::move(compose)) {}
    redundant_runtime(Storage storage, Compose compose = {}) : e_(std::make_unique<execution>(std::move(storage), std::move(compose))) {}
    static redundant_runtime from_snapshot(snapshot_type source, Compose compose = {}) {
      return from_snapshot(std::move(source), Storage{}, std::move(compose));
    }
    static redundant_runtime from_snapshot(snapshot_type source, Storage storage, Compose compose = {}) {
      return redundant_runtime(std::make_unique<execution>(std::move(source), std::move(storage), std::move(compose)));
    }
    Storage const & storage() const & { return active().storage; }
    Storage const & storage() const && = delete;
    redundant_runtime(redundant_runtime const &) = delete;
    redundant_runtime & operator=(redundant_runtime const &) = delete;
    redundant_runtime(redundant_runtime &&) noexcept = default;
    redundant_runtime & operator=(redundant_runtime &&) noexcept = default;
    snapshot_type snapshot() const { return active().published; }
    redundant_work work() const { return active().work; }
    std::uint64_t service_due() const { return active().service_due; }
    // Explicit metadata-only capture; unlike snapshot(), this charges and
    // allocates a new full frontier, without finalizing or scanning a payload.
    snapshot_type checkpoint() {
      auto & e = writable();
      try { e.direct(e.checkpoint_price(), category::metadata); e.checkpoint(); }
      catch (...) { e.poison(); throw; }
      return e.published;
    }
    bool pending() const noexcept { return e_ && (e_->unsafe || e_->checkpoint_pending); }
    bool failed() const noexcept { return e_ && e_->failed; }
    void poison() noexcept { if (e_) e_->poison(); }
    bool admission_ready() const noexcept { return e_ && e_->ready(); }
    bool recovering() const noexcept { return e_ && e_->recovery; }
    std::uint64_t credit() const { return active().credit; }
    std::uint64_t next_service_cost() const { return active().price(); }
    std::uint64_t admission_cost() const { auto const & e = active(); e.require_ready(); return e.admission_price(); }
    snapshot_type advance(std::uint64_t budget) {
      auto & e = writable();
      if (!budget || !pending()) return e.published;
      try { e.grant(budget); e.service_due -= std::min(e.service_due, budget); e.serve(); }
      catch (...) { e.poison(); throw; }
      return e.published;
    }
    std::optional<snapshot_type> try_contribute(profile_record const & record, std::uint64_t budget = 0) {
      auto & e = writable();
      if (!e.ready()) return std::nullopt;
      validate(record); (void)add(e.admissions, 1);
      auto prior = e.published;
      try { e.admit(record); if (budget) { e.grant(budget); e.service_due -= std::min(e.service_due, budget); e.serve(); } }
      catch (...) { e.published = std::move(prior); e.poison(); throw; }
      return e.published;
    }
    snapshot_type contribute(profile_record const & record) {
      auto & e = writable(); validate(record);
      auto allowance = service_budget(add(e.admissions, 1));
      if (!e.ready()) advance(allowance);
      if (!e.ready()) error_detail::raise<std::logic_error>("redundant admission needs additional recovery service");
      return *try_contribute(record, allowance);
    }
  private:
    static std::uint64_t add(std::uint64_t a, std::uint64_t b) { return profile_detail::add(a, b); }
    static std::uint64_t ceil(std::uint64_t n, std::uint64_t d) { return n / d + (n % d != 0); }
    static void require(bool value, char const * message) { if (!value) error_detail::raise<std::logic_error>(message); }
    static void validate(profile_record const & record) {
      auto key = record.key.view(), value = record.value.view();
      if ((key.size() & (P::bits_per_unit - 1)) || (value.size() & (P::bits_per_unit - 1)) ||
          (P::value_width && value.size() / P::bits_per_unit != *P::value_width))
        error_detail::raise<std::invalid_argument>("invalid encoded redundant contribution");
    }
    using merge_compose = std::conditional_t<std::is_same_v<Compose, replace_native_value>, Compose, std::reference_wrapper<Compose>>;
    using merge_type = typename Storage::template merge_type<merge_compose>;
    using index_type = typename Storage::template index_type<node_type>;
    enum class action { native_start, native_step, native_finish, index_start, index_step, index_finish, commit };
    enum class category { native, index, carrier, metadata, root };
    struct worker {
      action next = action::native_start;
      std::unique_ptr<merge_type> merge;
      std::unique_ptr<index_type> index;
      std::uint64_t charged = 0;
    };
    struct execution {
      Storage storage; // Outlives workers whose file outputs borrow its concrete context.
      Compose compose;
      native_pointer empty;
      pair_type empty_pair;
      std::array<level_type, maximum_levels> levels{};
      std::array<std::unique_ptr<worker>, maximum_levels> workers{};
      unsigned height = 1;
      std::uint64_t admissions = 0, next_identity = 1, unsafe = 0, credit = 0, service_due = 0;
      routes root;
      query_type query;
      snapshot_type published;
      redundant_work work;
      bool failed = false, recovery = false, checkpoint_pending = false, changed = false;
      void poison() noexcept {
        failed = true;
        if constexpr (requires { { storage.poison() } noexcept; }) storage.poison();
      }
      native_pointer make_empty() { return storage.empty(); }
      static pair_type make_empty_pair(native_pointer native) { cola_index_builder<P, native_type, node_type> builder(std::move(native)); return node_type::from_built(builder.finish()); }
      static snapshot_type initial(pair_type pair) {
        redundant_frontier<P, Storage> f; f.levels.resize(1);
        return snapshot_type(std::make_shared<typename snapshot_type::state const>(typename snapshot_type::state{
          std::move(f), query_type::adopt_prepared(std::move(pair)), {}}));
      }
      execution(Storage context, Compose value) : storage(std::move(context)), compose(std::move(value)), empty(make_empty()), empty_pair(make_empty_pair(empty)),
        query(query_type::adopt_prepared(empty_pair)), published(initial(empty_pair)) { direct(8, category::metadata); }
      execution(snapshot_type source, Storage context, Compose value) : storage(std::move(context)), compose(std::move(value)), empty(make_empty()), empty_pair(make_empty_pair(empty)),
        query(source.query_root()), published(std::move(source)) {
        auto const & f = published.frontier();
        require(!f.levels.empty() && f.levels.size() <= maximum_levels, "invalid redundant checkpoint height");
        height = static_cast<unsigned>(f.levels.size()); admissions = f.admissions; next_identity = f.next_identity; root = f.root; service_due = f.service_due;
        for (unsigned i = 0; i != height; ++i) {
          levels[i] = f.levels[i];
          if (levels[i].job) {
            workers[i] = std::make_unique<worker>();
            auto stage = levels[i].job->stage;
            workers[i]->next = stage == redundant_stage::native_merge ? action::native_start :
              stage == redundant_stage::commit ? action::commit : action::index_start;
          }
          refresh(i);
        }
        recovery = unsafe != 0;
        direct(8 + height, category::metadata);
      }
      struct active_slots { std::array<unsigned, 2> positions{}; unsigned count = 0; };
      active_slots active(unsigned i) const {
        active_slots out;
        for (unsigned s = 0; s != 3; ++s) if (levels[i].slots[s].state == redundant_slot_state::active) {
          require(out.count < 2, "too many active redundant slots"); out.positions[out.count++] = s;
        }
        if (out.count == 2 && levels[i].slots[out.positions[0]].object->first > levels[i].slots[out.positions[1]].object->first)
          std::swap(out.positions[0], out.positions[1]);
        return out;
      }
      std::optional<unsigned> find(unsigned i, redundant_slot_state state) const {
        for (unsigned s = 0; s != 3; ++s) if (levels[i].slots[s].state == state) return s;
        return {};
      }
      unsigned vacant(unsigned i) const { auto found = find(i, redundant_slot_state::empty); require(bool(found), "no redundant shadow slot"); return *found; }
      bool ready() const noexcept {
        if (failed || recovery || checkpoint_pending || service_due || levels[0].job) return false;
        unsigned count = 0;
        for (auto const & slot : levels[0].slots) count += slot.state == redundant_slot_state::active;
        return count < 2;
      }
      void require_ready() const { require(ready(), "redundant admission is not ready"); }
      void refresh(unsigned i) {
        auto flag = std::uint64_t{1} << i;
        if (levels[i].job || active(i).count == 2) unsafe |= flag; else unsafe &= ~flag;
      }
      merge_compose merger() { if constexpr (std::is_same_v<Compose, replace_native_value>) return compose; else return std::ref(compose); }
      static std::uint64_t augmented(object_pointer const & object) { return object ? object->augmented() : 0; }
      static std::uint64_t index_price(std::uint64_t native, std::uint64_t main, std::uint64_t secondary) {
        auto a = ceil(main, P::group_size), b = ceil(secondary, P::group_size);
        auto n = add(add(native, a), b);
        return add(add(10, profile_detail::multiply(n, P::group_size + 6)),
          add(ceil(a, P::codec_block_size), ceil(b, P::codec_block_size)));
      }
      static std::uint64_t root_price(std::uint64_t main, std::uint64_t secondary) {
        if (!secondary && main <= P::group_size) return 0;
        std::uint64_t result = 0;
        do {
          result = add(result, index_price(0, main, secondary));
          main = add(ceil(main, P::group_size), ceil(secondary, P::group_size)); secondary = 0;
        } while (main > P::group_size);
        return result;
      }
      std::uint64_t checkpoint_price() const { return 8 * height + 16; }
      std::uint64_t visibility_price() const { return 6 * height + 8; }
      void tally(std::uint64_t amount, category kind) {
        auto next = work; next.charged = add(next.charged, amount);
        auto field = kind == category::native ? &redundant_work::native_work : kind == category::index ? &redundant_work::index_work :
          kind == category::carrier ? &redundant_work::carrier_work : kind == category::root ? &redundant_work::root_work : &redundant_work::metadata_work;
        next.*field = add(next.*field, amount); work = next;
      }
      void direct(std::uint64_t amount, category kind) { auto total = add(work.granted, amount); tally(amount, kind); work.granted = total; }
      void grant(std::uint64_t amount) { auto next = add(credit, amount), total = add(work.granted, amount); credit = next; work.granted = total; }
      object_pointer object(native_pointer native, pair_type pair, routes next, unsigned level, std::uint64_t first, std::uint64_t last) {
        auto id = next_identity; next_identity = add(next_identity, 1);
        return std::make_shared<object_type const>(object_type{id, first, last, level, std::move(native), std::move(pair), std::move(next)});
      }
      pair_type build_index(native_pointer native, routes target) {
        auto builder = storage.template make_index<node_type>(std::move(native),
          target.main ? target.main->pair : pair_type{}, target.secondary ? target.secondary->native : native_pointer{});
        while (!builder->done()) { auto n = builder->step(1); work.index_occurrences = add(work.index_occurrences, n); }
        auto pair = storage.template finish_index<node_type>(*builder); work.indexes = add(work.indexes, 1); return pair;
      }
      pair_type prepare_root(pair_type main, native_pointer secondary = {}) {
        if (!main) return empty_pair;
        if (!secondary && main->virtual_size() <= P::group_size) return main;
        do {
          auto builder = storage.template make_index<node_type>(empty, main, secondary);
          while (!builder->done()) { auto n = builder->step(1); work.index_occurrences = add(work.index_occurrences, n); }
          main = storage.template finish_index<node_type>(*builder); secondary.reset();
          work.indexes = add(work.indexes, 1); work.carriers = add(work.carriers, 1);
        } while (main->virtual_size() > P::group_size);
        return main;
      }
      template <class F> static void visit(object_pointer const & value, F & fn) {
        if (!value) return;
        fn(value);
        visit(value->next.main, fn); visit(value->next.secondary, fn);
      }
      void visibility() {
        std::array<unsigned char, maximum_levels> visible{};
        auto mark = [&](object_pointer const & value) {
          require(value->level < height, "visible redundant level out of range");
          auto & level = levels[value->level]; bool found = false;
          for (unsigned s = 0; s != 3; ++s) if (level.slots[s].object == value) {
            require(!(visible[value->level] & (1u << s)), "repeated visible redundant object");
            visible[value->level] |= static_cast<unsigned char>(1u << s);
            level.slots[s].ever_visible = true; found = true;
          }
          require(found, "visible redundant object escaped slots");
          if (level.last_destination == value->identity) level.last_destination_visible = true;
        };
        visit(root.main, mark); visit(root.secondary, mark);
        for (unsigned i = 0; i != height; ++i) for (unsigned s = 0; s != 3; ++s) {
          auto & slot = levels[i].slots[s];
          if (slot.state == redundant_slot_state::consumed && slot.ever_visible && !(visible[i] & (1u << s))) slot = {};
        }
        auto prepared = prepare_root(root.main ? root.main->pair : pair_type{}, root.secondary ? root.secondary->native : native_pointer{});
        query = query_type::adopt_prepared(std::move(prepared)); changed = true;
      }
      void checkpoint() {
        if (!unsafe) service_due = 0;
        redundant_frontier<P, Storage> f; f.admissions = admissions; f.next_identity = next_identity; f.root = root; f.service_due = service_due;
        f.levels.assign(levels.begin(), levels.begin() + height);
        std::vector<object_pointer> runs;
        auto walk = [&](auto && self, object_pointer const & value) -> void {
          if (!value) return;
          self(self, value->next.main); self(self, value->next.secondary);
          if (value->mass()) runs.push_back(value);
        };
        walk(walk, root.main); walk(walk, root.secondary);
        std::uint64_t next = 0;
        for (auto const & value : runs) { require(value->first == next, "redundant history gap or overlap"); next = value->last; }
        require(next == admissions, "redundant checkpoint omits admissions");
        auto state = std::make_shared<typename snapshot_type::state const>(typename snapshot_type::state{std::move(f), query, std::move(runs)});
        published = snapshot_type(std::move(state)); work.checkpoints = add(work.checkpoints, 1);
        checkpoint_pending = false; changed = false;
        if (!unsafe) { recovery = false; credit = 0; service_due = 0; }
      }
      std::uint64_t admission_price() const {
        auto entries = active(0); routes route;
        if (!entries.count) if (auto c = find(0, redundant_slot_state::root_carrier)) route = levels[0].slots[*c].route;
        auto main = entries.count ? augmented(levels[0].slots[entries.positions[0]].object) :
          add(1, add(ceil(augmented(route.main), P::group_size), ceil(augmented(route.secondary), P::group_size)));
        auto result = add(12, add(checkpoint_price(), visibility_price()));
        if (!entries.count) result = add(result, index_price(1, augmented(route.main), augmented(route.secondary)));
        return add(result, root_price(main, entries.count ? 1 : 0));
      }
      void admit(profile_record const & record) {
        require_ready(); direct(admission_price(), category::root);
        auto native = storage.singleton(record);
        work.native_outputs = add(work.native_outputs, 1);
        auto entries = active(0); unsigned pos; routes route; pair_type pair;
        if (entries.count) pos = vacant(0);
        else {
          auto c = find(0, redundant_slot_state::root_carrier); pos = c ? *c : vacant(0);
          if (c) route = levels[0].slots[pos].route;
          pair = build_index(native, route);
        }
        auto next = add(admissions, 1);
        auto value = object(std::move(native), std::move(pair), route, 0, admissions, next);
        levels[0].slots[pos] = {redundant_slot_state::active, value, {}, {}, false};
        if (entries.count) root.secondary = value; else root = {value, {}};
        admissions = next; work.admissions = add(work.admissions, 1); refresh(0);
        service_due = unsafe ? service_budget(admissions) : 0; visibility(); checkpoint();
      }
      unsigned selected() const { require(unsafe != 0, "no unsafe redundant level"); return std::countr_zero(unsafe); }
      routes index_targets(unsigned i) const {
        auto const & r = *levels[i].job;
        if (r.stage == redundant_stage::destination_index) return r.destination_route;
        return r.new_main ? routes{r.output, {}} : routes{r.existing_main, r.output};
      }
      std::uint64_t price() const {
        if (checkpoint_pending) return checkpoint_price();
        if (!unsafe) return 0;
        auto i = selected();
        if (!levels[i].job) return 8;
        auto const & r = *levels[i].job; auto const & w = *workers[i];
        switch (w.next) {
          case action::native_start: return 3;
          case action::native_step: return 3;
          case action::native_finish: return add(ceil(w.merge->progress().keys, P::codec_block_size), 2);
          case action::index_start: return 5;
          case action::index_step: return P::group_size + 6;
          case action::index_finish: {
            auto t = index_targets(i);
            return add(5, add(ceil(ceil(augmented(t.main), P::group_size), P::codec_block_size),
              ceil(ceil(augmented(t.secondary), P::group_size), P::codec_block_size)));
          }
          case action::commit:
            return i ? 8 : add(8, add(visibility_price(), root_price(r.carrier->virtual_size(), 0)));
        }
        error_detail::raise<std::logic_error>("invalid redundant worker action");
      }
      category charge_kind(unsigned i) const {
        if (!levels[i].job) return category::metadata;
        auto const & w = *workers[i];
        if (w.next == action::commit) return i ? category::metadata : category::root;
        auto stage = levels[i].job->stage;
        return stage == redundant_stage::native_merge ? category::native :
          stage == redundant_stage::destination_index ? category::index : category::carrier;
      }
      void begin(unsigned i) {
        auto input = active(i); require(input.count == 2, "redundant merge needs two active inputs");
        require(i + 1 < maximum_levels, "redundant admission mass overflow");
        if (i + 1 == height) ++height;
        require(!(unsafe & (std::uint64_t{1} << (i + 1))), "adjacent unsafe redundant levels");
        auto & destination = levels[i + 1];
        require(!destination.last_destination || destination.last_destination_visible, "destination reused before visibility");
        auto carrier = find(i + 1, redundant_slot_state::carrier_ready);
        auto dest = carrier ? *carrier : vacant(i + 1);
        auto present = active(i + 1); require(present.count <= 1 && (!carrier || !present.count), "invalid destination occupancy");
        auto older = levels[i].slots[input.positions[0]].object, newer = levels[i].slots[input.positions[1]].object;
        auto mass = std::uint64_t{1} << i;
        require(older->last == newer->first && older->mass() == mass && newer->mass() == mass, "nonadjacent redundant merge history");
        redundant_job_recipe<P, Storage> recipe;
        recipe.inputs = input.positions; recipe.destination = dest; recipe.carrier_slot = vacant(i);
        recipe.destination_route = carrier ? destination.slots[dest].route : routes{};
        recipe.new_main = bool(carrier) || !present.count;
        if (present.count) {
          recipe.existing_main = destination.slots[present.positions[0]].object;
          require(!recipe.existing_main->secondary(), "single destination is secondary");
        }
        auto worker = std::make_unique<typename redundant_runtime::worker>();
        worker->charged = 8;
        levels[i].slots[recipe.carrier_slot] = {redundant_slot_state::carrier_building, {}, {}, {}, false};
        destination.slots[dest].state = redundant_slot_state::reserved;
        levels[i].job = std::move(recipe); workers[i] = std::move(worker); changed = true;
      }
      void perform(unsigned i) {
        if (!levels[i].job) { begin(i); return; }
        auto & r = *levels[i].job; auto & w = *workers[i];
        auto source = [&](unsigned which) { return levels[i].slots[r.inputs[which]].object; };
        switch (w.next) {
          case action::native_start:
            w.merge = storage.template make_merge<merge_compose>(source(0)->native, source(1)->native, merger());
            w.next = w.merge->done() ? action::native_finish : action::native_step;
            break;
          case action::native_step: {
            auto done = w.merge->step(1);
            work.native_inputs = add(work.native_inputs, done.input_records); work.native_outputs = add(work.native_outputs, done.keys);
            if (w.merge->done()) w.next = action::native_finish;
            break;
          }
          case action::native_finish:
            r.merged = storage.finish_merge(*w.merge); w.merge.reset();
            if (r.new_main) r.stage = redundant_stage::destination_index;
            else {
              r.output = object(r.merged, {}, {}, i + 1, source(0)->first, source(1)->last);
              levels[i + 1].slots[r.destination].object = r.output;
              r.stage = redundant_stage::carrier_index;
            }
            w.next = action::index_start; changed = true;
            break;
          case action::index_start: {
            auto t = index_targets(i);
            w.index = storage.template make_index<node_type>(r.stage == redundant_stage::destination_index ? r.merged : empty,
              t.main ? t.main->pair : pair_type{}, t.secondary ? t.secondary->native : native_pointer{});
            w.next = w.index->done() ? action::index_finish : action::index_step;
            break;
          }
          case action::index_step:
            work.index_occurrences = add(work.index_occurrences, w.index->step(1));
            if (w.index->done()) w.next = action::index_finish;
            break;
          case action::index_finish: {
            auto pair = storage.template finish_index<node_type>(*w.index); w.index.reset(); work.indexes = add(work.indexes, 1);
            if (r.stage == redundant_stage::destination_index) {
              r.output = object(r.merged, std::move(pair), r.destination_route, i + 1, source(0)->first, source(1)->last);
              levels[i + 1].slots[r.destination].object = r.output;
              r.stage = redundant_stage::carrier_index; w.next = action::index_start;
            } else {
              r.carrier = std::move(pair); r.stage = redundant_stage::commit; w.next = action::commit;
              work.carriers = add(work.carriers, 1);
            }
            changed = true;
            break;
          }
          case action::commit: {
            auto recipe = r; auto charge = w.charged; auto mass = std::uint64_t{1} << i;
            require(ceil(charge, mass) <= local_charge_bound, "redundant local structural bound exceeded");
            work.max_job_charge_per_mass = std::max(work.max_job_charge_per_mass, ceil(charge, mass));
            for (auto input : recipe.inputs) levels[i].slots[input].state = redundant_slot_state::consumed;
            auto target = recipe.new_main ? routes{recipe.output, {}} : routes{recipe.existing_main, recipe.output};
            levels[i].slots[recipe.carrier_slot] = {redundant_slot_state::carrier_ready, {}, target, recipe.carrier, false};
            auto & destination = levels[i + 1];
            destination.slots[recipe.destination] = {redundant_slot_state::active, recipe.output, {}, {}, false};
            destination.last_destination = recipe.output->identity; destination.last_destination_visible = false;
            levels[i].job.reset(); workers[i].reset(); work.merges = add(work.merges, 1);
            refresh(i); refresh(i + 1); changed = true;
            if (!i) {
              auto value = object(empty, recipe.carrier, target, 0, 0, 0);
              levels[0].slots[recipe.carrier_slot] = {redundant_slot_state::root_carrier, value, target, {}, false};
              root = {std::move(value), {}}; visibility(); checkpoint_pending = true;
            } else if (!unsafe) checkpoint_pending = true;
            break;
          }
        }
      }
      void serve() {
        while (unsafe || checkpoint_pending) {
          auto amount = price();
          if (credit < amount) break;
          if (checkpoint_pending) {
            tally(amount, category::metadata); credit -= amount; checkpoint();
          } else {
            auto i = selected(); tally(amount, charge_kind(i)); credit -= amount;
            if (workers[i]) workers[i]->charged = add(workers[i]->charged, amount);
            perform(i);
          }
        }
        if (!unsafe && !checkpoint_pending) { recovery = false; credit = 0; service_due = 0; }
      }
    };
    std::unique_ptr<execution> e_;
    explicit redundant_runtime(std::unique_ptr<execution> value) : e_(std::move(value)) {}
    execution const & active() const { if (!e_) error_detail::raise<std::logic_error>("moved-from redundant runtime"); return *e_; }
    execution & writable() { (void)active(); if (e_->failed) error_detail::raise<std::logic_error>("failed redundant runtime"); return *e_; }
  };
  // Select this executor through the same typed/storage family seam as the
  // conservative binary backend, without changing the encoded policy.
  template <class P, class Storage = profile_runtime_storage<P>> struct redundant_runtime_family {
    using policy_type = P;
    using storage_type = Storage;
    using snapshot_type = redundant_snapshot<P, Storage>;
    using frontier_type = redundant_frontier<P, Storage>;
    using object_type = redundant_object<P, Storage>;
    using routes_type = redundant_routes<P, Storage>;
    using slot_type = redundant_slot<P, Storage>;
    using level_type = redundant_level<P, Storage>;
    using job_recipe_type = redundant_job_recipe<P, Storage>;
    using node_type = redundant_node<P, Storage>;
    using native_type = typename Storage::native_type;
    template <class Compose> using runtime_type = redundant_runtime<P, Compose, Storage>;
  };
}
