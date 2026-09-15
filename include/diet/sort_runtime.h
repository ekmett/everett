/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Runs typed updates through sort-owned native files and the shared redundant scheduler.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <diet/redundant_runtime.h>
#include <diet/sort_profile_file.h>
#include <diet/sort_profile_merge.h>
#include <diet/typed_cola.h>

namespace diet {
  // Logical query keys carry their length separately: sort bits followed by
  // leaf order bits. They never acquire the opaque string transport's escapes.
  template <class P, class Selector = registry_selector<typename P::registry_type>> struct sort_key_transport {
    template <class S> static bit_string encode(typename sort_codec<S>::key_codec::value_type const & key) {
      return sort_profile_query<P, S, Selector>(key);
    }
    template <class S> static bit_string prefix() {
      bit_string result; sort_bit_writer out(result); Selector::template write<S>(out); return result;
    }
    template <class S> static auto decode(bit_view bits) {
      return sort_profile_key<typename sort_codec<S>::key_codec>::decode_order(bits);
    }
    template <class F> static decltype(auto) dispatch(bit_view bits, F && fn) {
      sort_bit_reader input(bits);
      return Selector::select(input, [&]<class S>(std::type_identity<S> tag, auto & source) -> decltype(auto) {
        auto key = decode<S>(source.take_bits(source.remaining()));
        return std::invoke(std::forward<F>(fn), tag, key);
      });
    }
  };

  template <class P, class Selector = registry_selector<typename P::registry_type>> struct sort_runtime_native {
    using policy_type = P;
    using stream_family = sort_profile_family<P, Selector>;
    using array_type = sort_profile_array<P, Selector>;
    using mapped_type = mapped_sort_profile<P, Selector>;
    using pointer = std::shared_ptr<sort_runtime_native const>;
    static pointer from_owned(array_type value) {
      return pointer(new sort_runtime_native(std::make_shared<array_type const>(std::move(value))));
    }
    static pointer from_mapped(std::shared_ptr<mapped_type const> value) {
      if (!value) throw std::invalid_argument("null mapped sort runtime native");
      return pointer(new sort_runtime_native(std::move(value)));
    }
    auto view() const { return owned_ ? owned_->view() : mapped_->view(); }
    std::uint64_t size() const { return view().size(); }
    std::shared_ptr<array_type const> owned() const noexcept { return owned_; }
    std::shared_ptr<mapped_type const> mapped() const noexcept { return mapped_; }
  private:
    std::shared_ptr<array_type const> owned_;
    std::shared_ptr<mapped_type const> mapped_;
    explicit sort_runtime_native(std::shared_ptr<array_type const> value) : owned_(std::move(value)) {}
    explicit sort_runtime_native(std::shared_ptr<mapped_type const> value) : mapped_(std::move(value)) {}
  };

  template <class P, class Selector = registry_selector<typename P::registry_type>> struct sort_runtime_storage {
    static_assert(P::unit == profile_unit::bit, "sort runtime uses bit-addressed records");
    using native_type = sort_runtime_native<P, Selector>;
    using mapped_native_type = mapped_sort_profile<P, Selector>;
    using mapped_pair_type = mapped_sort_cola<P, Selector>;
    static auto encode_native(sort_profile_array<P, Selector> const & value) { return encoded_sort_sections<P>::from(value); }
    template <class Compose> using merge_type = sort_profile_merge_builder<P, native_type, Compose, Selector>;
    static auto empty() { sort_profile_writer<P, Selector> writer; return native_type::from_owned(writer.finish()); }
    static auto singleton(profile_record const & record) {
      using leaves = typename registry_detail::info<typename P::registry_type>::leaves;
      sort_profile_writer<P, Selector> writer;
      sort_bit_reader input(record.key.view());
      Selector::select(input, [&]<class S>(std::type_identity<S>, auto & source) {
        sort_profile_frame frame;
        frame.path = record.key.view().prefix(source.position());
        frame.leaf = sort_profile_detail::ordinal<leaves, S>::value;
        frame.key_units = record.key.bit_size;
        frame.front_coded = sort_profile_key<typename sort_codec<S>::key_codec>::front_coded;
        std::array<bit_view, 1> spans{record.key.view()};
        writer.append_frame(frame, spans, 0, record.value.view());
      });
      return native_type::from_owned(writer.finish());
    }
  };

  // Policy/schema dispatch can select this concrete family before entering a
  // run. Selector is a protocol implementation, not necessarily a binary tree.
  template <class P = string_policy, class Selector = registry_selector<typename P::registry_type>>
  struct sort_runtime_family : redundant_runtime_family<P, sort_runtime_storage<P, Selector>> {
    using key_transport = sort_key_transport<P, Selector>;
    static std::string default_schema() {
      if constexpr (std::same_as<typename P::registry_type, string_registry> &&
                    std::same_as<Selector, registry_selector<typename P::registry_type>>)
        return "diet.optional-string/code0/sort-profile-v1";
      else return {};
    }
  };
}
