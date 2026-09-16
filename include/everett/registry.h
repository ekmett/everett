/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Describes sort codes, encoding requirements and typed registry dispatch.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <everett/error_detail.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace everett {
  enum class profile_unit : std::uint8_t { byte, bit };

  struct variable_values {};
  template <std::uint64_t N> struct fixed_values {
    static constexpr std::uint64_t width = N;
  };

  namespace registry_detail {
    template <class Values, unsigned Shift> struct value_bits;
    template <unsigned Shift> struct value_bits<variable_values, Shift> {
      static constexpr std::optional<std::uint64_t> value = std::nullopt;
    };
    template <std::uint64_t N, unsigned Shift> struct value_bits<fixed_values<N>, Shift> {
      static_assert(N <= (std::numeric_limits<std::uint64_t>::max() >> Shift),
                    "fixed value width exceeds a bit extent");
      static constexpr std::optional<std::uint64_t> value = N << Shift;
    };
  }

  // Requirements for already encoded key/value payloads. These helpers do not
  // supply a semantic key codec or establish prefix freedom of caller data.
  // Widths in Values are bytes here and bits in bit_encoding respectively.
  template <class Values = variable_values> struct byte_encoding {
    static constexpr profile_unit unit = profile_unit::byte;
    static constexpr auto fixed_value_bits = registry_detail::value_bits<Values, 3>::value;
  };
  template <class Values = variable_values> struct bit_encoding {
    static constexpr profile_unit unit = profile_unit::bit;
    static constexpr auto fixed_value_bits = registry_detail::value_bits<Values, 0>::value;
  };
  template <class Codec> struct encoded_sort { using encoding = Codec; };

  // Layout inference only: semantic value encoding and tombstone meaning stay
  // with the value codec. Specialize this trait for types without T::encoding.
  template <class T> struct value_encoding { using type = typename T::encoding; };
  template <> struct value_encoding<std::string> { using type = byte_encoding<>; };
  template <> struct value_encoding<std::optional<std::string>> { using type = byte_encoding<>; };

  // One tagless sort. Its type still identifies the sort to a typed visitor;
  // no discriminator precedes its key. Native keys must still be sorted.
  // This does not implement value codecs.
  template <class T> struct unsorted {
    using value_type = T;
    using encoding = typename value_encoding<T>::type;
  };
  // A reserved hole admits no sort and imposes neither unit nor value width.
  // As a standalone empty registry it defaults to byte addressing.
  struct sort_undefined {};
  template <class S> struct tip { using sort_type = S; };
  template <class L, class R> struct bin { using left_type = L; using right_type = R; };
  template <class... S> struct sort_list {};

  namespace registry_detail {
    template <class T> inline constexpr bool hole = std::is_same_v<T, sort_undefined>;
    template <class... S> struct sorts {};
    template <class A, class B> struct concatenate;
    template <class... A, class... B> struct concatenate<sorts<A...>, sorts<B...>> {
      using type = sorts<A..., B...>;
    };
    template <class S> struct unique;
    template <> struct unique<sorts<>> : std::true_type {};
    template <class A, class... B> struct unique<sorts<A, B...>>
      : std::bool_constant<(!std::is_same_v<A, B> && ...) && unique<sorts<B...>>::value> {};

    struct empty {
      using leaves = sorts<>;
      static constexpr std::size_t count = 0;
      static constexpr std::optional<profile_unit> mode = std::nullopt;
      static constexpr std::optional<std::uint64_t> width = std::nullopt;
    };
    template <class S, bool = hole<S>> struct leaf : empty {};
    template <class S> struct leaf<S, false> {
      using encoding = typename S::encoding;
      using leaves = sorts<S>;
      static constexpr std::size_t count = 1;
      static constexpr std::optional<profile_unit> mode = encoding::unit;
      static_assert(*mode == profile_unit::byte || *mode == profile_unit::bit,
                    "sort encoding has an invalid storage unit");
      static constexpr std::optional<std::uint64_t> width = encoding::fixed_value_bits;
      static_assert(*mode != profile_unit::byte || !width || (*width & 7) == 0,
                    "byte sort values must occupy whole bytes");
    };

    template <class A, class B> struct join {
      using leaves = typename concatenate<typename A::leaves, typename B::leaves>::type;
      static constexpr std::size_t count = A::count + B::count;
      static constexpr std::optional<std::uint64_t> width = !A::count ? B::width :
        !B::count ? A::width : A::width && B::width && A::width == B::width ? A::width : std::nullopt;
    };
    template <class... S> struct list_width : empty {};
    template <class S, class... Rest> struct list_width<S, Rest...>
      : join<leaf<S>, list_width<Rest...>> {};

    template <class R> struct info;
    template <class T> struct info<unsorted<T>> : leaf<unsorted<T>> {};
    template <> struct info<sort_undefined> : empty {};
    template <class S> struct info<tip<S>> : leaf<S> {};
    template <class L, class R> struct info<bin<L, R>> : join<info<L>, info<R>> {
      static constexpr std::optional<profile_unit> mode = profile_unit::bit;
    };
    template <class... S> struct info<sort_list<S...>> : list_width<S...> {
      static_assert(sizeof...(S) <= 256, "a byte sort list has at most 256 codes");
      static_assert(((!leaf<S>::mode || *leaf<S>::mode == profile_unit::byte) && ...),
                    "a byte sort list requires byte encodings");
      static constexpr std::optional<profile_unit> mode = profile_unit::byte;
    };

    template <class Old, class New> struct extends;
    template <class Old, class New> struct extension : std::is_same<Old, New> {};
    template <class T> struct extension<unsorted<T>, tip<unsorted<T>>> : std::true_type {};
    template <class T> struct extension<tip<unsorted<T>>, unsorted<T>> : std::true_type {};
    template <class OL, class OR, class NL, class NR>
    struct extension<bin<OL, OR>, bin<NL, NR>>
      : std::bool_constant<extends<OL, NL>::value && extends<OR, NR>::value> {};
    template <class Old, class New, std::size_t... I>
    consteval bool list_extends(std::index_sequence<I...>) {
      if constexpr (std::tuple_size_v<Old> > std::tuple_size_v<New>) return false;
      else return ((hole<std::tuple_element_t<I, Old>> ||
        std::is_same_v<std::tuple_element_t<I, Old>, std::tuple_element_t<I, New>>) && ...);
    }
    template <class... O, class... N> struct extension<sort_list<O...>, sort_list<N...>>
      : std::bool_constant<list_extends<std::tuple<O...>, std::tuple<N...>>(std::index_sequence_for<O...>{})> {};
    template <class Old, class New> struct extends
      : std::bool_constant<info<Old>::count == 0 || extension<Old, New>::value> {};

    template <class Leaves, class Reader, class Visitor> struct result;
    template <class Reader, class Visitor> struct result<sorts<>, Reader, Visitor> { using type = void; };
    template <class S, class... Rest, class Reader, class Visitor>
    struct result<sorts<S, Rest...>, Reader, Visitor> {
      using type = std::invoke_result_t<Visitor, std::type_identity<S>, Reader &>;
      static_assert((std::is_same_v<type,
        std::invoke_result_t<Visitor, std::type_identity<Rest>, Reader &>> && ...),
        "sort visitor branches must have the same return type");
    };
    inline std::uint64_t discriminator(auto & reader, unsigned width) {
      auto code = static_cast<std::uint64_t>(reader.read_bits(width));
      if (code >= (std::uint64_t{1} << width))
        error_detail::raise<std::invalid_argument>("sort discriminator exceeds its width");
      return code;
    }
    template <class S, class Result, class Reader, class Visitor>
    Result visit(Reader & reader, Visitor && visitor) {
      if constexpr (hole<S>) error_detail::raise<std::invalid_argument>("undefined sort code");
      else return std::invoke(std::forward<Visitor>(visitor), std::type_identity<S>{}, reader);
    }
    template <class R> struct dispatch;
    template <> struct dispatch<sort_undefined> {
      template <class Result, class Reader, class Visitor>
      static Result run(Reader &, Visitor &&) {
        error_detail::raise<std::invalid_argument>("empty sort registry");
      }
    };
    template <class S> struct dispatch<tip<S>> {
      template <class Result, class Reader, class Visitor>
      static Result run(Reader & reader, Visitor && visitor) {
        return visit<S, Result>(reader, std::forward<Visitor>(visitor));
      }
    };
    template <class T> struct dispatch<unsorted<T>> : dispatch<tip<unsorted<T>>> {};
    template <class L, class R> struct dispatch<bin<L, R>> {
      template <class Result, class Reader, class Visitor>
      static Result run(Reader & reader, Visitor && visitor) {
        if (discriminator(reader, 1))
          return dispatch<R>::template run<Result>(reader, std::forward<Visitor>(visitor));
        return dispatch<L>::template run<Result>(reader, std::forward<Visitor>(visitor));
      }
    };
    template <class... S> struct dispatch<sort_list<S...>> {
      template <std::size_t I, class Result, class Reader, class Visitor>
      static Result at(std::uint64_t code, Reader & reader, Visitor && visitor) {
        if constexpr (I == sizeof...(S))
          error_detail::raise<std::invalid_argument>("undefined sort code");
        else {
          if (code == I) return visit<std::tuple_element_t<I, std::tuple<S...>>, Result>(reader,
                                                                                      std::forward<Visitor>(visitor));
          return at<I + 1, Result>(code, reader, std::forward<Visitor>(visitor));
        }
      }
      template <class Result, class Reader, class Visitor>
      static Result run(Reader & reader, Visitor && visitor) {
        return at<0, Result>(discriminator(reader, 8), reader, std::forward<Visitor>(visitor));
      }
    };
  }

  // A codec's unit promises payload alignment, not alignment of its address:
  // a byte codec nested under bin may start at any bit offset. Its reader and
  // writer must support that offset. Empty registries have no common width.
  template <class R> struct registry_traits {
    using registry_type = R;
    static_assert(registry_detail::unique<typename registry_detail::info<R>::leaves>::value,
                  "each sort type must have exactly one registry code");
    static constexpr std::size_t sort_count = registry_detail::info<R>::count;
    static constexpr profile_unit unit = registry_detail::info<R>::mode.value_or(profile_unit::byte);
    static constexpr auto fixed_value_bits = registry_detail::info<R>::width;
    static constexpr auto value_width = fixed_value_bits ?
      std::optional<std::uint64_t>(*fixed_value_bits >> (unit == profile_unit::byte ? 3 : 0)) : std::nullopt;
    static constexpr bool fixed_width = value_width.has_value();
  };

  // Old -> New preserves all previously occupied codes and leaf sort types.
  // This is code/interpretation compatibility, not a persisted-header check:
  // extending a registry can change its inferred common value width.
  template <class Old, class New> struct registry_extends
    : std::bool_constant<(sizeof(registry_traits<Old>) != 0) && (sizeof(registry_traits<New>) != 0) &&
                         registry_detail::extends<Old, New>::value> {};
  template <class Old, class New>
  inline constexpr bool registry_extends_v = registry_extends<Old, New>::value;

  // Reader supplies read_bits(unsigned) -> uint64_t and throws on truncation.
  // Only the sort discriminator is consumed. The visitor receives the chosen
  // type and the same reader at its key payload, with no implicit alignment.
  // Unknown codes throw invalid_argument; errors do not roll back consumed
  // discriminator bits. Visitor branches must have one exact return type.
  template <class R, class Reader, class Visitor>
  decltype(auto) dispatch_sort(Reader & reader, Visitor && visitor) {
    (void)sizeof(registry_traits<R>);
    using result = typename registry_detail::result<typename registry_detail::info<R>::leaves,
                                                    Reader, Visitor &&>::type;
    return registry_detail::dispatch<R>::template run<result>(reader, std::forward<Visitor>(visitor));
  }
}
