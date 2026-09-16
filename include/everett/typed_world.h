/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Connects sort-owned semantics to encoded COLA updates and immutable typed snapshots.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/cola_runtime.h>
#include <everett/fingerprint.h>
#include <everett/sort_codec.h>
#include <everett/session.h>

#include <algorithm>
#include <bit>
#include <concepts>
#include <memory>
#include <string_view>

namespace everett {
  // A custom sort supplies state_type, initial(key), apply(key,state,arrow),
  // compose(key,older,newer), present(key,state), hash_key(key), and
  // hash_value(key,state). Its value codec encodes arrows, not necessarily
  // states. replacement=true permits first-hit reads and put/erase helpers.
  template <class S> struct sort_semantics : S {};

  template <> struct sort_semantics<unsorted<std::optional<std::string>>> {
    using state_type = std::optional<std::string>;
    static constexpr bool replacement = true;
    static state_type initial(std::string const &) { return std::nullopt; }
    static state_type apply(std::string const &, state_type const &, state_type value) { return value; }
    static state_type compose(std::string const &, state_type const &, state_type newer) { return newer; }
    static state_type erase(std::string const &) { return std::nullopt; }
    static bool present(std::string const &, state_type const & value) noexcept { return value.has_value(); }
    static std::uint64_t hash_key(std::string const & key) noexcept { return u64_table_hash{}.key(key); }
    static std::uint64_t hash_value(std::string const &, state_type const & value) noexcept {
      return value ? u64_table_hash::mix(u64_table_hash{}.key(*value) ^ 0xd6e8feb86659fd93ULL) : 0;
    }
  };

  namespace typed_detail {
    template <class Leaves> struct default_sort { using type = void; };
    template <class S> struct default_sort<registry_detail::sorts<S>> { using type = S; };
    template <class P> using default_sort_t = typename default_sort<
      typename registry_detail::info<typename P::registry_type>::leaves>::type;
    template <class S> using key_t = typename sort_codec<S>::key_codec::value_type;
    template <class S> using arrow_t = typename sort_codec<S>::value_codec::value_type;
    template <class S> using state_t = typename sort_semantics<S>::state_type;
    template <class S> constexpr bool replacement = [] {
      if constexpr (requires { sort_semantics<S>::replacement; }) return bool(sort_semantics<S>::replacement);
      else return false;
    }();

    template <class Leaves> struct all_replacements;
    template <class... S> struct all_replacements<registry_detail::sorts<S...>>
      : std::bool_constant<(replacement<S> && ...)> {};

    template <class P, class S> bit_string key(key_t<S> const & value) {
      bit_string result;
      sort_bit_writer out(result);
      write_sort_code<typename P::registry_type, S>(out);
      sort_codec<S>::key_codec::write_ordered(out, value);
      if (result.bit_size & (P::bits_per_unit - 1))
        throw std::invalid_argument("sort key is not aligned for its profile policy");
      return result;
    }
    template <class P, class S> bit_string value(arrow_t<S> const & value) {
      bit_string result;
      sort_bit_writer out(result);
      sort_codec<S>::value_codec::write(out, value);
      if constexpr (P::unit == profile_unit::byte)
        if (auto tail = result.bit_size & 7) out.write_bits(0, unsigned(8 - tail));
      if (P::value_width && (result.bit_size >> P::unit_shift) != *P::value_width)
        throw std::invalid_argument("sort arrow differs from the policy's fixed value width");
      return result;
    }
    template <class P, class S> arrow_t<S> value(bit_view encoded) {
      sort_bit_reader in(encoded);
      auto result = sort_codec<S>::value_codec::read(in);
      if constexpr (P::unit == profile_unit::byte) {
        if (in.remaining() > 7 || (in.remaining() && in.read_bits(unsigned(in.remaining()))))
          throw std::invalid_argument("noncanonical typed value padding");
      } else if (!in.empty()) throw std::invalid_argument("trailing typed arrow bits");
      return result;
    }
    template <class P, class F> decltype(auto) dispatch_key(bit_view encoded, F && action) {
      sort_bit_reader in(encoded);
      return dispatch_sort<typename P::registry_type>(in, [&]<class S>(std::type_identity<S> tag, auto & source) {
        auto decoded = sort_codec<S>::key_codec::read_ordered(source);
        if (!source.empty()) throw std::invalid_argument("trailing typed key bits");
        return std::invoke(std::forward<F>(action), tag, decoded);
      });
    }
    template <class P> struct profile_key_transport {
      template <class S> static bit_string encode(key_t<S> const & value) { return key<P, S>(value); }
      template <class S> static bit_string prefix() { return sort_code<typename P::registry_type, S>(); }
      template <class S> static key_t<S> decode(bit_view bits) {
        sort_bit_reader input(bits);
        auto result = sort_codec<S>::key_codec::read_ordered(input);
        if (!input.empty()) throw std::invalid_argument("trailing typed key bits");
        return result;
      }
      template <class F> static decltype(auto) dispatch(bit_view bits, F && fn) {
        return dispatch_key<P>(bits, std::forward<F>(fn));
      }
    };
    template <class P, class Family, class = void> struct transport { using type = profile_key_transport<P>; };
    template <class P, class Family> struct transport<P, Family, std::void_t<typename Family::key_transport>> {
      using type = typename Family::key_transport;
    };
    template <class P, class Family> using transport_t = typename transport<P, Family>::type;
    template <class P, class Transport = profile_key_transport<P>> struct compose {
      bit_string operator()(bit_view key, bit_view older, bit_view newer) const {
        return Transport::dispatch(key, [&]<class S>(std::type_identity<S>, auto const & decoded) {
          auto before = value<P, S>(older), after = value<P, S>(newer);
          return value<P, S>(sort_semantics<S>::compose(decoded, std::move(before), std::move(after)));
        });
      }
    };
  }

  template <class A = wrapping_fingerprint_algebra> struct typed_world_metadata {
    typename A::element signature = A::zero();
    std::uint64_t live_count = 0;
    std::string schema_id;

    // Compact trusted checkpoint payload: signature, live count, then the
    // nonempty schema ID. Runtime frontier intervals are stored separately.
    std::vector<std::byte> encode() const requires std::same_as<typename A::element, std::uint64_t> {
      if (schema_id.empty()) throw std::invalid_argument("empty typed schema identity");
      std::vector<std::byte> result(16 + schema_id.size());
      for (unsigned i = 0; i != 8; ++i) {
        result[i] = std::byte(signature >> (i << 3));
        result[8 + i] = std::byte(live_count >> (i << 3));
      }
      std::copy(std::as_bytes(std::span(schema_id)).begin(), std::as_bytes(std::span(schema_id)).end(), result.begin() + 16);
      return result;
    }
    static typed_world_metadata decode(std::span<std::byte const> data)
      requires std::same_as<typename A::element, std::uint64_t> {
      if (data.size() <= 16) throw std::invalid_argument("truncated typed metadata");
      typed_world_metadata result;
      for (unsigned i = 0; i != 8; ++i) {
        result.signature |= std::uint64_t(std::to_integer<unsigned char>(data[i])) << (i << 3);
        result.live_count |= std::uint64_t(std::to_integer<unsigned char>(data[8 + i])) << (i << 3);
      }
      result.schema_id.assign(reinterpret_cast<char const *>(data.data() + 16), data.size() - 16);
      return result;
    }
    bool operator==(typed_world_metadata const &) const = default;
  };

  template <class P, class A, class Family = binary_runtime_family<P>> struct typed_contribution;
  template <class P, class A, class Family = binary_runtime_family<P>> struct typed_batch;
  template <class P, class A, std::uint64_t DepthLimit, class Family> struct typed_engine;
  template <class P, class A, std::uint64_t DepthLimit, class Family> struct replacement_rebuild_engine;

  template <class P = string_policy, class A = wrapping_fingerprint_algebra,
    class Family = binary_runtime_family<P>> struct typed_world {
    using policy_type = P;
    using metadata_type = typed_world_metadata<A>;
    using runtime_family = Family;
    using key_transport = typed_detail::transport_t<P, Family>;
    using runtime_snapshot = typename Family::snapshot_type;
    using contribution_type = typed_contribution<P, A, Family>;
    using batch_type = typed_batch<P, A, Family>;
    runtime_snapshot const & runtime() const & noexcept { return state_->runtime; }
    runtime_snapshot const & runtime() const && = delete;
    metadata_type const & metadata() const & noexcept { return state_->metadata; }
    auto signature() const { return state_->metadata.signature; }
    std::uint64_t live_count() const noexcept { return state_->metadata.live_count; }

    // Metadata and the exact graph must have been admitted together. This
    // checks schema identity and counts; it deliberately does not scan hashes.
    static typed_world restore(runtime_snapshot data, metadata_type metadata, std::string_view expected_schema) {
      if (expected_schema.empty() || metadata.schema_id != expected_schema || metadata.live_count > data.admissions())
        throw std::invalid_argument("typed snapshot metadata or schema mismatch");
      return {std::move(data), std::move(metadata)};
    }
    template <class S = typed_detail::default_sort_t<P>> typed_detail::state_t<S>
    get(typed_detail::key_t<S> const & key) const {
      return get_encoded<S>(key, key_transport::template encode<S>(key));
    }
    batch_type batch() const;
    template <class S = typed_detail::default_sort_t<P>> contribution_type
    change(typed_detail::key_t<S> const & key, typed_detail::arrow_t<S> const & arrow) const;
    template <class S = typed_detail::default_sort_t<P>> contribution_type
    put(typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value) const
      requires typed_detail::replacement<S>;
    template <class S = typed_detail::default_sort_t<P>> contribution_type
    erase(typed_detail::key_t<S> const & key) const requires typed_detail::replacement<S>;
  private:
    template <class, class, std::uint64_t, class> friend struct typed_engine;
    template <class, class, std::uint64_t, class> friend struct replacement_rebuild_engine;
    // A contribution already owns its encoded key. The logical key remains
    // available for sort semantics, while this synchronous query shares the
    // same implementation as get without encoding it again. Only typed_batch
    // creates contribution records, through this same Family's transport;
    // preflight still dispatches and validates the encoded key first. Custom views and
    // all escaping cursor contexts still take an owning copy.
    template <class S, class Query> typed_detail::state_t<S>
    get_encoded(typed_detail::key_t<S> const & key, Query && encoded) const {
      static_assert(std::same_as<std::remove_cvref_t<Query>, bit_string>);
      using semantics = sort_semantics<S>;
      if constexpr (typed_detail::replacement<S>) {
        auto decode = [&](bit_view value) -> typed_detail::state_t<S> {
          return semantics::apply(key, semantics::initial(key), typed_detail::value<P, S>(value));
        };
        if constexpr (requires { cola_detail::first_value(state_->runtime.query_root(), std::forward<Query>(encoded), decode); }) {
          auto value = cola_detail::first_value(state_->runtime.query_root(), std::forward<Query>(encoded), decode);
          return value ? std::move(*value) : semantics::initial(key);
        }
      }
      auto cursor = [&] {
        if constexpr (requires { state_->runtime.cursor_owned(bit_string(std::forward<Query>(encoded))); })
          return state_->runtime.cursor_owned(bit_string(std::forward<Query>(encoded)));
        else return state_->runtime.cursor(encoded.view());
      }();
      if constexpr (typed_detail::replacement<S>) {
        while (!cursor.done()) {
          cursor.step(1);
          if (cursor.has_match()) {
            auto match = cursor.take_match();
            return semantics::apply(key, semantics::initial(key), typed_detail::value<P, S>(match.value.view()));
          }
        }
        return semantics::initial(key);
      } else {
        std::vector<typed_detail::arrow_t<S>> arrows;
        while (!cursor.done()) {
          cursor.step(1);
          while (cursor.has_match()) {
            auto match = cursor.take_match();
            arrows.push_back(typed_detail::value<P, S>(match.value.view()));
          }
        }
        auto state = semantics::initial(key);
        for (auto i = arrows.rbegin(); i != arrows.rend(); ++i) state = semantics::apply(key, std::move(state), *i);
        return state;
      }
    }
    struct state {
      runtime_snapshot runtime;
      metadata_type metadata;
    };
    // A snapshot retains one immutable head. Its schema and transitive file
    // dependencies remain shared rather than copied into each reader.
    std::shared_ptr<state const> state_;
    typed_world(runtime_snapshot data, metadata_type metadata)
      : state_(std::make_shared<state const>(state{std::move(data), std::move(metadata)})) {}
  };

  template <class P, class A, class Family> struct typed_contribution {
    std::optional<typed_world<P, A, Family>> const & base() const noexcept { return base_; }
    std::span<profile_record const> records() const noexcept { return records_; }
  private:
    friend struct typed_batch<P, A, Family>;
    std::optional<typed_world<P, A, Family>> base_;
    std::vector<profile_record> records_;
    typed_contribution(std::optional<typed_world<P, A, Family>> base, std::vector<profile_record> records)
      : base_(std::move(base)), records_(std::move(records)) {}
  };

  template <class P, class A, class Family> struct typed_batch {
    typed_batch() = default;
    explicit typed_batch(typed_world<P, A, Family> base) : base_(std::move(base)) {}
    template <class S = typed_detail::default_sort_t<P>> typed_batch &
    change(typed_detail::key_t<S> const & key, typed_detail::arrow_t<S> const & arrow) {
      records_.push_back({typed_detail::transport_t<P, Family>::template encode<S>(key), typed_detail::value<P, S>(arrow)});
      return *this;
    }
    template <class S = typed_detail::default_sort_t<P>> typed_batch &
    put(typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value)
      requires typed_detail::replacement<S> { return change<S>(key, value); }
    template <class S = typed_detail::default_sort_t<P>> typed_batch &
    erase(typed_detail::key_t<S> const & key) requires typed_detail::replacement<S> {
      if (base_ && !sort_semantics<S>::present(key, base_->template get<S>(key)))
        throw std::invalid_argument("deleting absent typed key");
      return change<S>(key, sort_semantics<S>::erase(key));
    }
    typed_contribution<P, A, Family> finish() && {
      std::sort(records_.begin(), records_.end(), [](auto const & a, auto const & b) {
        return compare_bits(a.key.view(), b.key.view()) < 0;
      });
      for (std::size_t i = 1; i < records_.size(); ++i)
        if (compare_bits(records_[i - 1].key.view(), records_[i].key.view()) == 0)
          throw std::invalid_argument("duplicate key in typed batch");
      return {std::move(base_), std::move(records_)};
    }
  private:
    std::optional<typed_world<P, A, Family>> base_;
    std::vector<profile_record> records_;
  };

  template <class P, class A, class Family> auto typed_world<P, A, Family>::batch() const -> batch_type { return batch_type(*this); }
  template <class P, class A, class Family> template <class S>
  auto typed_world<P, A, Family>::change(typed_detail::key_t<S> const & key, typed_detail::arrow_t<S> const & arrow) const -> contribution_type {
    auto result = batch(); result.template change<S>(key, arrow); return std::move(result).finish();
  }
  template <class P, class A, class Family> template <class S>
  auto typed_world<P, A, Family>::put(typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value) const -> contribution_type
    requires typed_detail::replacement<S> { return change<S>(key, value); }
  template <class P, class A, class Family> template <class S>
  auto typed_world<P, A, Family>::erase(typed_detail::key_t<S> const & key) const -> contribution_type
    requires typed_detail::replacement<S> {
    auto result = batch(); result.template erase<S>(key); return std::move(result).finish();
  }

  // DepthLimit bounds the main-chain nodes of every imported or published
  // root, including routing ancestors. It is enforced support, not an inferred
  // COLA theorem, and bounds the allowance for ready singleton admissions.
  template <class P = string_policy, class A = wrapping_fingerprint_algebra, std::uint64_t DepthLimit = 256,
    class Family = binary_runtime_family<P>>
  struct typed_engine {
    using policy_type = P;
    using world_type = typed_world<P, A, Family>;
    using contribution_type = typed_contribution<P, A, Family>;
    using metadata_type = typed_world_metadata<A>;
    using runtime_family = Family;
    using key_transport = typed_detail::transport_t<P, Family>;
    using compose_type = std::conditional_t<typed_detail::all_replacements<
      typename registry_detail::info<typename P::registry_type>::leaves>::value,
      replace_native_value, typed_detail::compose<P, key_transport>>;
    using runtime_type = typename Family::template runtime_type<compose_type>;
    static constexpr bool charged_service = requires { runtime_type::service_budget(std::uint64_t{}); };
    static constexpr std::uint64_t ready_admission_allowance = [] {
      if constexpr (charged_service) return 2 * runtime_type::local_charge_bound + 16 * DepthLimit + 512;
      else return 2 * P::group_size + 128 + DepthLimit + 32;
    }();
    // Reservation cannot inspect a concurrently changing executor. Reserve a
    // conservative 64-level service ceiling; actual service uses current h.
    static constexpr std::uint64_t admission_allowance = [] {
      if constexpr (charged_service)
        return ready_admission_allowance + runtime_type::local_charge_bound * 8 * (runtime_type::maximum_levels + 2);
      else return ready_admission_allowance;
    }();
    static_assert(DepthLimit && DepthLimit < (std::uint64_t{1} << 32) && P::group_size < (std::uint64_t{1} << 32));

    explicit typed_engine(std::string schema_id = default_schema())
      : runtime_(), current_(checked_snapshot(runtime_.snapshot()), metadata_type{A::zero(), 0, checked_schema(std::move(schema_id))}) {}
    static typed_engine from_snapshot(world_type state) { return typed_engine(std::move(state)); }
    template <class Storage> static typed_engine from_snapshot(world_type state, Storage storage)
      requires requires { runtime_type::from_snapshot(state.runtime(), std::move(storage)); } {
      return typed_engine(std::move(state), std::move(storage));
    }
    // The caller supplies an equivalent admitted layout. Metadata is a cheap
    // consistency check, not a cryptographic proof of equivalent contents.
    void rebase(world_type state) {
      require_active();
      if (pending() || state.metadata() != current_.metadata() || state.runtime().admissions() != current_.runtime().admissions())
        throw std::invalid_argument("typed rebase requires a settled equivalent snapshot");
      require_depth(state.runtime());
      auto replacement = [&] {
        if constexpr (requires { runtime_.storage(); }) return runtime_type::from_snapshot(state.runtime(), runtime_.storage());
        else return runtime_type::from_snapshot(state.runtime());
      }();
      runtime_ = std::move(replacement); current_ = std::move(state);
    }
    world_type snapshot() const { return current_; }
    // A rebuild shares the concrete execution context, not a borrowed backend pointer.
    auto storage() const requires requires (runtime_type const & value) { value.storage(); } { return runtime_.storage(); }
    bool pending() const noexcept { return runtime_.pending(); }
    bool failed() const noexcept { return failed_ || runtime_.failed(); }
    void poison() noexcept {
      failed_ = true;
      if constexpr (requires { { runtime_.poison() } noexcept; }) runtime_.poison();
    }
    bool admission_ready() const noexcept { return runtime_.admission_ready(); }
    auto work() const { return runtime_.work(); }
    static typed_batch<P, A, Family> batch() { return {}; }
    template <class S = typed_detail::default_sort_t<P>> static contribution_type
    change(typed_detail::key_t<S> const & key, typed_detail::arrow_t<S> const & arrow) {
      auto result = batch(); result.template change<S>(key, arrow); return std::move(result).finish();
    }
    template <class S = typed_detail::default_sort_t<P>> static contribution_type
    put(typed_detail::key_t<S> const & key, typed_detail::state_t<S> const & value)
      requires typed_detail::replacement<S> { return change<S>(key, value); }
    template <class S = typed_detail::default_sort_t<P>> static contribution_type
    erase(typed_detail::key_t<S> const & key) requires typed_detail::replacement<S> {
      auto result = batch(); result.template erase<S>(key); return std::move(result).finish();
    }
    static std::uint64_t reservation_work(std::uint64_t records) {
      return profile_detail::multiply(records, admission_allowance);
    }
    static session_reservation reservation(contribution_type const & input) {
      session_reservation result{reservation_work(input.records().size()), 0};
      for (auto const & record : input.records())
        result.bytes = profile_detail::add(result.bytes, profile_detail::add(record.key.bytes.size(), record.value.bytes.size()));
      return result;
    }
    std::optional<world_type> advance(std::uint64_t budget) {
      require_active();
      try {
        auto updated = runtime_.advance(budget);
        if (updated.same_layout(current_.runtime())) return std::nullopt;
        require_depth(updated);
        current_ = world_type(std::move(updated), current_.metadata());
        return current_;
      } catch (...) { poison(); throw; }
    }
    world_type contribute(contribution_type input) {
      require_active();
      auto metadata = prepare(input);
      if (input.records().empty()) return current_;
      try {
        if (initialize(input, metadata)) return current_;
        complete(input.records(), std::move(metadata));
        return current_;
      } catch (...) { poison(); throw; }
    }
  private:
    template <class, class, std::uint64_t, class> friend struct replacement_rebuild_engine;
    // The rebuild wrapper shares this complete preflight before installing a
    // pristine batch; neither caller can publish metadata ahead of execution.
    metadata_type prepare(contribution_type const & input) const {
      if (input.base() && input.base()->metadata().schema_id != current_.metadata().schema_id)
        throw std::invalid_argument("typed contribution uses another schema");
      auto metadata = current_.metadata();
      // Validate every old state and compute every delta before changing the
      // executor. Disjoint batches from one immutable base therefore commute.
      for (auto const & record : input.records()) {
        key_transport::dispatch(record.key.view(), [&]<class S>(std::type_identity<S>, auto const & key) {
          using semantics = sort_semantics<S>;
          auto old = current_.template get_encoded<S>(key, record.key);
          if (input.base() && old != input.base()->template get_encoded<S>(key, record.key))
            throw std::invalid_argument("stale typed key value");
          auto arrow = typed_detail::value<P, S>(record.value.view());
          auto next = semantics::apply(key, old, std::move(arrow));
          bool was = semantics::present(key, old), now = semantics::present(key, next);
          if constexpr (typed_detail::replacement<S>)
            if (!was && !now) throw std::invalid_argument("deleting absent typed key");
          auto before_hash = was ? A::lift(semantics::hash_value(key, old)) : A::zero();
          auto after_hash = now ? A::lift(semantics::hash_value(key, next)) : A::zero();
          metadata.signature = A::add(metadata.signature,
            A::multiply(A::lift(semantics::hash_key(key)), A::subtract(after_hash, before_hash)));
          if (now && !was) metadata.live_count = profile_detail::add(metadata.live_count, 1);
          if (was && !now) {
            if (!metadata.live_count) throw std::invalid_argument("inconsistent typed live count");
            --metadata.live_count;
          }
        });
      }
      return metadata;
    }
    // The caller has validated the complete batch. An initialized prefix and
    // this ordinary paid tail remain private until the final metadata is ready.
    void complete(std::span<profile_record const> records, metadata_type metadata) {
      for (auto const & record : records) {
        while (!runtime_.admission_ready()) runtime_.advance(std::max<std::uint64_t>(runtime_.next_service_cost(), 1));
        auto ready = runtime_.snapshot();
        if (ready.query_root().head()->depth() > DepthLimit || runtime_.admission_cost() > ready_admission_allowance)
          throw std::length_error("typed runtime exceeds ready-admission depth allowance");
        auto service = [&] {
          if constexpr (charged_service) return runtime_type::service_budget(profile_detail::add(ready.admissions(), 1));
          else return std::uint64_t{0};
        }();
        if (!runtime_.try_contribute(record, service)) throw std::logic_error("typed ready admission unexpectedly blocked");
      }
      auto state = [&]() -> typename world_type::runtime_snapshot {
        if constexpr (requires { runtime_.checkpoint(); }) {
          if (!records.empty()) return runtime_.checkpoint();
        }
        return runtime_.snapshot();
      }();
      require_depth(state);
      current_ = world_type(std::move(state), std::move(metadata));
    }
    bool initialize(contribution_type const & input, metadata_type const & metadata) {
      try {
        if constexpr (requires { runtime_.try_initialize_sorted(input.records(), std::uint64_t{}, DepthLimit); }) {
          auto count = input.records().size();
          if (count >= 2 && !current_.runtime().admissions() && !runtime_.pending()) {
            // Prefix and tail share the original linear reservation: M*A
            // funds initialization, and each remaining record retains its A.
            auto prefix = std::bit_floor(count);
            if (runtime_.try_initialize_sorted(input.records().first(prefix), reservation_work(prefix), DepthLimit)) {
              require_depth(runtime_.snapshot());
              complete(input.records().subspan(prefix), metadata);
              return true;
            }
          }
        }
        return false;
      } catch (...) { poison(); throw; }
    }
    runtime_type runtime_;
    world_type current_;
    bool failed_ = false;
    static void require_depth(typename world_type::runtime_snapshot const & state) {
      if (state.query_root().head()->depth() > DepthLimit)
        throw std::length_error("typed runtime exceeds publication depth allowance");
    }
    static auto checked_snapshot(typename world_type::runtime_snapshot state) {
      require_depth(state);
      return state;
    }
    explicit typed_engine(world_type state)
      : runtime_([&] {
          require_depth(state.runtime());
          return runtime_type::from_snapshot(state.runtime());
        }()), current_(std::move(state)) {}
    template <class Storage> typed_engine(world_type state, Storage storage)
      : runtime_([&] {
          require_depth(state.runtime());
          if constexpr (requires { storage.check_schema(state.metadata().schema_id); })
            storage.check_schema(state.metadata().schema_id);
          return runtime_type::from_snapshot(state.runtime(), std::move(storage));
        }()), current_(std::move(state)) {}
    static std::string default_schema() {
      if constexpr (requires { Family::default_schema(); }) return Family::default_schema();
      else if constexpr (std::same_as<typename P::registry_type, string_registry>)
        return "everett.optional-string/code0/v1";
      else if constexpr (std::same_as<typename P::registry_type, unsorted<std::optional<std::string>>>)
        return "everett.optional-string/tagless/v1";
      else return {};
    }
    static std::string checked_schema(std::string value) {
      if (value.empty()) throw std::invalid_argument("typed engine requires a schema identity");
      return value;
    }
    void require_active() const {
      if (failed_) throw std::logic_error("failed typed engine");
    }
  };
}
