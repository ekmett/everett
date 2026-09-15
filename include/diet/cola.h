/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include "diet/pins.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace diet {
  template <class V> struct cola_record {
    std::string key;
    std::optional<V> value;
    bool operator==(cola_record const &) const = default;
  };

  template <class V> struct cola_edit {
    std::string key;
    std::optional<V> before;
    std::optional<V> after;
    bool operator==(cola_edit const &) const = default;
  };

  template <class V> struct cola_run {
    explicit cola_run(std::vector<cola_record<V>> records)
      : records_(std::move(records)), object_id_(next_identity()) {}

    std::span<cola_record<V> const> records() const noexcept { return records_; }
    std::string const & object_id() const noexcept { return object_id_; }

  private:
    static std::string next_identity() {
      // Reference runs have process-local identities. Durable blob stores must
      // assign stable object identities of their own; signatures never do so.
      static std::atomic<std::uint64_t> sequence = 0;
      return "reference-run-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    }

    std::vector<cola_record<V>> records_;
    std::string object_id_;
  };

  namespace cola_detail {
    inline void write_u64(std::ostream & out, std::uint64_t value) {
      std::array<char, 8> bytes;
      for (unsigned i = 0; i < 8; ++i) bytes[i] = char((value >> (8 * i)) & 255);
      out.write(bytes.data(), bytes.size());
      if (!out) throw std::runtime_error("cola debug export write failed");
    }

    inline std::uint64_t read_u64(std::istream & in) {
      std::array<unsigned char, 8> bytes;
      in.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
      if (!in) throw std::runtime_error("truncated cola debug export");
      std::uint64_t value = 0;
      for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
      return value;
    }
  }

  struct u64_cola_codec {
    // The codec tag identifies values in debug resolved-table dumps.
    // Custom fixed-width values supply a codec.
    static constexpr std::uint64_t format_tag = 1;
    static void write(std::ostream & out, std::uint64_t value) {
      cola_detail::write_u64(out, value);
    }
    static std::uint64_t read(std::istream & in) { return cola_detail::read_u64(in); }
  };

  // Allocation bounds for debug_import of a resolved-table dump.
  struct cola_import_limits {
    std::uint64_t max_records = 1'000'000;
    std::uint64_t max_key_bytes = 64 * 1024 * 1024;
  };

  template <class V, class A, class H, class P> struct partition_round;

  // Semantic reference implementation. A snapshot pins immutable sorted runs,
  // and updates copy only the pin vector, never earlier record arrays. This has
  // no redundant COLA scheduling or front-coded disk layout yet. It is an oracle
  // for that implementation, not a claim of its asymptotic space/time bounds.
  template <class V = std::uint64_t, class A = wrapping_fingerprint_algebra,
    class H = u64_table_hash>
  struct reference_cola {
    using value_type = V;
    using element = typename A::element;
    using record = cola_record<V>;
    using run = cola_run<V>;
    using pin = std::shared_ptr<run const>;
    using pin_owner = pin_set<run, A>;

    explicit reference_cola(H hash = {})
      : state_(std::make_shared<state>()), hash_(std::move(hash)) {}

    static reference_cola from_records(std::vector<record> records, H hash = {}) {
      std::sort(records.begin(), records.end(),
        [](record const & a, record const & b) { return a.key < b.key; });
      auto result = reference_cola(std::move(hash));
      auto next = std::make_shared<state>();
      auto signature = A::zero();
      for (std::size_t i = 0; i < records.size(); ++i) {
        if (!records[i].value) throw std::invalid_argument("initial record is a tombstone");
        if (i && records[i - 1].key == records[i].key)
          throw std::invalid_argument("duplicate initial cola key");
        signature = A::add(signature,
          table_fingerprint<A>::binding(records[i].key, records[i].value, result.hash_));
      }
      next->live_size = records.size();
      if (!records.empty()) {
        auto object = std::make_shared<run>(std::move(records));
        next->owner = next->owner.add({object->object_id(), object, signature, signature});
      }
      result.state_ = std::move(next);
      return result;
    }

    std::optional<V> get(std::string_view key) const {
      auto objects = state_->owner.pins();
      for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
        auto entries = (*it)->records();
        auto found = std::lower_bound(entries.begin(), entries.end(), key,
          [](record const & entry, std::string_view k) { return entry.key < k; });
        if (found != entries.end() && found->key == key) return found->value;
      }
      return std::nullopt;
    }

    std::size_t live_size() const noexcept { return state_->live_size; }
    element signature() const { return state_->owner.signature(); }
    std::span<pin const> pins() const noexcept { return state_->owner.pins(); }
    pin_owner const & owner() const noexcept { return state_->owner; }

    // A snapshot/fork shares this exact immutable state and pin set.
    reference_cola snapshot() const { return *this; }

    std::vector<record> resolved() const {
      std::map<std::string, std::optional<V>, std::less<>> table;
      for (auto const & run_pin : state_->owner.pins())
        for (auto const & entry : run_pin->records()) table[entry.key] = entry.value;
      std::vector<record> result;
      result.reserve(state_->live_size);
      for (auto const & [key, value] : table)
        if (value) result.push_back({key, value});
      return result;
    }

    element recompute_signature() const {
      auto result = A::zero();
      for (auto const & entry : resolved())
        result = A::add(result, table_fingerprint<A>::binding(entry.key, entry.value, hash_));
      return result;
    }

    // Eager reference compaction. Old snapshots retain their original pins.
    reference_cola compact() const {
      auto result = from_records(resolved(), hash_);
      std::vector<std::string> inputs;
      for (auto const & item : state_->owner.entries()) inputs.push_back(item.object_id);
      auto replacements = result.owner().entries();
      auto next = std::make_shared<state>();
      next->owner = state_->owner.replace(inputs,
        std::vector<typename pin_owner::entry>(replacements.begin(), replacements.end()));
      next->live_size = state_->live_size;
      result.state_ = std::move(next);
      return result;
    }

    // Debug resolved-table dump (.rc): every live entry in sorted order, with
    // full keys without front coding followed by codec values. This is not an
    // intended access pattern; catalog saves retain object roots separately.
    // Callers own atomic file replacement, durability and codec/hash agreement.
    template <class C = u64_cola_codec>
    void debug_export(std::ostream & out, C codec = {}) const {
      constexpr std::string_view magic{"DIET.RC\0", 8};
      out.write(magic.data(), magic.size());
      cola_detail::write_u64(out, 1);
      cola_detail::write_u64(out, C::format_tag);
      cola_detail::write_u64(out, live_size());
      for (auto const & entry : resolved()) {
        cola_detail::write_u64(out, entry.key.size());
        out.write(entry.key.data(), static_cast<std::streamsize>(entry.key.size()));
        codec.write(out, *entry.value);
      }
      if (!out) throw std::runtime_error("cola debug export write failed");
    }

    // Materializes a fresh reference table from a debug dump, rather than
    // reopening pinned object roots.
    template <class C = u64_cola_codec>
    static reference_cola debug_import(std::istream & in, C codec = {}, H hash = {},
      cola_import_limits limits = {}) {
      std::array<char, 8> magic;
      in.read(magic.data(), magic.size());
      if (!in || std::string_view(magic.data(), magic.size()) != std::string_view{"DIET.RC\0", 8})
        throw std::runtime_error("invalid cola debug export header");
      if (cola_detail::read_u64(in) != 1)
        throw std::runtime_error("unsupported cola debug export version");
      if (cola_detail::read_u64(in) != C::format_tag)
        throw std::runtime_error("cola debug export value codec mismatch");
      auto count = cola_detail::read_u64(in);
      if (count > limits.max_records || count > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("cola debug export record limit");
      std::vector<record> entries;
      entries.reserve(static_cast<std::size_t>(count));
      std::uint64_t remaining = limits.max_key_bytes;
      for (std::uint64_t i = 0; i < count; ++i) {
        auto size = cola_detail::read_u64(in);
        if (size > remaining || size > std::numeric_limits<std::size_t>::max() ||
            size > std::uint64_t(std::numeric_limits<std::streamsize>::max()))
          throw std::runtime_error("cola debug export key byte limit");
        remaining -= size;
        std::string key(static_cast<std::size_t>(size), '\0');
        in.read(key.data(), static_cast<std::streamsize>(size));
        if (!in) throw std::runtime_error("truncated cola debug export key");
        if (!entries.empty() && !(entries.back().key < key))
          throw std::runtime_error("cola debug export keys are not strictly sorted");
        entries.push_back({std::move(key), codec.read(in)});
      }
      return from_records(std::move(entries), std::move(hash));
    }

  private:
    struct state {
      pin_owner owner;
      std::size_t live_size = 0;
    };

    reference_cola append(std::vector<cola_edit<V>> const & edits) const {
      auto next = std::make_shared<state>(*state_);
      std::vector<record> records;
      records.reserve(edits.size());
      auto contribution = A::zero();
      auto native_signature = A::zero();
      for (auto const & edit : edits) {
        contribution = A::add(contribution,
          table_fingerprint<A>::delta(edit.key, edit.before, edit.after, hash_));
        native_signature = A::add(native_signature,
          table_fingerprint<A>::binding(edit.key, edit.after, hash_));
        if (!edit.before && edit.after) ++next->live_size;
        if (edit.before && !edit.after) --next->live_size;
        records.push_back({edit.key, edit.after});
      }
      if (!records.empty()) {
        auto object = std::make_shared<run>(std::move(records));
        next->owner = next->owner.add({object->object_id(), object, contribution, native_signature});
      }
      reference_cola result(*this);
      result.state_ = std::move(next);
      return result;
    }

    template <class, class, class, class> friend struct partition_round;
    std::shared_ptr<state const> state_;
    H hash_;
  };

  template <class V, class A = wrapping_fingerprint_algebra> struct cola_batch {
    // round_id is supplied by the session protocol and is not derived from the
    // collision-prone fingerprint. All peers must agree on its starting cola.
    std::string round_id;
    std::string batch_id;
    std::uint64_t owner = 0;
    typename A::element base_signature = A::zero();
    // Advertised additive contribution of the complete changeset. Receivers
    // recompute it from the validated old/new bindings before accepting it.
    typename A::element delta = A::zero();
    std::vector<cola_edit<V>> edits;
    bool operator==(cola_batch const &) const = default;
  };

  enum class cola_apply_result { applied, replay };

  // P is a pure, deterministic function string_view -> uint64_t. Partitions
  // need not be key ranges. Builders may read the same immutable base in
  // parallel; a receiver serializes apply() calls on its own round accumulator.
  template <class V, class A, class H, class P> struct partition_round {
    using cola = reference_cola<V, A, H>;
    using batch = cola_batch<V, A>;

    partition_round(cola base, std::string round_id, P partition)
      : base_(std::move(base)), current_(base_), round_id_(std::move(round_id)),
        partition_(std::move(partition)) {
      if (round_id_.empty()) throw std::invalid_argument("empty cola round identity");
    }

    batch make_batch(std::string batch_id, std::uint64_t owner,
      std::vector<cola_record<V>> writes) const {
      if (batch_id.empty()) throw std::invalid_argument("empty cola batch identity");
      std::sort(writes.begin(), writes.end(),
        [](auto const & a, auto const & b) { return a.key < b.key; });
      batch result{round_id_, std::move(batch_id), owner, base_.signature(), A::zero(), {}};
      for (std::size_t i = 0; i < writes.size(); ++i) {
        auto const & entry = writes[i];
        if (i && writes[i - 1].key == entry.key)
          throw std::invalid_argument("duplicate cola write key");
        if (partition_(std::string_view(entry.key)) != owner)
          throw std::invalid_argument("cola write outside assigned partition");
        auto old = base_.get(entry.key);
        if (!old && !entry.value) throw std::invalid_argument("deleting absent cola key");
        result.delta = A::add(result.delta,
          table_fingerprint<A>::delta(entry.key, old, entry.value, base_.hash_));
        result.edits.push_back({entry.key, std::move(old), entry.value});
      }
      return result;
    }

    cola_apply_result apply(batch const & update) {
      if (update.round_id != round_id_ || update.base_signature != base_.signature())
        throw std::invalid_argument("cola batch does not name this round base");
      if (update.batch_id.empty()) throw std::invalid_argument("empty cola batch identity");
      auto prior = accepted_.find(update.batch_id);
      if (prior != accepted_.end()) {
        if (prior->second != update) throw std::invalid_argument("cola batch identity reused");
        return cola_apply_result::replay;
      }
      auto delta = A::zero();
      for (std::size_t i = 0; i < update.edits.size(); ++i) {
        auto const & edit = update.edits[i];
        if (i && !(update.edits[i - 1].key < edit.key))
          throw std::invalid_argument("cola batch keys must be strictly sorted");
        if (partition_(std::string_view(edit.key)) != update.owner)
          throw std::invalid_argument("cola edit outside assigned partition");
        if (claimed_.contains(edit.key)) throw std::invalid_argument("overlapping cola updates");
        if (base_.get(edit.key) != edit.before)
          throw std::invalid_argument("cola edit old value mismatch");
        if (!edit.before && !edit.after)
          throw std::invalid_argument("deleting absent cola key");
        delta = A::add(delta,
          table_fingerprint<A>::delta(edit.key, edit.before, edit.after, base_.hash_));
      }
      if (delta != update.delta)
        throw std::invalid_argument("cola batch contribution mismatch");

      // Stage all allocations before publication. A rejected or failed batch
      // leaves the current snapshot and its replay/overlap bookkeeping intact.
      auto next = current_.append(update.edits);
      auto accepted = accepted_;
      auto claimed = claimed_;
      accepted.emplace(update.batch_id, update);
      for (auto const & edit : update.edits) claimed.emplace(edit.key, update.batch_id);
      accepted_.swap(accepted);
      claimed_.swap(claimed);
      current_ = std::move(next);
      return cola_apply_result::applied;
    }

    cola const & base() const noexcept { return base_; }
    cola snapshot() const { return current_; }
    std::size_t accepted_batches() const noexcept { return accepted_.size(); }

  private:
    cola base_;
    cola current_;
    std::string round_id_;
    P partition_;
    std::map<std::string, batch, std::less<>> accepted_;
    std::map<std::string, std::string, std::less<>> claimed_;
  };

  template <class V, class A, class H, class P>
  partition_round(reference_cola<V, A, H>, std::string, P) -> partition_round<V, A, H, P>;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Declares Diet's cola support.
 */
