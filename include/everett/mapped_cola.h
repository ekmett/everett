/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/cola_query.h>
#include <everett/cola_sections.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace everett {
  // Each main node pins its own KV02/IX03 pair, the exact next main pair, and
  // the next secondary's native file. Secondary search ends there. Identities
  // are caller-authenticated declarations; binding checks them and shapes.
  template <class P> struct mapped_cola_blob {
    using policy_type = P;
    using native_type = mapped_native<P>;
    using native_pointer = std::shared_ptr<native_type const>;
    using index_type = mapped_cola_index<P>;
    using index_pointer = std::shared_ptr<index_type const>;
    using pair_type = std::shared_ptr<mapped_cola_blob const>;
    mapped_cola_blob(mapped_cola_blob const &) = delete;
    mapped_cola_blob & operator=(mapped_cola_blob const &) = delete;
    mapped_cola_blob(mapped_cola_blob &&) = delete;
    mapped_cola_blob & operator=(mapped_cola_blob &&) = delete;

    static pair_type bind(blob_identity identity, native_pointer native, index_pointer index,
        pair_type main = {}, native_pointer secondary = {}, std::optional<object_id> secondary_id = {}) {
      if (!native || !index || identity.native != index->native_id())
        error_detail::raise<std::invalid_argument>("invalid COLA mapped pair components");
      if (bool(index->main_id()) != bool(main) || (main && *index->main_id() != main->identity()))
        error_detail::raise<std::invalid_argument>("COLA main target identity mismatch");
      if (bool(secondary) != bool(secondary_id) || secondary_id != index->secondary_id())
        error_detail::raise<std::invalid_argument>("COLA secondary target identity mismatch");
      auto secondary_count = secondary ? secondary->size() : 0;
      if (index->borrowed(0).size() != (main ? main->group_count() : 0) ||
          index->borrowed(1).size() != secondary_count / P::group_size + (secondary_count % P::group_size != 0))
        error_detail::raise<std::invalid_argument>("COLA mapped target sample count mismatch");
      auto view = index->view(native->view());
      return pair_type(new mapped_cola_blob(std::move(identity), std::move(native), std::move(index),
        std::move(main), std::move(secondary), std::move(view)));
    }
    blob_identity const & identity() const & noexcept { return identity_; }
    blob_identity const & identity() const && = delete;
    native_pointer native_object() const noexcept { return native_; }
    index_pointer index_object() const noexcept { return index_; }
    pair_type main_target() const noexcept { return main_; }
    native_pointer secondary_target() const noexcept { return secondary_; }
    cola_index_view<P> view() const & { return view_; }
    cola_index_view<P> view() const && = delete;
    std::uint64_t virtual_size() const noexcept { return view_.virtual_size(); }
    std::uint64_t group_count() const noexcept { return view_.group_count(); }

    // Explicit recovery/validation walk. Rebuilds compact rank/EF metadata and
    // reconstructs sequential key frontiers, never an array of every full key.
    void scan() const {
      std::unordered_set<mapped_cola_blob const *> seen;
      std::unordered_set<native_type const *> native_seen;
      for (auto current = this; current; current = current->main_.get()) {
        if (!seen.insert(current).second) error_detail::raise<std::invalid_argument>("cyclic COLA mapped chain");
        if (native_seen.insert(current->native_.get()).second) current->native_->scan();
        if (current->secondary_ && native_seen.insert(current->secondary_.get()).second) current->secondary_->scan();
        current->index_->scan();
        current->scan_pair();
        current->scan_samples();
      }
    }
  private:
    blob_identity identity_;
    native_pointer native_;
    index_pointer index_;
    pair_type main_;
    native_pointer secondary_;
    cola_index_view<P> view_;
    mapped_cola_blob(blob_identity identity, native_pointer native, index_pointer index,
        pair_type main, native_pointer secondary, cola_index_view<P> view)
      : identity_(std::move(identity)), native_(std::move(native)), index_(std::move(index)),
        main_(std::move(main)), secondary_(std::move(secondary)), view_(std::move(view)) {}

    struct merged_cursor {
      profile_cursor<P> native;
      std::array<profile_cursor<P, stream_role::borrowed>, 2> borrowed;
      explicit merged_cursor(cola_index_view<P> source)
        : native(source.native()), borrowed{profile_cursor<P, stream_role::borrowed>(source.borrowed(0)),
          profile_cursor<P, stream_role::borrowed>(source.borrowed(1))} {}
      bool done() const noexcept { return native.done() && borrowed[0].done() && borrowed[1].done(); }
      unsigned origin() const {
        unsigned result = 3;
        bit_view best;
        if (!native.done()) { result = 0; best = native.peek().key.prefix; }
        for (unsigned i = 0; i != 2; ++i)
          if (!borrowed[i].done() && (result == 3 || compare_bits(borrowed[i].peek().key.prefix, best) < 0)) {
            result = i + 1; best = borrowed[i].peek().key.prefix;
          }
        if (result == 3) error_detail::raise<std::out_of_range>("COLA merged cursor at end");
        return result;
      }
      bit_view key(unsigned origin) const { return origin ? borrowed[origin - 1].peek().key.prefix : native.peek().key.prefix; }
      void advance(unsigned origin) { if (origin) borrowed[origin - 1].advance(); else native.advance(); }
    };
    void scan_pair() const {
      merged_cursor cursor(view_);
      bit_string previous_native;
      std::array<bit_string, 2> previous_borrowed;
      std::array<std::uint64_t, 2> borrowed_count{}, population{};
      bool had_native = false;
      std::uint64_t ordinal = 0;
      while (!cursor.done()) {
        auto origin = cursor.origin();
        auto key = cursor.key(origin);
        auto group = ordinal / P::group_size;
        if (ordinal % P::group_size == 0) {
          for (unsigned route = 0; route != 2; ++route) {
            auto lcp = borrowed_count[route] ? compare_common_bits(previous_borrowed[route].view(), key).common_bits : 0;
            if (view_.cut_lcps(route)[group] != lcp || view_.interleave(route).rank(group) != borrowed_count[route])
              error_detail::raise<std::invalid_argument>("COLA cut or rank disagrees with keys");
          }
          population = {};
        }
        if (origin) {
          auto route = origin - 1;
          bool expected = had_native && compare_bits(previous_native.view(), key) == 0;
          if (view_.false_borrow(route, borrowed_count[route]) != expected)
            error_detail::raise<std::invalid_argument>("COLA false-borrow flag disagrees with keys");
          previous_borrowed[route] = bit_string::copy(key);
          ++borrowed_count[route]; ++population[route];
        } else { previous_native = bit_string::copy(key); had_native = true; }
        cursor.advance(origin);
        ++ordinal;
        if (ordinal % P::group_size == 0 || cursor.done())
          for (unsigned route = 0; route != 2; ++route)
            if (view_.interleave(route).class_at(group) != population[route])
              error_detail::raise<std::invalid_argument>("COLA population disagrees with keys");
      }
      if (ordinal != virtual_size() || borrowed_count[0] != view_.borrowed(0).size() ||
          borrowed_count[1] != view_.borrowed(1).size())
        error_detail::raise<std::invalid_argument>("COLA merged stream count mismatch");
    }
    void scan_samples() const {
      auto samples = view_.borrowed(0).cursor();
      if (main_) {
        cola_sample_cursor<P, mapped_cola_blob> target(main_);
        while (!target.done()) {
          if (samples.done() || compare_bits(samples.peek().key.prefix, target.peek().key) != 0)
            error_detail::raise<std::invalid_argument>("COLA main sample differs from target");
          samples.advance(); target.advance();
        }
      }
      if (!samples.done()) error_detail::raise<std::invalid_argument>("COLA trailing main samples");
      auto side_samples = view_.borrowed(1).cursor();
      if (secondary_) {
        auto target = secondary_->view().cursor();
        std::uint64_t ordinal = 0;
        while (!target.done()) {
          if (ordinal % P::group_size == 0) {
            if (side_samples.done() || compare_bits(side_samples.peek().key.prefix, target.peek().key.prefix) != 0)
              error_detail::raise<std::invalid_argument>("COLA secondary sample differs from target");
            side_samples.advance();
          }
          target.advance(); ++ordinal;
        }
      }
      if (!side_samples.done()) error_detail::raise<std::invalid_argument>("COLA trailing secondary samples");
    }
  };

  template <class P> using mapped_cola_query_root = cola_query_root<P, mapped_cola_blob<P>>;

  // Resolves exact targets using fixed metadata; native mappings shared by
  // main/secondary roles are cached. It never scans or rebuilds a payload.
  template <class P> mapped_cola_query_root<P> open_mapped_cola_query(
      std::filesystem::path const & root, blob_identity const & head) {
    using blob = mapped_cola_blob<P>;
    using native_pointer = typename blob::native_pointer;
    struct pending { blob_identity identity; native_pointer native, secondary; typename blob::index_pointer index; };
    std::vector<pending> chain;
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, native_pointer> natives;
    auto native = [&](object_id const & id) {
      auto found = natives.find(id.hex());
      if (found == natives.end()) found = natives.emplace(id.hex(), std::make_shared<mapped_native<P> const>(
        mapped_native<P>::open(root / object_path(id, file_kind::native_blob)))).first;
      return found->second;
    };
    std::optional<blob_identity> current = head;
    while (current) {
      if (!seen.insert(current->index.hex()).second) error_detail::raise<std::invalid_argument>("cyclic COLA object identities");
      auto index = std::make_shared<mapped_cola_index<P> const>(
        mapped_cola_index<P>::open(root / object_path(current->index, file_kind::fractional_index)));
      if (index->native_id() != current->native) error_detail::raise<std::invalid_argument>("COLA chain native identity mismatch");
      chain.push_back({*current, native(current->native), index->secondary_id() ? native(*index->secondary_id()) : native_pointer{}, index});
      current = index->main_id();
    }
    typename blob::pair_type pair;
    for (auto i = chain.size(); i-- > 0;)
      pair = blob::bind(std::move(chain[i].identity), std::move(chain[i].native), chain[i].index,
        std::move(pair), std::move(chain[i].secondary), chain[i].index->secondary_id());
    return mapped_cola_query_root<P>::adopt_prepared(std::move(pair));
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Binds mapped COLA main and secondary targets with exact immutable pins.
 */
