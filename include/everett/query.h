/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/index_pipeline.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace everett {
  template <class P> struct query_root;
  template <class P> struct query_root_builder;
  template <class P> struct query_cursor;

  // One native segment. Source identity is the exact pinned pair; encounter
  // order in a catalog chain does not establish chronological composition.
  template <class P> struct query_match {
    using policy_type = P;
    using pair_type = std::shared_ptr<profile_blob<P> const>;
    pair_type source;
    std::uint64_t ordinal = 0;
    bit_string value;
  };

  // A prepared immutable chain whose augmented head fits in one K-entry window.
  // Exact sample contents remain a construction precondition: metadata checks
  // cannot authenticate arbitrary equal-count samples pushed to index_builder.
  // index_pipeline supplies samples from its pinned producer. Objects retained
  // here must remain immutable through every alias for the owner's lifetime.
  template <class P> struct query_root {
    using policy_type = P;
    using blob_type = profile_blob<P>;
    using pair_type = std::shared_ptr<blob_type const>;

    static query_root build(pair_type source);
    pair_type head() const noexcept { return head_; }
    query_cursor<P> cursor(bit_view query) const;

  private:
    friend struct query_root_builder<P>;
    explicit query_root(pair_type head) : head_(std::move(head)) {}
    pair_type head_;
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
      validate(head_);
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
      if (!head_) throw std::logic_error("query root preparation has no source");
      if (finished_) throw std::logic_error("query root preparation is finished");
      return pipeline_ ? pipeline_->step(quanta) : 0;
    }
    query_root<P> finish() {
      if (!head_) throw std::logic_error("query root preparation has no source");
      if (!done()) throw std::logic_error("query root preparation still has input");
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

    static void validate(pair_type const & source) {
      static_assert(P::group_size >= 3);
      if (!source) throw std::invalid_argument("null query root");
      std::unordered_set<blob_type const *> seen;
      for (auto current = source; current; current = current->target()) {
        if (!seen.insert(current.get()).second) throw std::invalid_argument("cyclic query chain");
        auto native = current->native().size(), borrowed = current->borrowed().size();
        if (native > std::numeric_limits<std::uint64_t>::max() - borrowed ||
            native + borrowed != current->virtual_size())
          throw std::invalid_argument("query catalog size mismatch");
        auto target = current->target();
        if (borrowed != (target ? target->group_count() : 0))
          throw std::invalid_argument("query chain sample count or target mismatch");
        auto policy = current->borrowed_policy();
        if (policy != profile_borrowed_policy::shared_boundaries &&
            policy != profile_borrowed_policy::bidirectional &&
            !(policy == profile_borrowed_policy::ordinary && !borrowed))
          throw std::invalid_argument("query chain requires bounded borrowed context");
      }
    }
  };

  // The cursor owns its query and current target pin. Each successful step
  // visits at most catalog_budget catalogs and stops at its first native match.
  // Take that owned match before stepping again. Equal native keys do not stop
  // descent: the downstream route is retained for the next call. Visited pairs
  // can be released unless a pending/returned match owns them. Copying a cursor
  // copies its query, context and pending value; copies then advance independently
  // while sharing the same immutable suffix and source pins.
  //
  // Search costs O(K) entry/header work per visited catalog plus prefix/value
  // copying and decoding. Root preparation adds O(log_K A) catalogs for a head
  // of A entries. These bounds do not cover arbitrary string bytes, arrow
  // evaluation, disk faults, or a future level scheduler.
  template <class P> struct query_cursor {
    using policy_type = P;
    using blob_type = profile_blob<P>;
    using pair_type = std::shared_ptr<blob_type const>;
    using match_type = query_match<P>;

    explicit query_cursor(query_root<P> const & root, bit_view query) : current_(root.head()) {
      if (!current_) throw std::invalid_argument("query root has no head");
      if (query.size() % P::bits_per_unit)
        throw std::invalid_argument("query length is not aligned to the profile unit");
      query_ = bit_string::copy(query);
      if (!current_->virtual_size()) current_.reset();
    }
    bool done() const noexcept { return !current_ && !pending_; }
    bool has_match() const noexcept { return pending_.has_value(); }
    bool failed() const noexcept { return failed_; }

    std::uint64_t step(std::uint64_t catalog_budget = 1) {
      if (failed_) throw std::logic_error("query cursor has failed");
      if (pending_) return 0;
      std::uint64_t visited = 0;
      try {
        while (current_ && visited != catalog_budget) {
          auto result = current_->search_window(query_.view(), group_, {anchor_.view(), anchor_units_});
          auto target = current_->target();
          if (result.borrowed_predecessor) {
            auto const & next = *result.borrowed_predecessor;
            if (!target || !next.has_context || next.target_ordinal % P::group_size ||
                next.target_ordinal >= target->virtual_size())
              throw std::invalid_argument("query descent has no valid target context");
          }
          if (result.native)
            pending_.emplace(match_type{current_, result.native->ordinal, std::move(result.native->value)});
          if (result.borrowed_predecessor) {
            auto & next = *result.borrowed_predecessor;
            group_ = next.target_ordinal / P::group_size;
            anchor_ = std::move(next.prefix);
            // The prefix can be query-limited; its size is not the key's full
            // length needed by front-coded backspace/reconstruction context.
            anchor_units_ = next.full_units;
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
      if (failed_) throw std::logic_error("query cursor has failed");
      if (!pending_) throw std::logic_error("query cursor has no pending match");
      auto result = std::move(*pending_);
      pending_.reset();
      return result;
    }

  private:
    pair_type current_;
    bit_string query_;
    bit_string anchor_;
    std::uint64_t anchor_units_ = 0;
    std::uint64_t group_ = 0;
    std::optional<match_type> pending_;
    bool failed_ = false;
  };

  template <class P> query_root<P> query_root<P>::build(pair_type source) {
    query_root_builder<P> builder(std::move(source));
    while (!builder.done()) builder.step(4096);
    return builder.finish();
  }
  template <class P> query_cursor<P> query_root<P>::cursor(bit_view query) const {
    return query_cursor<P>(*this, query);
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Prepares immutable catalog chains and enumerates every matching native segment.
 */
