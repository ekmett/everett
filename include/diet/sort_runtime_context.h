/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Owns catalog reservations and streamed native merge outputs for one runtime worker.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/runtime_store.h>
#include <diet/sort_runtime.h>
#include <diet/sort_profile_file_merge.h>

namespace diet {
  // One serialized worker owns this concrete context. Storage handles share
  // its lifetime when an equivalent mapped publication rebases the core; they
  // do not add concurrent access or locks. Catalog and file operations stay
  // outside the merge's per-record loop.
  template <class P, class Selector = registry_selector<typename P::registry_type>, class Ids = random_object_ids,
            class CatalogOps = sqlite_catalog_ops, class FileOps = posix_object_ops>
  struct sort_runtime_context {
    using native_type = sort_runtime_native<P, Selector>;
    using native_pointer = std::shared_ptr<native_type const>;
    using catalog_type = sqlite_catalog<P, CatalogOps>;
    template <class Compose> using merge_type = sort_profile_file_merge<P, native_type, Compose, Selector, FileOps>;
    static std::shared_ptr<sort_runtime_context> open(std::filesystem::path const & root, Ids ids = {},
        catalog_options options = {}, CatalogOps catalog_ops = {}, FileOps file_ops = {}) {
      auto catalog = catalog_type::open(root, options, std::move(catalog_ops));
      if (catalog.schema_version() != 4) throw std::invalid_argument("streamed runtime requires a named catalog");
      return std::shared_ptr<sort_runtime_context>(new sort_runtime_context(std::move(catalog), std::move(ids), std::move(file_ops)));
    }
    sort_runtime_context(sort_runtime_context const &) = delete;
    sort_runtime_context & operator=(sort_runtime_context const &) = delete;
    sort_runtime_context(sort_runtime_context &&) = delete;
    sort_runtime_context & operator=(sort_runtime_context &&) = delete;
    bool failed() const noexcept { return failed_ || catalog_.poisoned(); }
    void poison() noexcept { failed_ = true; }
    std::uint64_t sealed_outputs() const noexcept { return sealed_outputs_; }
    object_id const & catalog_identity() const & noexcept { return identity_; }
    object_id const & catalog_identity() const && = delete;
    std::filesystem::path const & root() const & noexcept { return catalog_.root(); }
    std::filesystem::path const & root() const && = delete;

    template <class Compose> auto make_merge(native_pointer older, native_pointer newer, Compose compose) {
      require_active();
      try {
        auto output = ids_(); object_attempt_id attempt(ids_().hex());
        auto owner = ids_().hex(); auto operation = ids_().hex();
        std::array<catalog_object_reservation, 1> reservation{{{output, file_kind::native_blob}}};
        // Current published roots already retain durable inputs. Private
        // uncommitted inputs need not survive a failed logical publication.
        catalog_.reserve(operation, attempt, owner, {}, reservation);
        return std::make_unique<merge_type<Compose>>(root(), output, attempt,
          std::move(older), std::move(newer), file_ops_, std::move(compose));
      } catch (...) { failed_ = true; throw; }
    }
    template <class Merge> native_pointer finish_merge(Merge & merge) {
      require_active();
      try {
        auto receipt = merge.finish();
        catalog_.record_sealed(ids_().hex(), receipt);
        auto native = native_type::from_sealed(root(), identity_, std::move(receipt));
        ++sealed_outputs_; return native;
      } catch (...) { failed_ = true; throw; }
    }
  private:
    catalog_type catalog_;
    Ids ids_;
    FileOps file_ops_;
    object_id identity_;
    std::uint64_t sealed_outputs_ = 0;
    bool failed_ = false;
    sort_runtime_context(catalog_type catalog, Ids ids, FileOps file_ops)
      : catalog_(std::move(catalog)), ids_(std::move(ids)), file_ops_(std::move(file_ops)), identity_(catalog_.identity()) {}
    void require_active() const { if (failed()) throw std::logic_error("failed sort runtime context"); }
  };

  template <class P, class Selector = registry_selector<typename P::registry_type>, class Ids = random_object_ids,
            class CatalogOps = sqlite_catalog_ops, class FileOps = posix_object_ops>
  struct sort_file_runtime_storage : sort_runtime_storage<P, Selector> {
    using context_type = sort_runtime_context<P, Selector, Ids, CatalogOps, FileOps>;
    using native_type = typename context_type::native_type;
    using native_pointer = typename context_type::native_pointer;
    template <class Compose> using merge_type = typename context_type::template merge_type<Compose>;
    sort_file_runtime_storage() = default; // Empty seed snapshots do not start a merge.
    explicit sort_file_runtime_storage(std::shared_ptr<context_type> context) : context_(std::move(context)) {
      if (!context_) throw std::invalid_argument("null sort runtime context");
    }
    static sort_file_runtime_storage open(std::filesystem::path const & root, Ids ids = {},
        catalog_options options = {}, CatalogOps catalog_ops = {}, FileOps file_ops = {}) {
      return sort_file_runtime_storage(context_type::open(root, std::move(ids), options, std::move(catalog_ops), std::move(file_ops)));
    }
    std::shared_ptr<context_type> context() const noexcept { return context_; }
    void poison() noexcept { if (context_) context_->poison(); }
    template <class Compose> auto make_merge(native_pointer older, native_pointer newer, Compose compose) {
      require_context(); return context_->make_merge(std::move(older), std::move(newer), std::move(compose));
    }
    template <class Merge> native_pointer finish_merge(Merge & merge) { require_context(); return context_->finish_merge(merge); }
  private:
    std::shared_ptr<context_type> context_;
    void require_context() const { if (!context_) throw std::logic_error("streamed merge needs an opened storage context"); }
  };

  template <class P = string_policy, class Selector = registry_selector<typename P::registry_type>, class Ids = random_object_ids,
            class CatalogOps = sqlite_catalog_ops, class FileOps = posix_object_ops>
  using streaming_sort_runtime_family = sort_runtime_family<P, Selector, sort_file_runtime_storage<P, Selector, Ids, CatalogOps, FileOps>>;
}
