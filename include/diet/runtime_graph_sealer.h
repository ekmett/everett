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

    std::shared_ptr<native_binding_type const> ensure_native(native_pointer const & native) {
      if (!native) throw std::invalid_argument("null runtime native owner");
      auto result = native->bindings_.get_or_create(catalog_.identity(), catalog_.root(), [&] {
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
        auto encoded = [&] {
          if constexpr (requires { typename Family::storage_type; })
            return Family::storage_type::encode_native(*native->owned());
          else return encode_native_sections(*native->owned());
        }();
        auto receipt = encoded.seal(catalog_.root(), id, attempt);
        catalog_.record_sealed(operation(), receipt);
        auto mapped = std::make_shared<mapped_native_type const>(mapped_native_type::open(receipt.path));
        return std::make_shared<native_binding_type const>(
          native_seal{catalog_.identity(), std::move(receipt)}, std::move(mapped));
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
        auto native = ensure_native(pair->native_owner());
        auto main = pair->main_target() ? ensure_pair(pair->main_target()) : nullptr;
        auto secondary = pair->secondary_target() ? ensure_native(pair->secondary_target()) : nullptr;
        blob_identity id{native->receipt.object, ids_()};
        std::array<blob_identity, 1> input{main ? main->identity : id};
        auto attempt = reserve(id.index, file_kind::fractional_index,
          std::span<blob_identity const>(input.data(), main ? 1 : 0));
        auto encoded = encode_cola_sections(*pair->built(), id.native,
          main ? std::optional<blob_identity>(main->identity) : std::nullopt,
          secondary ? std::optional<object_id>(secondary->receipt.object) : std::nullopt);
        auto receipt = encoded.seal(catalog_.root(), id.index, attempt);
        catalog_.record_sealed(operation(), receipt);
        catalog_.template register_pair<mapped_type>(operation(), id);
        auto index = std::make_shared<typename mapped_type::index_type const>(mapped_type::index_type::open(receipt.path));
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
    catalog_type & catalog_;
    Ids & ids_;
    std::string * last_operation_;
    std::string operation() {
      auto id = ids_().hex();
      if (last_operation_) *last_operation_ = id;
      return id;
    }
    object_attempt_id reserve(object_id const & id, file_kind kind, std::span<blob_identity const> inputs = {}) {
      object_attempt_id attempt(ids_().hex());
      auto owner = ids_().hex();
      std::array<catalog_object_reservation, 1> outputs{{{id, kind}}};
      catalog_.reserve(operation(), attempt, owner, inputs, outputs);
      return attempt;
    }
  };
}
