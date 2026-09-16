/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Owns catalog reservations and streamed native and index outputs for one runtime worker.
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
#include <diet/sort_profile_adaptive.h>
#include <diet/cola_adaptive_index.h>
#include <diet/runtime_graph_sealer.h>
#include <diet/output_budget.h>

namespace diet {
  struct runtime_output_options {
    std::size_t retained_bytes = std::size_t{8} << 20;
    std::size_t object_bytes = std::size_t{128} << 10;
  };
  // One serialized worker owns this concrete context. Storage handles share
  // its lifetime when an equivalent mapped publication rebases the core; they
  // do not add concurrent access or locks. The first output spill may reserve
  // files while appending a record; subsequent appends reuse that attempt.
  template <class P, class Selector = registry_selector<typename P::registry_type>, class Ids = random_object_ids,
            class CatalogOps = sqlite_catalog_ops, class FileOps = posix_object_ops>
  struct sort_runtime_context : std::enable_shared_from_this<sort_runtime_context<P, Selector, Ids, CatalogOps, FileOps>> {
  private:
    // Listed before each writer base, so the writer closes its borrowed Ops
    // and scratch descriptors before releasing the final context owner.
    struct context_pin {
      std::shared_ptr<sort_runtime_context> owner_;
      explicit context_pin(std::shared_ptr<sort_runtime_context> owner) : owner_(std::move(owner)) {}
    };
    struct native_stream_factory {
      sort_runtime_context * owner;
      auto operator()() { return owner->start_native_stream(); }
      void poison() noexcept { owner->poison(); }
    };
  public:
    using native_type = sort_runtime_native<P, Selector>;
    using native_pointer = std::shared_ptr<native_type const>;
    using catalog_type = sqlite_catalog<P, CatalogOps>;
    template <class Compose> struct merge_type : private context_pin,
        public sort_profile_adaptive_merge<P, native_type, Compose, Selector, FileOps, native_stream_factory> {
      using base_type = sort_profile_adaptive_merge<P, native_type, Compose, Selector, FileOps, native_stream_factory>;
    private:
      friend struct sort_runtime_context;
      merge_type(std::shared_ptr<sort_runtime_context> owner, native_pointer older, native_pointer newer, Compose compose)
        : context_pin(owner), base_type(native_stream_factory{owner.get()}, owner->budget_, owner->outputs_.object_bytes,
            std::move(older), std::move(newer), std::move(compose)) {}
    };
    static std::shared_ptr<sort_runtime_context> open(std::filesystem::path const & root, Ids ids = {},
        catalog_options options = {}, CatalogOps catalog_ops = {}, FileOps file_ops = {},
        runtime_output_options outputs = {}) {
      auto catalog = catalog_type::open(root, options, std::move(catalog_ops));
      if (catalog.schema_version() != 4) throw std::invalid_argument("streamed runtime requires a named catalog");
      return std::shared_ptr<sort_runtime_context>(new sort_runtime_context(std::move(catalog), std::move(ids), std::move(file_ops), outputs));
    }
    sort_runtime_context(sort_runtime_context const &) = delete;
    sort_runtime_context & operator=(sort_runtime_context const &) = delete;
    sort_runtime_context(sort_runtime_context &&) = delete;
    sort_runtime_context & operator=(sort_runtime_context &&) = delete;
    bool failed() const noexcept { return failed_ || catalog_.poisoned(); }
    void poison() noexcept { failed_ = true; }
    std::uint64_t sealed_outputs() const noexcept { return sealed_outputs_; }
    std::uint64_t sealed_indexes() const noexcept { return sealed_indexes_; }
    std::size_t retained_output_bytes() const noexcept { return budget_.used(); }
    std::size_t output_limit() const noexcept { return budget_.limit(); }
    native_pointer empty() const noexcept { return empty_; }
    object_id const & catalog_identity() const & noexcept { return identity_; }
    object_id const & catalog_identity() const && = delete;
    std::filesystem::path const & root() const & noexcept { return catalog_.root(); }
    std::filesystem::path const & root() const && = delete;

    template <class Compose> auto make_merge(native_pointer older, native_pointer newer, Compose compose) {
      require_active();
      try {
        return std::unique_ptr<merge_type<Compose>>(new merge_type<Compose>(this->shared_from_this(),
          std::move(older), std::move(newer), std::move(compose)));
      } catch (...) { failed_ = true; throw; }
    }
    template <class Merge> native_pointer finish_merge(Merge & merge) {
      require_active();
      try {
        if (merge.owner_.get() != this) throw std::invalid_argument("merge belongs to another runtime context");
        auto completed = merge.finish();
        if (auto owned = std::get_if<0>(&completed)) return native_type::from_owned(std::move(*owned));
        auto receipt = std::get<1>(std::move(completed));
        catalog_.record_sealed(ids_().hex(), receipt);
        auto native = native_type::from_sealed(root(), identity_, std::move(receipt));
        ++sealed_outputs_; return native;
      } catch (...) { failed_ = true; throw; }
    }
    template <class Node> struct index_factory {
      using family = sort_runtime_family<P, Selector, typename Node::storage_type>;
      using mapped_type = typename family::storage_type::mapped_pair_type;
      using destination_type = cola_detail::index_destination<P, FileOps, posix_index_spool_ops>;
      std::shared_ptr<sort_runtime_context> owner_;
      native_pointer native, secondary;
      typename Node::pair_type main;
      std::shared_ptr<native_binding<typename mapped_type::native_type> const> native_, secondary_;
      std::shared_ptr<pair_binding<mapped_type> const> main_;
      void poison() noexcept { owner_->poison(); }
      std::unique_ptr<destination_type> operator()() {
        auto & owner = *owner_; owner.require_active();
        try {
          runtime_store_detail::graph_sealer<P, Ids, CatalogOps, family> sealer(owner.catalog_, owner.ids_);
          native_ = sealer.ensure_native(native);
          main_ = main ? sealer.ensure_pair(main) : nullptr;
          secondary_ = secondary ? sealer.ensure_native(secondary) : nullptr;
          auto output = owner.ids_(); object_attempt_id attempt(owner.ids_().hex());
          std::array<catalog_object_reservation, 1> reservation{{{output, file_kind::fractional_index}}};
          std::array<blob_identity, 1> inputs{main_ ? main_->identity : blob_identity{native_->receipt.object, output}};
          auto pin = owner.ids_().hex(); auto operation = owner.ids_().hex();
          owner.catalog_.reserve(operation, attempt, pin, std::span<blob_identity const>(inputs.data(), main_ ? 1 : 0), reservation);
          cola_file_dependencies dependencies{native_->receipt.object,
            main_ ? std::optional<blob_identity>(main_->identity) : std::nullopt,
            secondary_ ? std::optional<object_id>(secondary_->receipt.object) : std::nullopt};
          return std::make_unique<destination_type>(owner.root(), std::move(output), std::move(attempt),
            std::move(dependencies), owner.file_ops_, owner.spool_ops_);
        } catch (...) { owner.failed_ = true; throw; }
      }
    };
    template <class Node> using index_type = cola_adaptive_index_builder<P, native_type, Node, index_factory<Node>, FileOps>;
    template <class Node> auto make_index(native_pointer native, typename Node::pair_type main, native_pointer secondary) {
      require_active();
      try {
        index_factory<Node> factory{this->shared_from_this(), native, secondary, main, {}, {}, {}};
        return std::make_unique<index_type<Node>>(std::move(factory), budget_, outputs_.object_bytes,
          std::move(native), std::move(main), std::move(secondary));
      } catch (...) { failed_ = true; throw; }
    }
    template <class Node> typename Node::pair_type finish_index(index_type<Node> & index) {
      require_active();
      try {
        using family = sort_runtime_family<P, Selector, typename Node::storage_type>;
        using mapped_type = typename family::storage_type::mapped_pair_type;
        if (index.factory().owner_.get() != this) throw std::invalid_argument("index belongs to another runtime context");
        auto output = index.finish();
        if (auto built = std::get_if<std::shared_ptr<typename Node::built_type const>>(&output))
          return Node::from_built(std::move(*built));
        auto receipt = std::get<object_seal_receipt>(std::move(output));
        auto native = index.native_owner(); auto main = index.main_target(); auto secondary = index.secondary_target();
        // The first spill acknowledged these exact dependencies. The job
        // retains their authority and mappings along with its source facades.
        auto const & factory = index.factory();
        auto const & n = factory.native_; auto const & m = factory.main_; auto const & s = factory.secondary_;
        blob_identity identity{n->receipt.object, receipt.object};
        auto mapped_index = catalog_.template seal_pair<mapped_type>(ids_().hex(), identity, receipt);
        auto mapped = mapped_type::bind(identity, n->mapped, std::move(mapped_index), m ? m->mapped : nullptr,
          s ? s->mapped : nullptr, s ? std::optional<object_id>(s->receipt.object) : std::nullopt);
        auto binding = std::make_shared<pair_binding<mapped_type> const>(pair_seal{identity_, identity, std::move(receipt)}, std::move(mapped));
        auto result = Node::from_sealed_parts(std::move(binding), root(), std::move(native), std::move(main), std::move(secondary));
        ++sealed_indexes_; return result;
      } catch (...) { failed_ = true; throw; }
    }
  private:
    catalog_type catalog_;
    Ids ids_;
    FileOps file_ops_;
    posix_index_spool_ops spool_ops_;
    runtime_output_options outputs_;
    output_budget budget_;
    native_pointer empty_ = sort_runtime_storage<P, Selector>::empty();
    object_id identity_;
    std::uint64_t sealed_outputs_ = 0, sealed_indexes_ = 0;
    bool failed_ = false;
    sort_runtime_context(catalog_type catalog, Ids ids, FileOps file_ops, runtime_output_options outputs)
      : catalog_(std::move(catalog)), ids_(std::move(ids)), file_ops_(std::move(file_ops)), outputs_(outputs),
        budget_(outputs.retained_bytes), identity_(catalog_.identity()) {}
    std::unique_ptr<object_stream<P, FileOps>> start_native_stream() {
      require_active();
      try {
        auto output = ids_(); object_attempt_id attempt(ids_().hex());
        auto owner = ids_().hex(); auto operation = ids_().hex();
        std::array<catalog_object_reservation, 1> reservation{{{output, file_kind::native_blob}}};
        // Current roots retain durable inputs. Private outputs may be replayed
        // from that published frontier if this logical publication fails.
        catalog_.reserve(operation, attempt, owner, {}, reservation);
        return std::make_unique<object_stream<P, FileOps>>(root(), std::move(output), std::move(attempt),
          file_kind::native_blob, 192, file_ops_);
      } catch (...) { failed_ = true; throw; }
    }
    void require_active() const { if (failed()) throw std::logic_error("failed sort runtime context"); }
  };

  template <class P, class Selector = registry_selector<typename P::registry_type>, class Ids = random_object_ids,
            class CatalogOps = sqlite_catalog_ops, class FileOps = posix_object_ops>
  struct sort_file_runtime_storage : sort_runtime_storage<P, Selector> {
    using clean_storage_type = sort_runtime_storage<P, Selector>;
    using context_type = sort_runtime_context<P, Selector, Ids, CatalogOps, FileOps>;
    using native_type = typename context_type::native_type;
    using native_pointer = typename context_type::native_pointer;
    template <class Compose> using merge_type = typename context_type::template merge_type<Compose>;
    sort_file_runtime_storage() = default; // Empty seed snapshots do not start file output.
    explicit sort_file_runtime_storage(std::shared_ptr<context_type> context) : context_(std::move(context)) {
      if (!context_) throw std::invalid_argument("null sort runtime context");
    }
    static sort_file_runtime_storage open(std::filesystem::path const & root, Ids ids = {},
        catalog_options options = {}, CatalogOps catalog_ops = {}, FileOps file_ops = {},
        runtime_output_options outputs = {}) {
      return sort_file_runtime_storage(context_type::open(root, std::move(ids), options, std::move(catalog_ops), std::move(file_ops), outputs));
    }
    std::shared_ptr<context_type> context() const noexcept { return context_; }
    native_pointer empty() const { return context_ ? context_->empty() : sort_runtime_storage<P, Selector>::empty(); }
    void poison() noexcept { if (context_) context_->poison(); }
    template <class Compose> auto make_merge(native_pointer older, native_pointer newer, Compose compose) {
      require_context(); return context_->make_merge(std::move(older), std::move(newer), std::move(compose));
    }
    template <class Merge> native_pointer finish_merge(Merge & merge) { require_context(); return context_->finish_merge(merge); }
    template <class Node> using index_type = typename context_type::template index_type<Node>;
    template <class Node> auto make_index(native_pointer native, typename Node::pair_type main = {}, native_pointer secondary = {}) {
      require_context(); return context_->template make_index<Node>(std::move(native), std::move(main), std::move(secondary));
    }
    template <class Node> auto finish_index(index_type<Node> & index) { require_context(); return context_->template finish_index<Node>(index); }
  private:
    std::shared_ptr<context_type> context_;
    void require_context() const { if (!context_) throw std::logic_error("streamed output needs an opened storage context"); }
  };

  template <class P = string_policy, class Selector = registry_selector<typename P::registry_type>, class Ids = random_object_ids,
            class CatalogOps = sqlite_catalog_ops, class FileOps = posix_object_ops>
  using streaming_sort_runtime_family = sort_runtime_family<P, Selector, sort_file_runtime_storage<P, Selector, Ids, CatalogOps, FileOps>>;
}
