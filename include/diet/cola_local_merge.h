/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/cola_index.h>
#include <diet/native_merge.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace diet {
  enum class cola_destination_kind { main, secondary };
  enum class cola_merge_phase { native_merge, destination_index, carrier_index, ready, taken, inactive };

  // A local destination plan pins exact existing objects. A new main retains
  // the next level's targets; a new secondary joins an existing main and is a
  // native-only leaf. This plan assigns no logical slots or admission mass.
  template <class P> struct cola_destination_plan {
    using policy_type = P;
    using pair_type = typename cola_index<P>::pair_type;
    using native_pointer = typename cola_index<P>::native_pointer;
    static cola_destination_plan for_main(pair_type main = {}, native_pointer secondary = {}) {
      if (secondary && !main)
        error_detail::raise<std::invalid_argument>("COLA destination secondary requires a main");
      return {cola_destination_kind::main, std::move(main), std::move(secondary)};
    }
    static cola_destination_plan for_secondary(pair_type main) {
      if (!main) error_detail::raise<std::invalid_argument>("COLA secondary destination requires an existing main");
      return {cola_destination_kind::secondary, std::move(main), {}};
    }
    cola_destination_kind kind() const noexcept { return kind_; }
    pair_type main_target() const noexcept { return main_; }
    native_pointer secondary_target() const noexcept { return secondary_; }
  private:
    cola_destination_kind kind_;
    pair_type main_;
    native_pointer secondary_;
    cola_destination_plan(cola_destination_kind kind, pair_type main, native_pointer secondary)
      : kind_(kind), main_(std::move(main)), secondary_(std::move(secondary)) {}
  };

  template <class P> struct cola_local_merge_result {
    using policy_type = P;
    using pair_type = typename cola_index<P>::pair_type;
    using native_pointer = typename cola_index<P>::native_pointer;
    native_pointer merged_native;
    pair_type main;
    native_pointer secondary;
    pair_type carrier;
  };

  // One local COLA job, with explicit native/index/carrier completion. Inputs
  // must represent adjacent chronological history in older,newer order; sorted
  // keys cannot establish that semantic premise. No key is elided.
  //
  // step counts distinct native keys or augmented index occurrences in only
  // the current stage. It never performs finish_stage implicitly. Construction
  // and finish_stage may decode initial heads, allocate, and finalize EF/rank;
  // their work is not bounded by the record budget. A secondary has no index
  // stage. The ready carrier is a local empty-native routing artifact, not
  // necessarily a prepared query root. No slots, visibility, publication,
  // durable progress or scheduler service guarantee are implemented here.
  //
  // Source and plan owners remain pinned for the job's lifetime, including
  // after failure. A failed execution/finalization poisons this job; a rejected
  // precondition does not. Moves transfer stable builder objects without
  // moving user callback state. Assignment releases the previous job's pins.
  template <class P, class Compose = replace_native_value> struct cola_local_merge_job {
    using policy_type = P;
    using index_type = cola_index<P>;
    using native_type = profile_array<P>;
    using native_pointer = typename index_type::native_pointer;
    using pair_type = typename index_type::pair_type;
    using plan_type = cola_destination_plan<P>;
    using result_type = cola_local_merge_result<P>;
    using merge_type = native_merge_builder<P, native_type, Compose>;
    using index_builder_type = cola_index_builder<P>;

    cola_local_merge_job(native_pointer older, native_pointer newer, plan_type plan,
        Compose compose = {}, std::optional<std::uint64_t> common = P::value_width)
      : older_(checked(std::move(older))), newer_(checked(std::move(newer))), plan_(checked(std::move(plan))),
        merge_(std::make_unique<merge_type>(older_, newer_, std::move(compose), common)) {}
    cola_local_merge_job(cola_local_merge_job const &) = delete;
    cola_local_merge_job & operator=(cola_local_merge_job const &) = delete;
    cola_local_merge_job(cola_local_merge_job &&) noexcept = default;
    cola_local_merge_job & operator=(cola_local_merge_job &&) noexcept = default;

    cola_merge_phase phase() const noexcept { return older_ ? phase_ : cola_merge_phase::inactive; }
    bool failed() const noexcept { return failed_; }
    bool finished() const noexcept { return older_ && phase_ == cola_merge_phase::taken; }
    bool done() const noexcept { return older_ && !failed_ && phase_ == cola_merge_phase::ready; }
    bool stage_done() const noexcept {
      if (!older_ || failed_ || finished()) return false;
      if (phase_ == cola_merge_phase::native_merge) return merge_->done();
      if (phase_ == cola_merge_phase::destination_index || phase_ == cola_merge_phase::carrier_index)
        return index_->done();
      return phase_ == cola_merge_phase::ready;
    }
    std::uint64_t step(std::uint64_t budget = 1) {
      require_active();
      try {
        if (phase_ == cola_merge_phase::native_merge) return merge_->step(budget).keys;
        if (phase_ == cola_merge_phase::destination_index || phase_ == cola_merge_phase::carrier_index)
          return index_->step(budget);
        return 0;
      } catch (...) { failed_ = true; throw; }
    }
    void finish_stage() {
      require_active();
      if (phase_ == cola_merge_phase::ready || !stage_done())
        error_detail::raise<std::logic_error>("COLA local stage is not ready for finalization");
      try {
        if (phase_ == cola_merge_phase::native_merge) {
          merged_ = std::make_shared<native_type const>(merge_->finish());
          merge_.reset();
          if (plan_.kind() == cola_destination_kind::main) {
            index_ = std::make_unique<index_builder_type>(merged_, plan_.main_target(), plan_.secondary_target());
            phase_ = cola_merge_phase::destination_index;
          } else {
            main_ = plan_.main_target();
            secondary_ = merged_;
            start_carrier();
          }
        } else if (phase_ == cola_merge_phase::destination_index) {
          main_ = std::make_shared<index_type const>(index_->finish());
          index_.reset();
          start_carrier();
        } else {
          carrier_ = std::make_shared<index_type const>(index_->finish());
          index_.reset();
          phase_ = cola_merge_phase::ready;
        }
      } catch (...) { failed_ = true; throw; }
    }
    result_type finish() {
      require_active();
      if (!done()) error_detail::raise<std::logic_error>("COLA local merge is not ready");
      phase_ = cola_merge_phase::taken;
      return {std::move(merged_), std::move(main_), std::move(secondary_), std::move(carrier_)};
    }
  private:
    native_pointer older_, newer_;
    plan_type plan_;
    std::unique_ptr<merge_type> merge_;
    std::unique_ptr<index_builder_type> index_;
    native_pointer merged_, secondary_;
    pair_type main_, carrier_;
    cola_merge_phase phase_ = cola_merge_phase::native_merge;
    bool failed_ = false;

    static native_pointer checked(native_pointer source) {
      if (!source) error_detail::raise<std::invalid_argument>("null COLA local merge source");
      return source;
    }
    static plan_type checked(plan_type plan) {
      if ((!plan.main_target() && plan.secondary_target()) ||
          (plan.kind() == cola_destination_kind::secondary && !plan.main_target()))
        error_detail::raise<std::invalid_argument>("invalid COLA destination plan");
      return plan;
    }
    void require_active() const {
      if (!older_ || failed_ || finished())
        error_detail::raise<std::logic_error>("COLA local merge is no longer active");
    }
    void start_carrier() {
      auto empty = std::make_shared<native_type const>(native_type::build({}));
      index_ = std::make_unique<index_builder_type>(std::move(empty), main_, secondary_);
      phase_ = cola_merge_phase::carrier_index;
    }
  };
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Stages one owning native merge and its exact COLA destination/carrier indexes.
 */
