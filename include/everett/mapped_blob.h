/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/query.h>
#include <everett/sections.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace everett {
  // Pins two physical files and their exact downstream pair. Bind checks
  // identity declarations and shapes; scan checks contents and sampled keys.
  // The caller/catalog vouches for each owner's opaque physical identity.
  template <class P> struct mapped_blob {
    using policy_type = P;
    using pair_type = std::shared_ptr<mapped_blob const>;
    using native_type = mapped_native<P>;
    using index_type = mapped_index<P>;
    mapped_blob(mapped_blob const &) = delete;
    mapped_blob & operator=(mapped_blob const &) = delete;
    mapped_blob(mapped_blob &&) = delete;
    mapped_blob & operator=(mapped_blob &&) = delete;

    static pair_type bind(blob_identity identity, std::shared_ptr<native_type const> native,
        std::shared_ptr<index_type const> index, pair_type target = {}) {
      if (!native || !index) throw std::invalid_argument("null Everett mapped pair component");
      if (identity.native != index->native_id()) throw std::invalid_argument("Everett index names another native object");
      auto const & expected = index->target_id();
      if (bool(expected) != bool(target) || (expected && *expected != target->identity()))
        throw std::invalid_argument("Everett mapped pair target identity mismatch");
      if (index->borrowed().size() != (target ? target->group_count() : 0))
        throw std::invalid_argument("Everett mapped pair sample count mismatch");
      profile_blob_view<P> view(native->view(), index->borrowed(), index->interleave(),
                                index->false_borrow_bits(), index->cut_lcps(), index->virtual_size());
      return pair_type(new mapped_blob(std::move(identity), std::move(native), std::move(index),
                                        std::move(target), std::move(view)));
    }

    blob_identity const & identity() const & noexcept { return identity_; }
    blob_identity const & identity() const && = delete;
    std::shared_ptr<native_type const> native_object() const noexcept { return native_; }
    std::shared_ptr<index_type const> index_object() const noexcept { return index_; }
    pair_type target() const noexcept { return target_; }
    profile_view<P, stream_role::native> native() const & { return view_.native(); }
    profile_view<P, stream_role::native> native() const && = delete;
    profile_view<P, stream_role::borrowed> borrowed() const & { return view_.borrowed(); }
    profile_view<P, stream_role::borrowed> borrowed() const && = delete;
    profile_blob_view<P> view() const & { return view_; }
    profile_blob_view<P> view() const && = delete;
    rank_groups_view<P::group_size> interleave() const & noexcept { return view_.interleave(); }
    rank_groups_view<P::group_size> interleave() const && = delete;
    word_view cut_lcps() const & noexcept { return view_.cut_lcps(); }
    word_view cut_lcps() const && = delete;
    std::uint64_t virtual_size() const noexcept { return view_.virtual_size(); }
    std::uint64_t group_count() const noexcept { return view_.group_count(); }
    bool false_borrow(std::uint64_t ordinal) const { return view_.false_borrow(ordinal); }
    profile_blob_window project(std::uint64_t group) const { return view_.project(group); }
    profile_blob_window_result<P> search_window(std::uint64_t group,
        profile_query_context<P> const & lower, profile_comparison_work * native_work = nullptr,
        profile_comparison_work * borrowed_work = nullptr) const {
      return view_.search_window(group, lower, native_work, borrowed_work);
    }

    // Explicit complete-chain scan. Reconstructs sequential keys and rebuilds
    // compact directories as scratch; it does not materialize all full keys.
    // Success attests current readable bytes, not recovery from a failed fsync.
    void scan() const {
      std::unordered_set<mapped_blob const *> seen;
      std::unordered_set<native_type const *> native_seen;
      for (auto current = this; current; current = current->target_.get()) {
        if (!seen.insert(current).second) throw std::invalid_argument("cyclic Everett mapped chain");
        if (native_seen.insert(current->native_.get()).second) current->native_->scan();
        current->index_->scan();
        current->scan_pair();
        current->scan_samples();
      }
    }

  private:
    blob_identity identity_;
    std::shared_ptr<native_type const> native_;
    std::shared_ptr<index_type const> index_;
    pair_type target_;
    profile_blob_view<P> view_;

    mapped_blob(blob_identity identity, std::shared_ptr<native_type const> native,
        std::shared_ptr<index_type const> index, pair_type target, profile_blob_view<P> view)
      : identity_(std::move(identity)), native_(std::move(native)), index_(std::move(index)),
        target_(std::move(target)), view_(std::move(view)) {}

    struct merged_cursor {
      profile_cursor<P, stream_role::native> native;
      profile_cursor<P, stream_role::borrowed> borrowed;
      explicit merged_cursor(mapped_blob const & source) : native(source.native()), borrowed(source.borrowed()) {}
      bool done() const noexcept { return native.done() && borrowed.done(); }
      bool is_borrowed() const {
        if (done()) throw std::out_of_range("Everett merged cursor at end");
        return !borrowed.done() && (native.done() ||
          compare_bits(borrowed.peek().key.prefix, native.peek().key.prefix) < 0);
      }
      bit_view key() const { return is_borrowed() ? borrowed.peek().key.prefix : native.peek().key.prefix; }
      void advance() { if (is_borrowed()) borrowed.advance(); else native.advance(); }
    };

    void scan_pair() const {
      merged_cursor cursor(*this);
      bit_string previous_native, previous_borrowed;
      bool had_native = false;
      std::uint64_t ordinal = 0, borrowed_count = 0, population = 0;
      auto ranks = interleave();
      auto cuts = cut_lcps();
      while (!cursor.done()) {
        auto key = cursor.key();
        auto group = ordinal / P::group_size;
        if (ordinal % P::group_size == 0) {
          auto lcp = borrowed_count ? compare_common_bits(previous_borrowed.view(), key).common_bits : 0;
          if (cuts[group] != lcp) throw std::invalid_argument("Everett cut LCP disagrees with keys");
          if (ranks.rank(group) != borrowed_count) throw std::invalid_argument("Everett rank disagrees with interleaving");
          population = 0;
        }
        if (cursor.is_borrowed()) {
          bool expected = had_native && compare_bits(previous_native.view(), key) == 0;
          if (false_borrow(borrowed_count) != expected)
            throw std::invalid_argument("Everett false-borrow flag disagrees with keys");
          previous_borrowed = bit_string::copy(key);
          ++borrowed_count;
          ++population;
        } else {
          previous_native = bit_string::copy(key);
          had_native = true;
        }
        cursor.advance();
        ++ordinal;
        if (ordinal % P::group_size == 0 || cursor.done())
          if (ranks.class_at(group) != population)
            throw std::invalid_argument("Everett rank population disagrees with keys");
      }
      if (ordinal != virtual_size() || borrowed_count != borrowed().size())
        throw std::invalid_argument("Everett merged stream count mismatch");
    }
    void scan_samples() const {
      auto samples = borrowed().cursor();
      if (!target_) {
        if (!samples.done()) throw std::invalid_argument("Everett borrowed stream has no target");
        return;
      }
      merged_cursor target(*target_);
      std::uint64_t ordinal = 0;
      while (!target.done()) {
        if (ordinal % P::group_size == 0) {
          if (samples.done() || compare_bits(samples.peek().key.prefix, target.key()) != 0)
            throw std::invalid_argument("Everett borrowed key is not the exact target sample");
          samples.advance();
        }
        target.advance();
        ++ordinal;
      }
      if (!samples.done()) throw std::invalid_argument("Everett trailing borrowed target samples");
    }
  };

  template <class P> using mapped_query_root = query_root<P, mapped_blob<P>>;

  // Resolve the exact on-disk links using only fixed metadata, cache shared
  // native mappings within this chain, and adopt its already-prepared head.
  // This reads no FC payload or rank/EF words and never rebuilds an index.
  template <class P> mapped_query_root<P> open_mapped_query(std::filesystem::path const & root,
                                                          blob_identity const & head) {
    using native_pointer = std::shared_ptr<mapped_native<P> const>;
    using index_pointer = std::shared_ptr<mapped_index<P> const>;
    struct pending_pair { blob_identity identity; native_pointer native; index_pointer index; };
    std::vector<pending_pair> pending;
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, native_pointer> natives;
    std::optional<blob_identity> current = head;
    while (current) {
      if (!seen.insert(current->index.hex()).second) throw std::invalid_argument("cyclic Everett object identities");
      auto index = std::make_shared<mapped_index<P> const>(
        mapped_index<P>::open(root / object_path(current->index, file_kind::fractional_index)));
      if (index->native_id() != current->native) throw std::invalid_argument("Everett chain native identity mismatch");
      auto found = natives.find(current->native.hex());
      if (found == natives.end()) {
        auto native = std::make_shared<mapped_native<P> const>(
          mapped_native<P>::open(root / object_path(current->native, file_kind::native_blob)));
        found = natives.emplace(current->native.hex(), std::move(native)).first;
      }
      pending.push_back({*current, found->second, index});
      current = index->target_id();
    }
    typename mapped_blob<P>::pair_type pair;
    for (auto i = pending.size(); i-- > 0;)
      pair = mapped_blob<P>::bind(std::move(pending[i].identity), std::move(pending[i].native),
                                  std::move(pending[i].index), std::move(pair));
    return mapped_query_root<P>::adopt_prepared(std::move(pair));
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Binds immutable mapped blobs to exact dependency chains and validates their samples.
 */
