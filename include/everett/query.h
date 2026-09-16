/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Prepares immutable catalog chains and enumerates every matching native segment.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/error_detail.h>

#include <everett/index_pipeline.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace everett {
  template <class P, class Blob = profile_blob<P>> struct query_root;
  template <class P> struct query_root_builder;
  template <class P, class Blob = profile_blob<P>> struct query_cursor;

  // One native segment. Source identity is the exact pinned pair; encounter
  // order in a catalog chain does not establish chronological composition.
  template <class P, class Blob = profile_blob<P>> struct query_match {
    using policy_type = P;
    using blob_type = Blob;
    using pair_type = std::shared_ptr<blob_type const>;
    pair_type source;
    std::uint64_t ordinal = 0;
    bit_string value;
  };

  // A prepared immutable chain whose augmented head fits in one K-entry window.
  // Exact sample contents remain a construction precondition: metadata checks
  // cannot authenticate arbitrary equal-count samples pushed to index_builder.
  // index_pipeline supplies samples from its pinned producer. Objects retained
  // here must remain immutable through every alias for the owner's lifetime.
  template <class P, class Blob> struct query_root {
    using policy_type = P;
    using blob_type = Blob;
    using pair_type = std::shared_ptr<blob_type const>;
    static_assert(std::is_same_v<typename blob_type::policy_type, P>);

    static query_root build(pair_type source) requires std::is_same_v<Blob, profile_blob<P>>;
    // An owner has already prepared this bounded routing head and bound exact
    // dependencies. Adoption checks chain shapes and cycles, never sample keys.
    static query_root adopt_prepared(pair_type source) {
      validate(source);
      if (source->virtual_size() > P::group_size)
        error_detail::raise<std::invalid_argument>("query root head exceeds one cascade group");
      return query_root(std::move(source));
    }
    pair_type head() const noexcept { return head_; }
    query_cursor<P, Blob> cursor(bit_view query) const;

  private:
    friend struct query_root_builder<P>;
    explicit query_root(pair_type head) : head_(std::move(head)) {}
    pair_type head_;

    static void validate(pair_type const & source) {
      static_assert(P::group_size >= 3);
      if (!source) error_detail::raise<std::invalid_argument>("null query root");
      std::unordered_set<blob_type const *> seen;
      for (auto current = source; current; current = current->target()) {
        if (!seen.insert(current.get()).second) error_detail::raise<std::invalid_argument>("cyclic query chain");
        auto native = current->native().size(), borrowed = current->borrowed().size();
        if (native > std::numeric_limits<std::uint64_t>::max() - borrowed ||
            native + borrowed != current->virtual_size())
          error_detail::raise<std::invalid_argument>("query catalog size mismatch");
        auto groups = current->virtual_size() / P::group_size +
          (current->virtual_size() % P::group_size != 0);
        if (current->group_count() != groups)
          error_detail::raise<std::invalid_argument>("query catalog group count mismatch");
        auto target = current->target();
        if (borrowed != (target ? target->group_count() : 0))
          error_detail::raise<std::invalid_argument>("query chain sample count or target mismatch");
        if (current->cut_lcps().size() != current->group_count())
          error_detail::raise<std::invalid_argument>("query chain cut LCP count mismatch");
      }
    }
  };

  // Shape validation visits the existing chain once without decoding its keys.
  // A head already of size <= K keeps its exact identity and needs no sampler.
  // Larger heads acquire empty-native routing catalogs through one pipeline:
  // a source scan and successively ceil(size/K) samples, with no full-key array.
  // step() inherits the pipeline's entry-work budget; bytes are not bounded by
  // that budget, and finish() separately finalizes compact metadata.
  template <class P> struct query_root_builder {
    using policy_type = P;
    using blob_type = profile_blob<P>;
    using pair_type = std::shared_ptr<blob_type const>;

    explicit query_root_builder(pair_type source) : head_(std::move(source)) {
      query_root<P>::validate(head_);
      auto count = head_->virtual_size();
      std::size_t levels = 0;
      while (count > P::group_size) {
        count = count / P::group_size + (count % P::group_size != 0);
        ++levels;
      }
      if (levels) {
        auto empty = std::make_shared<blob_type const>(blob_type::build({}));
        pipeline_.emplace(head_, std::vector<pair_type>(levels, std::move(empty)));
      }
    }
    query_root_builder(query_root_builder const &) = delete;
    query_root_builder & operator=(query_root_builder const &) = delete;
    query_root_builder(query_root_builder &&) = default;
    query_root_builder & operator=(query_root_builder &&) = default;

    bool done() const noexcept { return !pipeline_ || pipeline_->done(); }
    bool finished() const noexcept { return finished_; }
    std::uint64_t step(std::uint64_t quanta) {
      if (!head_) error_detail::raise<std::logic_error>("query root preparation has no source");
      if (finished_) error_detail::raise<std::logic_error>("query root preparation is finished");
      return pipeline_ ? pipeline_->step(quanta) : 0;
    }
    query_root<P> finish() {
      if (!head_) error_detail::raise<std::logic_error>("query root preparation has no source");
      if (!done()) error_detail::raise<std::logic_error>("query root preparation still has input");
      if (!finished_) {
        if (pipeline_) head_ = pipeline_->finish();
        finished_ = true;
      }
      return query_root<P>(head_);
    }

  private:
    pair_type head_;
    std::optional<index_pipeline<P>> pipeline_;
    bool finished_ = false;
  };

  // The cursor owns its query and current target pin. Each successful step
  // visits at most catalog_budget catalogs and stops at its first native match.
  // Take that owned match before stepping again. Equal native keys do not stop
  // descent: the downstream route is retained for the next call. Visited pairs
  // can be released unless a pending/returned match owns them. Copying a cursor
  // shares its immutable query and copies its context and pending value; copies advance independently
  // while sharing the same immutable suffix and source pins.
  //
  // Search costs O(K+W) entry/header work per visited catalog plus literal
  // comparison and value copying. Root preparation adds O(log_K A) catalogs for a head
  // of A entries. These bounds do not cover arbitrary string bytes, arrow
  // evaluation, disk faults, or a future level scheduler.
  template <class P, class Blob> struct query_cursor {
    using policy_type = P;
    using blob_type = Blob;
    using pair_type = std::shared_ptr<blob_type const>;
    using match_type = query_match<P, Blob>;

    explicit query_cursor(query_root<P, Blob> const & root, bit_view query)
      : current_(root.head()), context_(query) {
      if (!current_) error_detail::raise<std::invalid_argument>("query root has no head");
      if (!current_->virtual_size()) current_.reset();
    }
    bool done() const noexcept { return !current_ && !pending_; }
    bool has_match() const noexcept { return pending_.has_value(); }
    bool failed() const noexcept { return failed_; }

    std::uint64_t step(std::uint64_t catalog_budget = 1) {
      if (failed_) error_detail::raise<std::logic_error>("query cursor has failed");
      if (pending_) return 0;
      std::uint64_t visited = 0;
      try {
        while (current_ && visited != catalog_budget) {
          auto result = current_->search_window(group_, context_);
          auto target = current_->target();
          if (result.borrowed_predecessor) {
            auto const & next = *result.borrowed_predecessor;
            if (!target || next.target_ordinal % P::group_size ||
                next.target_ordinal >= target->virtual_size())
              error_detail::raise<std::invalid_argument>("query descent has no valid target context");
          }
          if (result.native)
            pending_.emplace(match_type{current_, result.native->ordinal, std::move(result.native->value)});
          if (result.borrowed_predecessor) {
            auto & next = *result.borrowed_predecessor;
            group_ = next.target_ordinal / P::group_size;
            context_ = std::move(next.comparison);
            current_ = std::move(target);
          } else {
            // The first borrowed key samples the target's minimum. No borrowed
            // predecessor means no downstream native key can equal this query.
            current_.reset();
          }
          ++visited;
          if (pending_) break;
        }
      } catch (...) {
        failed_ = true;
        throw;
      }
      return visited;
    }
    match_type take_match() {
      if (failed_) error_detail::raise<std::logic_error>("query cursor has failed");
      if (!pending_) error_detail::raise<std::logic_error>("query cursor has no pending match");
      auto result = std::move(*pending_);
      pending_.reset();
      return result;
    }

  private:
    pair_type current_;
    profile_query_context<P> context_;
    std::uint64_t group_ = 0;
    std::optional<match_type> pending_;
    bool failed_ = false;
  };

  template <class P, class Blob>
  query_root<P, Blob> query_root<P, Blob>::build(pair_type source)
      requires std::is_same_v<Blob, profile_blob<P>> {
    query_root_builder<P> builder(std::move(source));
    while (!builder.done()) builder.step(4096);
    return builder.finish();
  }
  template <class P, class Blob>
  query_cursor<P, Blob> query_root<P, Blob>::cursor(bit_view query) const {
    return query_cursor<P, Blob>(*this, query);
  }
}
