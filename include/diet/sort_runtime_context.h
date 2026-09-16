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
#include <diet/cola_file_index.h>
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
    template <class Node> struct index_type : private context_pin, public cola_file_index_builder<P, native_type, Node, FileOps> {
      using base_type = cola_file_index_builder<P, native_type, Node, FileOps>;
    private:
      friend struct sort_runtime_context;
      using mapped_type = typename Node::storage_type::mapped_pair_type;
      using native_binding_pointer = std::shared_ptr<native_binding<typename mapped_type::native_type> const>;
      using pair_binding_pointer = std::shared_ptr<pair_binding<mapped_type> const>;
      native_binding_pointer native_, secondary_;
      pair_binding_pointer main_;
      index_type(std::shared_ptr<sort_runtime_context> owner, object_id output, object_attempt_id attempt,
          native_pointer native, typename Node::pair_type main, native_pointer secondary,
          native_binding_pointer n, pair_binding_pointer m, native_binding_pointer s)
        : context_pin(owner), base_type(owner->root(), std::move(output), std::move(attempt),
            {n->receipt.object, m ? std::optional<blob_identity>(m->identity) : std::nullopt,
              s ? std::optional<object_id>(s->receipt.object) : std::nullopt},
            std::move(native), std::move(main), std::move(secondary), owner->file_ops_, owner->spool_ops_),
          native_(std::move(n)), secondary_(std::move(s)), main_(std::move(m)) {}
    };
    template <class Node> auto make_index(native_pointer native, typename Node::pair_type main, native_pointer secondary) {
      require_active();
      try {
        using family = sort_runtime_family<P, Selector, typename Node::storage_type>;
        runtime_store_detail::graph_sealer<P, Ids, CatalogOps, family> sealer(catalog_, ids_);
        auto n = sealer.ensure_native(native);
        auto m = main ? sealer.ensure_pair(main) : nullptr;
        auto s = secondary ? sealer.ensure_native(secondary) : nullptr;
        auto output = ids_(); object_attempt_id attempt(ids_().hex());
        std::array<catalog_object_reservation, 1> reservation{{{output, file_kind::fractional_index}}};
        std::array<blob_identity, 1> inputs{m ? m->identity : blob_identity{n->receipt.object, output}};
        auto owner = ids_().hex(); auto operation = ids_().hex();
        catalog_.reserve(operation, attempt, owner, std::span<blob_identity const>(inputs.data(), m ? 1 : 0), reservation);
        return std::unique_ptr<index_type<Node>>(new index_type<Node>(this->shared_from_this(), output, attempt,
          std::move(native), std::move(main), std::move(secondary), std::move(n), std::move(m), std::move(s)));
      } catch (...) { failed_ = true; throw; }
    }
    template <class Node> typename Node::pair_type finish_index(index_type<Node> & index) {
      require_active();
      try {
        using family = sort_runtime_family<P, Selector, typename Node::storage_type>;
        using mapped_type = typename family::storage_type::mapped_pair_type;
        if (index.owner_.get() != this) throw std::invalid_argument("index belongs to another runtime context");
        auto receipt = index.finish();
        auto native = index.native_owner(); auto main = index.main_target(); auto secondary = index.secondary_target();
        // Construction already acknowledged these exact dependencies. The job
        // retains their authority and mappings along with its source facades.
        auto const & n = index.native_; auto const & m = index.main_; auto const & s = index.secondary_;
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
