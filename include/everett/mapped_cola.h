/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Binds mapped COLA main and secondary targets with exact immutable pins.
 *
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
#include <set>
#include <utility>
#include <vector>

namespace everett {
  template <class Blob> void scan_mapped_cola(Blob const & source);
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
    void scan() const { scan_mapped_cola(*this); }
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

  };

  // Explicit shared recovery context. Each mapped native and index is scanned
  // once even when several checkpoint roots share their downstream suffix.
  template <class Blob> struct mapped_cola_scan {
    using P = typename Blob::policy_type;
    using native_type = typename Blob::native_type;
    using index_type = typename Blob::index_type;
    void operator()(Blob const & head) {
      if (failed_) throw std::logic_error("failed COLA recovery context");
      try { scan(head); } catch (...) { failed_ = true; throw; }
    }
  private:
    bool failed_ = false;
    // Keep control-block identities even after the caller releases a mapping.
    // A later allocation at the same address must still be scanned.
    std::set<std::weak_ptr<index_type const>, std::owner_less<>> indexes_;
    std::set<std::weak_ptr<native_type const>, std::owner_less<>> natives_;
    void scan(Blob const & head) {
      std::unordered_set<Blob const *> path;
      for (auto current = &head; current; current = current->main_target().get()) {
        if (!path.insert(current).second) throw std::invalid_argument("cyclic COLA recovery graph");
        auto index = current->index_object();
        if (!indexes_.insert(index).second) break;
        auto native = current->native_object(), secondary = current->secondary_target();
        if (natives_.insert(native).second) native->scan();
        if (secondary && natives_.insert(secondary).second) secondary->scan();
        index->scan();
        scan_routes(*current);
      }
    }
    static void scan_routes(Blob const & source) {
      auto view = source.view();
      auto native = view.native().cursor();
      std::array borrowed{view.borrowed(0).cursor(), view.borrowed(1).cursor()};
      using native_cursor = decltype(native);
      using borrowed_cursors = decltype(borrowed);
      struct merged {
        native_cursor & native;
        borrowed_cursors & borrowed;
        bool done() const noexcept { return native.done() && borrowed[0].done() && borrowed[1].done(); }
        unsigned origin() const {
          unsigned result = 3; bit_view best;
          if (!native.done()) { result = 0; best = native.peek().key.prefix; }
          for (unsigned i = 0; i != 2; ++i)
            if (!borrowed[i].done() && (result == 3 || compare_bits<typename P::architecture>(borrowed[i].peek().key.prefix, best) < 0)) {
              result = i + 1; best = borrowed[i].peek().key.prefix;
            }
          if (result == 3) throw std::out_of_range("COLA recovery cursor at end");
          return result;
        }
        bit_view key(unsigned origin) const { return origin ? borrowed[origin - 1].peek().key.prefix : native.peek().key.prefix; }
        void advance(unsigned origin) { if (origin) borrowed[origin - 1].advance(); else native.advance(); }
      } cursor{native, borrowed};
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
            auto lcp = borrowed_count[route] ? compare_common_bits<typename P::architecture>(previous_borrowed[route].view(), key).common_bits : 0;
            if (view.cut_lcps(route)[group] != lcp || view.interleave(route).template rank<typename P::architecture>(group) != borrowed_count[route])
              error_detail::raise<std::invalid_argument>("COLA cut or rank disagrees with keys");
          }
          population = {};
        }
        if (origin) {
          auto route = origin - 1;
          bool expected = had_native && compare_bits<typename P::architecture>(previous_native.view(), key) == 0;
          if (view.false_borrow(route, borrowed_count[route]) != expected)
            error_detail::raise<std::invalid_argument>("COLA false-borrow flag disagrees with keys");
          previous_borrowed[route] = bit_string::copy(key);
          ++borrowed_count[route]; ++population[route];
        } else { previous_native = bit_string::copy(key); had_native = true; }
        cursor.advance(origin);
        ++ordinal;
        if (ordinal % P::group_size == 0 || cursor.done())
          for (unsigned route = 0; route != 2; ++route)
            if (view.interleave(route).class_at(group) != population[route])
              error_detail::raise<std::invalid_argument>("COLA population disagrees with keys");
      }
      if (ordinal != source.virtual_size() || borrowed_count[0] != view.borrowed(0).size() ||
          borrowed_count[1] != view.borrowed(1).size())
        error_detail::raise<std::invalid_argument>("COLA merged stream count mismatch");
      auto samples = view.borrowed(0).cursor();
      if (auto main = source.main_target()) {
        cola_sample_cursor<P, Blob> target(main);
        while (!target.done()) {
          if (samples.done() || compare_bits<typename P::architecture>(samples.peek().key.prefix, target.peek().key) != 0)
            error_detail::raise<std::invalid_argument>("COLA main sample differs from target");
          samples.advance(); target.advance();
        }
      }
      if (!samples.done()) error_detail::raise<std::invalid_argument>("COLA trailing main samples");
      auto side_samples = view.borrowed(1).cursor();
      if (auto secondary = source.secondary_target()) {
        auto target = secondary->view().cursor();
        std::uint64_t ordinal = 0;
        while (!target.done()) {
          if (ordinal % P::group_size == 0) {
            if (side_samples.done() || compare_bits<typename P::architecture>(side_samples.peek().key.prefix, target.peek().key.prefix) != 0)
              error_detail::raise<std::invalid_argument>("COLA secondary sample differs from target");
            side_samples.advance();
          }
          target.advance(); ++ordinal;
        }
      }
      if (!side_samples.done()) error_detail::raise<std::invalid_argument>("COLA trailing secondary samples");
    }
  };
  template <class Blob> void scan_mapped_cola(Blob const & source) { mapped_cola_scan<Blob>{}(source); }

  // Build only new borrowed payload/navigation while retaining received native
  // bytes and exact mapped targets. Seal this artifact's IX03 sections, then
  // bind the result as a mapped_cola_blob for homogeneous query traversal.
  template <class P> using mapped_cola_artifact = cola_index<P, mapped_native<P>, mapped_cola_blob<P>>;
  template <class P> using mapped_cola_index_builder = cola_index_builder<P, mapped_native<P>, mapped_cola_blob<P>>;

  template <class P> using mapped_cola_query_root = cola_query_root<P, mapped_cola_blob<P>>;

  // Interns exact immutable targets across every root of a checkpoint. Opening
  // reads directories only; payload scans remain explicit recovery operations.
  template <class P, class Blob = mapped_cola_blob<P>> struct mapped_cola_resolver {
    using blob = Blob;
    using native_type = typename blob::native_type;
    using index_type = typename blob::index_type;
    using native_pointer = typename blob::native_pointer;
    using pair_type = typename blob::pair_type;
    explicit mapped_cola_resolver(std::filesystem::path root) : root_(std::move(root)) {}
    native_pointer native(object_id const & id) {
      auto found = natives_.find(id.hex());
      if (found == natives_.end()) found = natives_.emplace(id.hex(), std::make_shared<native_type const>(
        native_type::open(root_ / object_path(id, file_kind::native_blob)))).first;
      return found->second;
    }
    pair_type pair(blob_identity const & head) {
      struct pending { blob_identity identity; native_pointer native, secondary; typename blob::index_pointer index; };
      std::vector<pending> chain;
      std::unordered_set<std::string> seen;
      std::optional<blob_identity> current = head;
      pair_type target;
      while (current) {
        if (auto known = pairs_.find(current->index.hex()); known != pairs_.end()) {
          if (known->second->identity() != *current)
            error_detail::raise<std::invalid_argument>("COLA reused index has another native identity");
          target = known->second; break;
        }
        if (!seen.insert(current->index.hex()).second)
          error_detail::raise<std::invalid_argument>("cyclic COLA object identities");
        auto index = std::make_shared<index_type const>(
          index_type::open(root_ / object_path(current->index, file_kind::fractional_index)));
        if (index->native_id() != current->native)
          error_detail::raise<std::invalid_argument>("COLA chain native identity mismatch");
        chain.push_back({*current, native(current->native), index->secondary_id() ? native(*index->secondary_id()) : native_pointer{}, index});
        current = index->main_id();
      }
      for (auto i = chain.rbegin(); i != chain.rend(); ++i) {
        target = blob::bind(i->identity, std::move(i->native), i->index,
          std::move(target), std::move(i->secondary), i->index->secondary_id());
        pairs_.emplace(i->identity.index.hex(), target);
      }
      return target;
    }
  private:
    std::filesystem::path root_;
    std::unordered_map<std::string, native_pointer> natives_;
    std::unordered_map<std::string, pair_type> pairs_;
  };

  template <class P> mapped_cola_query_root<P> open_mapped_cola_query(
      std::filesystem::path const & root, blob_identity const & head) {
    mapped_cola_resolver<P> resolver(root);
    return mapped_cola_query_root<P>::adopt_prepared(resolver.pair(head));
  }
}
