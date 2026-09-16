/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Keeps small encoded indexes in memory and spills the same IX03 stream lazily.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/cola_file_index.h>
#include <diet/output_budget.h>
#include <variant>

namespace diet {
  namespace cola_detail {
    template <class P, class Ops, class SpoolOps> struct index_destination {
      cola_file_dependencies dependencies;
      object_stream<P, Ops> stream;
      secondary_spool<SpoolOps> spool;
      index_destination(std::filesystem::path root, object_id id, object_attempt_id attempt,
          cola_file_dependencies deps, Ops & ops, SpoolOps & spool_ops)
        : dependencies(std::move(deps)), stream(std::move(root), std::move(id), std::move(attempt),
            file_kind::fractional_index, cola_section_detail::directory_bytes, ops),
          spool(stream.paths().private_output.string() + ".secondary", spool_ops) {}
    };

    // Factory acquires a physical destination only at the first spill. It owns
    // any borrowed Ops, so it outlives the destination and both staged sinks.
    template <class P, class Native, class Main, class Ops, class SpoolOps, class Factory>
    struct adaptive_index_output {
      using index_type = cola_index<P, Native, Main>;
      using destination_type = index_destination<P, Ops, SpoolOps>;
      using result_type = std::variant<std::shared_ptr<index_type const>, object_seal_receipt>;
      struct route_stream {
        adaptive_index_output * owner;
        unsigned route;
        void append(std::span<std::byte const> bytes) {
          if (bytes.empty()) return;
          auto & target = owner->destination();
          if (!route) target.stream.append(bytes); else target.spool.append(bytes);
        }
        std::uint64_t body_bytes() const noexcept {
          if (!owner->destination_) return !route ? cola_section_detail::directory_bytes : 0;
          return !route ? owner->destination_->stream.body_bytes() : owner->destination_->spool.body_bytes();
        }
      };
      using sink_type = sort_profile_detail::file_bit_sink<P, Ops, route_stream>;
      adaptive_index_output(Factory factory, output_budget budget, std::size_t limit)
        : factory_(std::move(factory)), budget_(std::move(budget)), limit_(limit),
          routes_{{{this, 0}, {this, 1}}}, main_(routes_[0]), secondary_(routes_[1]) {
        if (!limit_ || !budget_.limit()) (void)destination();
      }
      adaptive_index_output(adaptive_index_output const &) = delete;
      adaptive_index_output & operator=(adaptive_index_output const &) = delete;
      bool failed() const noexcept { return failed_ || (destination_ && destination_->stream.failed()); }
      bool finished() const noexcept { return finished_; }
      bool spilled() const noexcept { return bool(destination_); }
      Factory const & factory() const noexcept { return factory_; }
      object_write_paths const & paths() const & {
        if (!destination_) throw std::logic_error("index has no physical output");
        return destination_->stream.paths();
      }
      object_write_paths const & paths() const && = delete;
      std::uint64_t spooled_bytes() const noexcept { return destination_ ? destination_->spool.body_bytes() : 0; }
      void append_known(unsigned route, bit_view key, std::uint64_t common) {
        require_active();
        try {
          if (!route) profiles_[0].append(main_, key, common);
          else if (route == 1) profiles_[1].append(secondary_, key, common);
          else throw std::out_of_range("adaptive index route");
        } catch (...) { poison(); throw; }
      }
      result_type finish(typename index_type::native_pointer native, typename index_type::main_pointer main,
          typename index_type::native_pointer secondary, index_metadata<P> metadata) {
        require_active();
        try {
          profiles_[0].finish_metadata(main_.position());
          profiles_[1].finish_metadata(secondary_.position());
          if (!spilled()) {
            // Copy only the bounded staged payload; all sparse arrays move.
            std::array<bit_string, 2> payload{bit_string::copy(main_.buffered_payload()),
                                            bit_string::copy(secondary_.buffered_payload())};
            auto charge = retained_bytes(payload, metadata);
            if (charge <= limit_) if (auto lease = budget_.try_acquire(charge)) {
              using adoption = profile_detail::borrowed_sections<P>;
              std::array<typename index_type::borrowed_array, 2> borrowed{
                adoption::adopt(std::move(payload[0].bytes), std::move(profiles_[0].offsets), profiles_[0].metadata),
                adoption::adopt(std::move(payload[1].bytes), std::move(profiles_[1].offsets), profiles_[1].metadata)};
              auto built = index_output<P, Native, Main>::adopt(std::move(native), std::move(main), std::move(secondary),
                std::move(borrowed), std::move(metadata));
              auto owned = output_budget::attach(std::move(built), std::move(*lease));
              finished_ = true; return owned;
            }
          }
          auto & target = destination();
          if (!native || bool(main) != bool(target.dependencies.main) || bool(secondary) != bool(target.dependencies.secondary))
            throw std::invalid_argument("adaptive COLA dependency shape");
          main_.finish_payload(); secondary_.finish_payload();
          auto receipt = seal_file_index<P>(target.dependencies, profiles_, metadata, native->size(),
            main_, target.stream, target.spool);
          finished_ = true; return receipt;
        } catch (...) { poison(); throw; }
      }
    private:
      Factory factory_;
      output_budget budget_;
      std::size_t limit_;
      std::unique_ptr<destination_type> destination_;
      std::array<route_stream, 2> routes_;
      sink_type main_, secondary_;
      std::array<borrowed_file_state<P>, 2> profiles_;
      bool failed_ = false, finished_ = false;
      void poison() noexcept {
        failed_ = true;
        if constexpr (requires { { factory_.poison() } noexcept; }) factory_.poison();
      }
      destination_type & destination() {
        if (!destination_) {
          destination_ = factory_();
          if (!destination_) throw std::logic_error("null adaptive index destination");
        }
        return *destination_;
      }
      void require_active() const { if (failed() || finished()) throw std::logic_error("inactive adaptive index output"); }
      std::size_t retained_bytes(std::array<bit_string, 2> const & payload, index_metadata<P> const & metadata) const {
        std::uint64_t bytes = sizeof(index_type) + sizeof(output_budget::lease);
        auto add = [&](auto const & values) {
          bytes = profile_detail::add(bytes, profile_detail::multiply(values.capacity(), sizeof(typename std::remove_cvref_t<decltype(values)>::value_type)));
        };
        for (unsigned route = 0; route != 2; ++route) {
          add(payload[route].bytes);
          auto const & offsets = profiles_[route].offsets;
          add(offsets.low); add(offsets.high); add(offsets.samples); add(offsets.sparse);
          add(metadata.ranks[route].classes); add(metadata.ranks[route].checkpoints);
          add(metadata.flags[route]); add(metadata.cuts[route]);
        }
        if (bytes > std::numeric_limits<std::size_t>::max()) throw std::length_error("retained index extent");
        return static_cast<std::size_t>(bytes);
      }
    };
  }

  template <class P, class Native, class Main, class Factory,
            class Ops = posix_object_ops, class SpoolOps = posix_index_spool_ops>
  struct cola_adaptive_index_builder {
    using output_type = cola_detail::adaptive_index_output<P, Native, Main, Ops, SpoolOps, Factory>;
    using builder_type = cola_index_builder<P, Native, Main, cola_detail::file_index_output_ref<output_type>>;
    using native_pointer = typename builder_type::native_pointer;
    using main_pointer = typename builder_type::main_pointer;
    cola_adaptive_index_builder(Factory factory, output_budget budget, std::size_t limit,
        native_pointer native, main_pointer main = {}, native_pointer secondary = {})
      : output_(std::move(factory), std::move(budget), limit),
        builder_({&output_}, std::move(native), std::move(main), std::move(secondary)) {}
    cola_adaptive_index_builder(cola_adaptive_index_builder const &) = delete;
    cola_adaptive_index_builder & operator=(cola_adaptive_index_builder const &) = delete;
    bool done() const noexcept { return builder_.done(); }
    bool failed() const noexcept { return builder_.failed(); }
    bool finished() const noexcept { return builder_.finished(); }
    bool spilled() const noexcept { return output_.spilled(); }
    std::uint64_t size() const noexcept { return builder_.size(); }
    native_pointer native_owner() const noexcept { return builder_.native_owner(); }
    main_pointer main_target() const noexcept { return builder_.main_target(); }
    native_pointer secondary_target() const noexcept { return builder_.secondary_target(); }
    std::uint64_t step(std::uint64_t budget) { return builder_.step(budget); }
    auto finish() { return builder_.finish(); }
    Factory const & factory() const noexcept { return output_.factory(); }
    std::uint64_t spooled_bytes() const noexcept { return output_.spooled_bytes(); }
    object_write_paths const & paths() const & { return output_.paths(); }
    object_write_paths const & paths() const && = delete;
  private:
    output_type output_;
    builder_type builder_;
  };
}
