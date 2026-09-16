/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Seals new runtime owners once and stops at acknowledged shared dependencies.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/runtime_checkpoint.h>
#include <diet/runtime_seal.h>
#include <diet/sqlite_catalog.h>

namespace diet::runtime_store_detail {
  // The backend owns Catalog/Ids and serializes its own calls. Distinct
  // backends can resolve the same immutable owner concurrently; its binding
  // slot serializes production of the one acknowledged identity in this store.
  template <class P, class Ids, class Ops, class Family> struct graph_sealer {
    using catalog_type = sqlite_catalog<P, Ops>;
    using node_type = typename Family::node_type;
    using native_type = typename Family::native_type;
    using pair_type = typename node_type::pair_type;
    using native_pointer = typename node_type::native_pointer;
    using mapped_pointer = decltype(std::declval<node_type const &>().mapped());
    using mapped_type = std::remove_const_t<typename mapped_pointer::element_type>;
    using mapped_native_type = typename mapped_type::native_type;
    using native_binding_type = native_binding<mapped_native_type>;
    using pair_binding_type = pair_binding<mapped_type>;

    graph_sealer(catalog_type & catalog, Ids & ids, std::string * last_operation = nullptr)
      : catalog_(catalog), ids_(ids), last_operation_(last_operation) {}

    // Reserve one bounded set of independent, ready publication units. Each
    // unit retains its ordinary seal acknowledgment. Dependency discovery and
    // slot preparation finish before any nonblocking producer claim is held.
    // The remaining graph uses the ordinary serial walk below.
    std::size_t prepare_ready(std::span<pair_type const> roots,
        std::span<native_pointer const> natives) {
      constexpr std::size_t limit = 16;
      std::vector<pair_type> unbound;
      std::unordered_set<node_type const *> seen;
      std::unordered_set<native_type const *> paired;
      auto visit = [&](auto && self, pair_type const & pair) -> void {
        if (!pair || !seen.insert(pair.get()).second ||
            pair->bindings_.find(catalog_.identity(), catalog_.root())) return;
        if (!pair->built()) return; // Ordinary admission diagnoses this case.
        paired.insert(pair->native_owner().get());
        self(self, pair->main_target());
        unbound.push_back(pair);
      };
      for (auto const & pair : roots) visit(visit, pair);

      std::vector<ready_unit> units;
      units.reserve(limit);
      std::unordered_set<native_type const *> selected;
      for (auto const & pair : unbound) {
        if (units.size() == limit) break;
        auto main = pair->main_target() ? pair->main_target()->bindings_.find(catalog_.identity(), catalog_.root()) : nullptr;
        auto secondary = pair->secondary_target() ? pair->secondary_target()->bindings_.find(catalog_.identity(), catalog_.root()) : nullptr;
        if ((pair->main_target() && !main) || (pair->secondary_target() && !secondary)) continue;
        auto const & owner = pair->native_owner();
        if (!owner) continue;
        auto native = owner->bindings_.find(catalog_.identity(), catalog_.root());
        if (!native && (!batch_owned(owner) || !selected.insert(owner.get()).second)) continue;
        if (main) catalog_.verify_sealed(main->receipt, file_kind::fractional_index);
        if (secondary) catalog_.verify_sealed(secondary->receipt, file_kind::native_blob);
        if (native) catalog_.verify_sealed(native->receipt, file_kind::native_blob);
        units.emplace_back(pair, owner, std::move(main), std::move(secondary), std::move(native));
        auto & unit = units.back();
        unit.pair_claim.emplace(pair->bindings_.prepare(catalog_.identity(), catalog_.root()));
        if (!unit.native) unit.native_claim.emplace(owner->bindings_.prepare(catalog_.identity(), catalog_.root()));
      }
      for (auto const & native : natives) {
        if (units.size() == limit) break;
        // A native which belongs to an unbound pair must stay available for
        // native/index fusion, even when that pair's dependencies are unready.
        if (!native || paired.contains(native.get()) || !batch_owned(native) ||
            !selected.insert(native.get()).second ||
            native->bindings_.find(catalog_.identity(), catalog_.root())) continue;
        units.emplace_back(pair_type{}, native);
        units.back().native_claim.emplace(native->bindings_.prepare(catalog_.identity(), catalog_.root()));
      }
      if (units.size() < 2) return 0;

      std::vector<ready_unit *> claimed;
      claimed.reserve(units.size());
      for (auto & unit : units) {
        if (unit.pair_claim) {
          if (!unit.pair_claim->try_lock()) continue;
          if (unit.pair_claim->value()) { unit.pair_claim->release(); continue; }
        }
        if (unit.native_claim) {
          if (!unit.native_claim->try_lock()) {
            if (unit.pair_claim) unit.pair_claim->release();
            continue;
          }
          if (unit.native_claim->value()) {
            unit.native_claim->release();
            if (unit.pair_claim) unit.pair_claim->release();
            continue; // Re-resolve the changed plan in ordinary fallback.
          }
        }
        claimed.push_back(&unit);
      }
      if (claimed.size() < 2) return 0; // Claims leave scope before fallback.

      std::vector<catalog_object_reservation> outputs;
      std::vector<blob_identity> inputs;
      outputs.reserve(claimed.size() * 2);
      for (auto * unit : claimed) {
        if (unit->native) {
          unit->native_id = unit->native->receipt.object;
        } else {
          unit->native_id = ids_();
          outputs.push_back({*unit->native_id, file_kind::native_blob});
        }
        if (unit->pair) {
          unit->index_id = ids_();
          outputs.push_back({*unit->index_id, file_kind::fractional_index});
        }
        if (unit->main) inputs.push_back(unit->main->identity);
      }
      std::sort(inputs.begin(), inputs.end(), [](auto const & a, auto const & b) {
        return a.native.hex() < b.native.hex() || (a.native == b.native && a.index.hex() < b.index.hex());
      });
      inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
      auto attempt = reserve(outputs, inputs);
      for (auto * unit : claimed) finish_ready(*unit, attempt);
      return claimed.size();
    }

    std::shared_ptr<native_binding_type const> ensure_native(native_pointer const & native) {
      if (!native) throw std::invalid_argument("null runtime native owner");
      auto result = native->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
        return produce_native(native);
      });
      // A binding attests to acknowledged construction. Import into this
      // backend still checks the current envelope, without scanning its body.
      catalog_.verify_sealed(result->receipt, file_kind::native_blob);
      return result;
    }

    std::shared_ptr<pair_binding_type const> ensure_pair(pair_type const & pair) {
      if (!pair) throw std::invalid_argument("null runtime pair owner");
      auto result = pair->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
        if (!pair->built()) throw std::invalid_argument("mapped pair has no binding in this catalog");
        // Dependencies can share this pair's native owner. Resolve them before
        // taking its producer lock, so aliases select the acknowledged fallback.
        auto main = pair->main_target() ? ensure_pair(pair->main_target()) : nullptr;
        auto secondary = pair->secondary_target() ? ensure_native(pair->secondary_target()) : nullptr;
        auto const & owner = pair->native_owner();
        if (!owner) throw std::invalid_argument("null runtime native owner");
        std::optional<pair_seal> fused;
        std::shared_ptr<typename mapped_type::index_type const> fused_index;
        auto native = owner->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
          if constexpr (requires { owner->sealed(); })
            if (owner->sealed()) return produce_native(owner);
          if (!owner->owned()) return produce_native(owner);
          blob_identity id{ids_(), ids_()};
          std::array outputs{catalog_object_reservation{id.native, file_kind::native_blob},
            catalog_object_reservation{id.index, file_kind::fractional_index}};
          std::array<blob_identity, 1> input{main ? main->identity : id};
          auto attempt = reserve(outputs, std::span<blob_identity const>(input.data(), main ? 1 : 0));
          auto native_receipt = encode_native(owner).seal(catalog_.root(), id.native, attempt);
          auto encoded = encode_cola_sections(*pair->built(), id.native,
            main ? std::optional<blob_identity>(main->identity) : std::nullopt,
            secondary ? std::optional<object_id>(secondary->receipt.object) : std::nullopt);
          auto index_receipt = encoded.seal(catalog_.root(), id.index, attempt);
          auto [mapped_native, index] = catalog_.template seal_native_pair<mapped_type>(
            operation(), id, native_receipt, index_receipt);
          auto binding = std::make_shared<native_binding_type const>(
            native_seal{catalog_.identity(), std::move(native_receipt)}, std::move(mapped_native));
          fused.emplace(pair_seal{catalog_.identity(), id, std::move(index_receipt)});
          fused_index = std::move(index);
          return binding;
        });
        catalog_.verify_sealed(native->receipt, file_kind::native_blob);
        if (fused) {
          auto mapped = mapped_type::bind(fused->identity, native->mapped, std::move(fused_index),
            main ? main->mapped : nullptr, secondary ? secondary->mapped : nullptr,
            secondary ? std::optional<object_id>(secondary->receipt.object) : std::nullopt);
          return std::make_shared<pair_binding_type const>(std::move(*fused), std::move(mapped));
        }
        blob_identity id{native->receipt.object, ids_()};
        std::array<blob_identity, 1> input{main ? main->identity : id};
        auto attempt = reserve(id.index, file_kind::fractional_index,
          std::span<blob_identity const>(input.data(), main ? 1 : 0));
        auto encoded = encode_cola_sections(*pair->built(), id.native,
          main ? std::optional<blob_identity>(main->identity) : std::nullopt,
          secondary ? std::optional<object_id>(secondary->receipt.object) : std::nullopt);
        auto receipt = encoded.seal(catalog_.root(), id.index, attempt);
        auto index = catalog_.template seal_pair<mapped_type>(operation(), id, receipt);
        auto mapped = mapped_type::bind(id, native->mapped, std::move(index),
          main ? main->mapped : nullptr, secondary ? secondary->mapped : nullptr,
          secondary ? std::optional<object_id>(secondary->receipt.object) : std::nullopt);
        return std::make_shared<pair_binding_type const>(
          pair_seal{catalog_.identity(), std::move(id), std::move(receipt)}, std::move(mapped));
      });
      catalog_.verify_sealed(result->receipt, file_kind::fractional_index);
      return result;
    }

    // Mapping admission has already established the exact graph. Bind its
    // facade owners to the completed catalog rows without retaining a second
    // registry of weak owners. Normal reads inspect metadata only.
    void bind_native(native_pointer const & native, object_id const & id) {
      auto result = native->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
        return std::make_shared<native_binding_type const>(native_seal{
          catalog_.identity(), catalog_.sealed_receipt(id, file_kind::native_blob)}, native->mapped());
      });
      if (result->receipt.object != id) throw std::invalid_argument("native owner has another catalog identity");
    }
    void bind_pair(pair_type const & pair, blob_identity const & id) {
      auto result = pair->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
        return std::make_shared<pair_binding_type const>(pair_seal{
          catalog_.identity(), id, catalog_.sealed_receipt(id.index, file_kind::fractional_index)}, pair->mapped());
      });
      if (result->identity != id) throw std::invalid_argument("pair owner has another catalog identity");
    }
    object_id native_id(native_pointer const & native) const {
      auto value = native->bindings_.find(catalog_.identity(), catalog_.root());
      if (!value) throw std::logic_error("runtime native has not been sealed");
      return value->receipt.object;
    }
    blob_identity pair_id(pair_type const & pair) const {
      auto value = pair->bindings_.find(catalog_.identity(), catalog_.root());
      if (!value) throw std::logic_error("runtime pair has not been sealed");
      return value->identity;
    }

    native_pointer mapped_native(native_pointer const & native) {
      auto binding = native->bindings_.find(catalog_.identity(), catalog_.root());
      if (!binding) throw std::logic_error("runtime native has not been sealed");
      if (native->mapped() == binding->mapped) return native;
      // Only a distinct facade is cached. A mapped owner returns itself above,
      // so the memo never acquires a shared-pointer reference to its own owner.
      return native->mapped_owners_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
        auto result = native_type::from_mapped(binding->mapped);
        result->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] { return binding; });
        return result;
      });
    }
    pair_type mapped_pair(pair_type const & pair) {
      auto binding = pair->bindings_.find(catalog_.identity(), catalog_.root());
      if (!binding) throw std::logic_error("runtime pair has not been sealed");
      if (pair->canonical_mapped() && pair->mapped() == binding->mapped) return pair;
      return pair->mapped_owners_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
        auto result = node_type::from_mapped_parts(binding->mapped, mapped_native(pair->native_owner()),
          pair->main_target() ? mapped_pair(pair->main_target()) : nullptr,
          pair->secondary_target() ? mapped_native(pair->secondary_target()) : nullptr);
        result->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] { return binding; });
        return result;
      });
    }

  private:
    using native_producer = typename catalog_bindings<native_binding_type>::producer;
    using pair_producer = typename catalog_bindings<pair_binding_type>::producer;
    struct ready_unit {
      pair_type pair;
      native_pointer owner;
      std::shared_ptr<pair_binding_type const> main;
      std::shared_ptr<native_binding_type const> secondary, native;
      std::optional<pair_producer> pair_claim;
      std::optional<native_producer> native_claim;
      std::optional<object_id> native_id, index_id;
      ready_unit(pair_type p, native_pointer n, std::shared_ptr<pair_binding_type const> m = {},
          std::shared_ptr<native_binding_type const> s = {}, std::shared_ptr<native_binding_type const> value = {})
        : pair(std::move(p)), owner(std::move(n)), main(std::move(m)), secondary(std::move(s)), native(std::move(value)) {}
    };
    static bool batch_owned(native_pointer const & native) {
      if constexpr (requires { native->sealed(); }) if (native->sealed()) return false;
      return bool(native->owned());
    }
    void finish_ready(ready_unit & unit, object_attempt_id const & attempt) {
      std::optional<object_seal_receipt> native_receipt;
      if (!unit.native) native_receipt.emplace(encode_native(unit.owner).seal(catalog_.root(), *unit.native_id, attempt));
      if (!unit.pair) {
        catalog_.record_sealed(operation(), *native_receipt);
        auto mapped = std::make_shared<mapped_native_type const>(mapped_native_type::open(native_receipt->path));
        auto binding = std::make_shared<native_binding_type const>(
          native_seal{catalog_.identity(), std::move(*native_receipt)}, std::move(mapped));
        unit.native_claim->install(std::move(binding));
        return;
      }
      blob_identity id{*unit.native_id, *unit.index_id};
      auto encoded = encode_cola_sections(*unit.pair->built(), id.native,
        unit.main ? std::optional<blob_identity>(unit.main->identity) : std::nullopt,
        unit.secondary ? std::optional<object_id>(unit.secondary->receipt.object) : std::nullopt);
      auto receipt = encoded.seal(catalog_.root(), id.index, attempt);
      std::shared_ptr<typename mapped_type::index_type const> index;
      if (native_receipt) {
        auto [mapped_native, prepared_index] = catalog_.template seal_native_pair<mapped_type>(
          operation(), id, *native_receipt, receipt);
        unit.native = std::make_shared<native_binding_type const>(
          native_seal{catalog_.identity(), std::move(*native_receipt)}, std::move(mapped_native));
        unit.native_claim->install(unit.native);
        index = std::move(prepared_index);
      } else index = catalog_.template seal_pair<mapped_type>(operation(), id, receipt);
      auto mapped = mapped_type::bind(id, unit.native->mapped, std::move(index),
        unit.main ? unit.main->mapped : nullptr, unit.secondary ? unit.secondary->mapped : nullptr,
        unit.secondary ? std::optional<object_id>(unit.secondary->receipt.object) : std::nullopt);
      auto binding = std::make_shared<pair_binding_type const>(
        pair_seal{catalog_.identity(), id, std::move(receipt)}, std::move(mapped));
      unit.pair_claim->install(std::move(binding));
    }
    catalog_type & catalog_;
    Ids & ids_;
    std::string * last_operation_;
    std::string operation() {
      auto id = ids_().hex();
      if (last_operation_) *last_operation_ = id;
      return id;
    }
    auto encode_native(native_pointer const & native) {
      if constexpr (requires { typename Family::storage_type; })
        return Family::storage_type::encode_native(*native->owned());
      else return encode_native_sections(*native->owned());
    }
    std::shared_ptr<native_binding_type const> produce_native(native_pointer const & native) {
      if constexpr (requires { native->sealed(); }) {
        if (auto sealed = native->sealed()) {
          if (sealed->catalog != catalog_.identity())
            throw std::invalid_argument("native seal belongs to another catalog");
          catalog_.verify_sealed(sealed->receipt, file_kind::native_blob);
          return std::make_shared<native_binding_type const>(*sealed, native->mapped());
        }
      }
      if (!native->owned()) throw std::invalid_argument("mapped native has no binding in this catalog");
      auto id = ids_();
      auto attempt = reserve(id, file_kind::native_blob);
      auto receipt = encode_native(native).seal(catalog_.root(), id, attempt);
      catalog_.record_sealed(operation(), receipt);
      auto mapped = std::make_shared<mapped_native_type const>(mapped_native_type::open(receipt.path));
      return std::make_shared<native_binding_type const>(
        native_seal{catalog_.identity(), std::move(receipt)}, std::move(mapped));
    }
    object_attempt_id reserve(object_id const & id, file_kind kind, std::span<blob_identity const> inputs = {}) {
      std::array<catalog_object_reservation, 1> outputs{{{id, kind}}};
      return reserve(outputs, inputs);
    }
    object_attempt_id reserve(std::span<catalog_object_reservation const> outputs,
        std::span<blob_identity const> inputs = {}) {
      object_attempt_id attempt(ids_().hex());
      auto owner = ids_().hex();
      catalog_.reserve(operation(), attempt, owner, inputs, outputs);
      return attempt;
    }
  };
}
