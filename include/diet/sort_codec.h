/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Encodes typed bit records with inherited sort prefixes and sort-owned grammars.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#pragma once

#include <diet/profile.h>
#include <diet/registry.h>

#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace diet {
  namespace sort_codec_detail {
    template <class Code> struct count_policy {
      static constexpr auto unit = profile_unit::bit;
      static constexpr auto backspace_code = policy_detail::backspace_traits<Code>::code;
      static constexpr auto backspace_parameter = policy_detail::backspace_traits<Code>::parameter;
    };
    inline bit_view string_bits(std::string const & value) {
      return {std::as_bytes(std::span(value.data(), value.size())),
              profile_detail::multiply(value.size(), 8)};
    }
  }

  // These streams have bit addresses even when an individual codec writes bytes.
  struct sort_bit_writer {
    explicit sort_bit_writer(bit_string & data) : data_(data) { data.validate(); }
    std::uint64_t position() const noexcept { return data_.bit_size; }
    void write_bits(std::uint64_t value, unsigned width) {
      if (width > 64 || (width < 64 && (value >> width)))
        error_detail::raise<std::invalid_argument>("value exceeds bit field");
      auto at = position();
      profile_detail::resize(data_, profile_detail::add(at, width));
      profile_detail::put_fixed(data_, at, value, width);
    }
    void append(bit_view bits) { profile_detail::append(data_, bits); }
    template <class Code = exponential_golomb<0>> void write_count(std::uint64_t value) {
      profile_detail::write_backspace<sort_codec_detail::count_policy<Code>>(data_, value);
    }
  private:
    bit_string & data_;
  };

  struct sort_bit_reader {
    explicit sort_bit_reader(bit_view data) : data_(data) {}
    std::uint64_t position() const noexcept { return at_; }
    std::uint64_t remaining() const noexcept { return data_.size() - at_; }
    bool empty() const noexcept { return remaining() == 0; }
    std::uint64_t read_bits(unsigned width) { return profile_detail::read_fixed(data_, at_, width); }
    bit_view take_bits(std::uint64_t count) {
      if (count > remaining()) error_detail::raise<std::invalid_argument>("truncated sort payload");
      auto result = data_.subview(at_, count);
      at_ += count;
      return result;
    }
    template <class Code = exponential_golomb<0>> std::uint64_t read_count() {
      return profile_detail::read_backspace<sort_codec_detail::count_policy<Code>>(data_, at_);
    }
  private:
    bit_view data_;
    std::uint64_t at_ = 0;
  };

  // A frame borrows literal bits and keeps the inherited prefix implicit.
  struct fc_key_frame {
    std::uint64_t retained_bits = 0;
    bit_view literal;
    std::uint64_t size() const { return profile_detail::add(retained_bits, literal.size()); }
    bit_comparison compare(bit_view query, bit_comparison previous) const {
      if (previous.common_bits < retained_bits) return previous;
      if (retained_bits > query.size())
        error_detail::raise<std::invalid_argument>("FC comparison lacks inherited prefix");
      auto result = compare_common_bits(literal, query.subview(retained_bits, query.size() - retained_bits));
      result.common_bits += retained_bits;
      return result;
    }
  };

  // Strings compare as unsigned bytes. Their ordered key representation escapes
  // zero as 00 ff and terminates with 00 00; it is distinct from FC wire framing.
  struct ordered_string_key {
    using value_type = std::string;
    static bool less(value_type const & a, value_type const & b) {
      return compare_bits(sort_codec_detail::string_bits(a), sort_codec_detail::string_bits(b)) < 0;
    }
    static void write_ordered(sort_bit_writer & out, value_type const & value) {
      for (unsigned char c : value) {
        out.write_bits(c, 8);
        if (!c) out.write_bits(255, 8);
      }
      out.write_bits(0, 16);
    }
    static value_type read_ordered(sort_bit_reader & in) {
      value_type result;
      for (;;) {
        auto c = in.read_bits(8);
        if (!c) {
          auto escape = in.read_bits(8);
          if (!escape) return result;
          if (escape != 255) error_detail::raise<std::invalid_argument>("noncanonical string escape");
        }
        result.push_back(static_cast<char>(c));
      }
    }
  };

  template <class CountCode = exponential_golomb<0>> struct fc_string_key : ordered_string_key {
    static fc_key_frame read_frame(sort_bit_reader & in, std::uint64_t previous_bits) {
      auto backspace = in.template read_count<CountCode>();
      if (backspace > previous_bits) error_detail::raise<std::invalid_argument>("FC backspace exceeds key");
      auto retained = previous_bits - backspace;
      auto literal = in.take_bits(in.template read_count<CountCode>());
      if ((profile_detail::add(retained, literal.size()) & 7) != 0)
        error_detail::raise<std::invalid_argument>("FC string ends inside a byte");
      return {retained, literal};
    }
    static void write(sort_bit_writer & out, value_type const & value, value_type const * previous = nullptr) {
      auto key = sort_codec_detail::string_bits(value);
      auto old = previous ? sort_codec_detail::string_bits(*previous) : bit_view{};
      auto retained = compare_common_bits(old, key).common_bits;
      out.template write_count<CountCode>(old.size() - retained);
      out.template write_count<CountCode>(key.size() - retained);
      out.append(key.subview(retained, key.size() - retained));
    }
    static value_type read(sort_bit_reader & in, value_type const * previous = nullptr) {
      auto old = previous ? sort_codec_detail::string_bits(*previous) : bit_view{};
      auto frame = read_frame(in, old.size());
      value_type result(static_cast<std::size_t>(frame.size() >> 3), '\0');
      auto target = reinterpret_cast<std::byte *>(result.data());
      profile_detail::copy_bits(target, 0, old.prefix(frame.retained_bits));
      profile_detail::copy_bits(target, frame.retained_bits, frame.literal);
      return result;
    }
  };

  template <class CountCode = exponential_golomb<0>> struct fc_bit_key {
    using value_type = bit_string;
    static bool less(value_type const & a, value_type const & b) { return compare_bits(a.view(), b.view()) < 0; }
    static fc_key_frame read_frame(sort_bit_reader & in, std::uint64_t previous_bits) {
      auto backspace = in.template read_count<CountCode>();
      if (backspace > previous_bits) error_detail::raise<std::invalid_argument>("FC backspace exceeds key");
      auto retained = previous_bits - backspace;
      auto literal = in.take_bits(in.template read_count<CountCode>());
      (void)profile_detail::add(retained, literal.size());
      return {retained, literal};
    }
    static void write(sort_bit_writer & out, value_type const & value, value_type const * previous = nullptr) {
      auto key = value.view(), old = previous ? previous->view() : bit_view{};
      auto retained = compare_common_bits(old, key).common_bits;
      out.template write_count<CountCode>(old.size() - retained);
      out.template write_count<CountCode>(key.size() - retained);
      out.append(key.subview(retained, key.size() - retained));
    }
    static value_type read(sort_bit_reader & in, value_type const * previous = nullptr) {
      auto old = previous ? previous->view() : bit_view{};
      auto frame = read_frame(in, old.size());
      auto result = bit_string::copy(old.prefix(frame.retained_bits));
      profile_detail::append(result, frame.literal);
      return result;
    }
    // Canonical bit keys use 1b per input bit, followed by a zero terminator.
    static void write_ordered(sort_bit_writer & out, value_type const & value) {
      auto bits = value.view();
      for (std::uint64_t i = 0; i != bits.size(); ++i) out.write_bits(2 | bits.at(i), 2);
      out.write_bits(0, 1);
    }
    static value_type read_ordered(sort_bit_reader & in) {
      value_type result;
      sort_bit_writer out(result);
      while (in.read_bits(1)) out.write_bits(in.read_bits(1), 1);
      return result;
    }
  };

  template <class CountCode = exponential_golomb<0>> struct string_value {
    using value_type = std::string;
    static constexpr std::optional<std::uint64_t> fixed_value_bits = std::nullopt;
    static void write(sort_bit_writer & out, value_type const & value) {
      out.template write_count<CountCode>(value.size());
      out.append(sort_codec_detail::string_bits(value));
    }
    static value_type read(sort_bit_reader & in) {
      auto bytes = in.template read_count<CountCode>();
      if (bytes > (in.remaining() >> 3)) error_detail::raise<std::invalid_argument>("truncated string value");
      auto bits = in.take_bits(bytes << 3);
      value_type result(static_cast<std::size_t>(bytes), '\0');
      profile_detail::copy_bits(reinterpret_cast<std::byte *>(result.data()), 0, bits);
      return result;
    }
    static void skip(sort_bit_reader & in) {
      auto bytes = in.template read_count<CountCode>();
      if (bytes > (in.remaining() >> 3)) error_detail::raise<std::invalid_argument>("truncated string value");
      (void)in.take_bits(bytes << 3);
    }
  };

  template <class CountCode = exponential_golomb<0>> struct bit_value {
    using value_type = bit_string;
    static constexpr std::optional<std::uint64_t> fixed_value_bits = std::nullopt;
    static void write(sort_bit_writer & out, value_type const & value) {
      auto bits = value.view();
      out.template write_count<CountCode>(bits.size());
      out.append(bits);
    }
    static value_type read(sort_bit_reader & in) { return bit_string::copy(in.take_bits(in.template read_count<CountCode>())); }
    static void skip(sort_bit_reader & in) { (void)in.take_bits(in.template read_count<CountCode>()); }
  };

  template <class CountCode = exponential_golomb<0>> struct raw_string_key : ordered_string_key {
    static void write(sort_bit_writer & out, value_type const & value, value_type const * = nullptr) {
      string_value<CountCode>::write(out, value);
    }
    static value_type read(sort_bit_reader & in, value_type const * = nullptr) {
      return string_value<CountCode>::read(in);
    }
  };

  template <unsigned Bits> struct unsigned_value {
    static_assert(Bits >= 1 && Bits <= 64);
    using value_type = std::uint64_t;
    static constexpr std::optional<std::uint64_t> fixed_value_bits = Bits;
    static void write(sort_bit_writer & out, value_type value) { out.write_bits(value, Bits); }
    static value_type read(sort_bit_reader & in) { return in.read_bits(Bits); }
    static void skip(sort_bit_reader & in) { (void)in.take_bits(Bits); }
  };

  template <unsigned Bits> struct unsigned_key {
    using value_type = std::uint64_t;
    static bool less(value_type a, value_type b) noexcept { return a < b; }
    static void write(sort_bit_writer & out, value_type value, value_type const * = nullptr) {
      unsigned_value<Bits>::write(out, value);
    }
    static value_type read(sort_bit_reader & in, value_type const * = nullptr) {
      return unsigned_value<Bits>::read(in);
    }
    static void write_ordered(sort_bit_writer & out, value_type value) { write(out, value); }
    static value_type read_ordered(sort_bit_reader & in) { return read(in); }
  };

  // The absent case is one bit, regardless of the present codec's width.
  template <class Codec> struct tombstone_value {
    using value_type = std::optional<typename Codec::value_type>;
    static constexpr std::optional<std::uint64_t> fixed_value_bits =
      Codec::fixed_value_bits == 0 ? std::optional<std::uint64_t>{1} : std::nullopt;
    static void write(sort_bit_writer & out, value_type const & value) {
      out.write_bits(value.has_value(), 1);
      if (value) Codec::write(out, *value);
    }
    static value_type read(sort_bit_reader & in) {
      if (!in.read_bits(1)) return std::nullopt;
      return Codec::read(in);
    }
    static void skip(sort_bit_reader & in) { if (in.read_bits(1)) Codec::skip(in); }
  };

  template <class Codec, auto Sentinel> struct niche_value {
    using value_type = std::optional<typename Codec::value_type>;
    static constexpr auto fixed_value_bits = Codec::fixed_value_bits;
    static void write(sort_bit_writer & out, value_type const & value) {
      if (value && *value == Sentinel) error_detail::raise<std::invalid_argument>("value occupies tombstone niche");
      Codec::write(out, value ? *value : Sentinel);
    }
    static value_type read(sort_bit_reader & in) {
      auto value = Codec::read(in);
      if (value == Sentinel) return std::nullopt;
      return value;
    }
    static void skip(sort_bit_reader & in) { Codec::skip(in); }
  };

  struct no_value {
    using value_type = std::monostate;
    static constexpr std::optional<std::uint64_t> fixed_value_bits = 0;
    static void write(sort_bit_writer &, value_type) noexcept {}
    static value_type read(sort_bit_reader &) noexcept { return {}; }
    static void skip(sort_bit_reader &) noexcept {}
  };

  // The sort may specialize this trait rather than declaring codec aliases.
  template <class S> struct sort_codec {
    using key_codec = typename S::key_codec;
    using value_codec = typename S::value_codec;
  };

  template <> struct sort_codec<unsorted<std::optional<std::string>>> {
    using key_codec = fc_string_key<>;
    using value_codec = tombstone_value<string_value<>>;
  };
  template <> struct sort_codec<unsorted<std::string>> {
    using key_codec = fc_string_key<>;
    using value_codec = string_value<>;
  };

  namespace sort_codec_detail {
    template <class S> constexpr void validate_value_width() {
      static_assert(!S::encoding::fixed_value_bits ||
        S::encoding::fixed_value_bits == sort_codec<S>::value_codec::fixed_value_bits,
        "sort encoding promises a value width its codec does not supply");
    }
    template <class S, class Leaves> struct contains;
    template <class S, class... T> struct contains<S, registry_detail::sorts<T...>>
      : std::bool_constant<(std::is_same_v<S, T> || ...)> {};
    template <class R, class S> inline constexpr bool contains_sort =
      contains<S, typename registry_detail::info<R>::leaves>::value;
    template <class R, class S> struct code;
    template <class S> struct code<tip<S>, S> {
      static constexpr std::uint64_t size = 0;
      static void write(sort_bit_writer &) {}
    };
    template <class T> struct code<unsorted<T>, unsorted<T>> : code<tip<unsorted<T>>, unsorted<T>> {};
    template <class L, class R, class S> struct code<bin<L, R>, S> {
      static constexpr std::uint64_t size = 1 + [] {
        if constexpr (contains_sort<L, S>) return code<L, S>::size;
        else return code<R, S>::size;
      }();
      static void write(sort_bit_writer & out) {
        constexpr bool right = !contains_sort<L, S>;
        out.write_bits(right, 1);
        if constexpr (right) code<R, S>::write(out);
        else code<L, S>::write(out);
      }
    };
    template <class... T, class S> struct code<sort_list<T...>, S> {
      static constexpr std::uint64_t size = 8;
      static void write(sort_bit_writer & out) {
        std::uint64_t i = 0, index = 0;
        ((std::is_same_v<T, S> ? (index = i, ++i) : ++i), ...);
        out.write_bits(index, 8);
      }
    };
    template <class S> struct key_state {
      using sort_type = S;
      using key_type = typename sort_codec<S>::key_codec::value_type;
      key_type key;
    };
    template <class Leaves> struct states;
    template <class... S> struct states<registry_detail::sorts<S...>> {
      using type = std::variant<std::monostate, key_state<S>...>;
    };
    // Replay retained discriminator bits, then consume exactly enough new bits
    // to reach a leaf. A leaf may not terminate inside the retained path.
    struct prefix_reader {
      sort_bit_reader & input;
      bit_view retained;
      sort_bit_writer & path;
      std::uint64_t at = 0;
      std::uint64_t read_bits(unsigned width) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i != width; ++i) {
          auto bit = at < retained.size() ? profile_detail::load_bits(retained, at++, 1) : input.read_bits(1);
          path.write_bits(bit, 1);
          value = (value << 1) | bit;
        }
        return value;
      }
    };
  }

  template <class Registry, class S> void write_sort_code(sort_bit_writer & out) {
    (void)sizeof(registry_traits<Registry>);
    static_assert(sort_codec_detail::contains_sort<Registry, S>, "sort is absent from registry");
    sort_codec_detail::code<Registry, S>::write(out);
  }
  template <class Registry, class S> bit_string sort_code() {
    bit_string result;
    sort_bit_writer out(result);
    write_sort_code<Registry, S>(out);
    return result;
  }

  struct sort_record_control {
    std::uint64_t start = 0;
    std::uint64_t sort_retained = 0;
    std::uint64_t sort_bits = 0;
    std::uint64_t key_start = 0;
    std::uint64_t value_start = 0;
    std::uint64_t end = 0;
  };

  // A checkpoint contains the previous discriminator and typed key. It is an
  // in-memory decoding anchor, not a serialized schema or an index restart.
  template <class Registry> struct sort_record_state {
    bit_string path;
    typename sort_codec_detail::states<typename registry_detail::info<Registry>::leaves>::type previous;
    void validate() const {
      path.validate();
      std::visit([&](auto const & value) {
        using state = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<state, std::monostate>) {
          if (path.bit_size) error_detail::raise<std::invalid_argument>("sort anchor has no key");
        } else if (path != sort_code<Registry, typename state::sort_type>())
          error_detail::raise<std::invalid_argument>("sort anchor path disagrees with key type");
      }, previous);
    }
  };

  template <class Registry, class TreeCode = exponential_golomb<0>, stream_role Role = stream_role::native>
  struct sort_record_writer {
    static_assert(registry_traits<Registry>::unit == profile_unit::bit,
                  "sort record streams require a bit registry");
    using state_type = sort_record_state<Registry>;
    template <class S> using value_codec = std::conditional_t<Role == stream_role::native,
      typename sort_codec<S>::value_codec, no_value>;
    sort_record_writer() = default;
    explicit sort_record_writer(state_type state) : state_(std::move(state)) { state_.validate(); }

    template <class S> sort_record_control append(
        typename sort_codec<S>::key_codec::value_type const & key,
        typename value_codec<S>::value_type const & value) {
      if (failed_) error_detail::raise<std::logic_error>("failed sort record writer");
      try {
        sort_codec_detail::validate_value_width<S>();
        using key_codec = typename sort_codec<S>::key_codec;
        auto previous = std::get_if<sort_codec_detail::key_state<S>>(&state_.previous);
        bool same_sort = previous != nullptr;
        auto path = same_sort ? bit_string{} : sort_code<Registry, S>();
        auto path_bits = same_sort ? state_.path.view() : path.view();
        auto comparison = same_sort ? bit_comparison{state_.path.bit_size, 0} :
          compare_common_bits(state_.path.view(), path_bits);
        if ((!std::holds_alternative<std::monostate>(state_.previous) && comparison.order > 0) ||
            (previous && !key_codec::less(previous->key, key)))
          error_detail::raise<std::invalid_argument>("sort records must have unique sorted keys");
        profile_detail::resize(frame_, 0);
        sort_bit_writer out(frame_);
        sort_record_control control{data_.bit_size, comparison.common_bits, path_bits.size()};
        out.template write_count<TreeCode>(state_.path.bit_size - control.sort_retained);
        out.append(path_bits.subview(control.sort_retained, path_bits.size() - control.sort_retained));
        control.key_start = control.start + out.position();
        key_codec::write(out, key, previous ? &previous->key : nullptr);
        control.value_start = control.start + out.position();
        value_codec<S>::write(out, value);
        control.end = control.start + out.position();
        // All state construction precedes publication of these bytes.
        sort_codec_detail::key_state<S> next{key};
        profile_detail::append(data_, frame_.view());
        state_.previous = std::move(next);
        if (!same_sort) state_.path = std::move(path);
        return control;
      } catch (...) { failed_ = true; throw; }
    }
    template <class S> sort_record_control append(typename sort_codec<S>::key_codec::value_type const & key)
      requires std::is_same_v<value_codec<S>, no_value> { return append<S>(key, {}); }
    bit_string const & data() const & {
      if (failed_) error_detail::raise<std::logic_error>("failed sort record writer");
      return data_;
    }
    bit_string const & data() const && = delete;
    state_type const & state() const & noexcept { return state_; }
    bool failed() const noexcept { return failed_; }
  private:
    bit_string data_;
    bit_string frame_;
    state_type state_;
    bool failed_ = false;
  };

  template <class Registry, class TreeCode = exponential_golomb<0>, stream_role Role = stream_role::native>
  struct sort_record_reader {
    static_assert(registry_traits<Registry>::unit == profile_unit::bit,
                  "sort record streams require a bit registry");
    using state_type = sort_record_state<Registry>;
    explicit sort_record_reader(bit_view data, state_type state = {}) : input_(data), state_(std::move(state)) {
      state_.validate();
    }
    bool empty() const noexcept { return input_.empty(); }
    bool failed() const noexcept { return failed_; }
    std::uint64_t position() const noexcept { return input_.position(); }
    state_type const & state() const & noexcept { return state_; }

    // Callback receives ephemeral references. Its exception consumes this record
    // and poisons the reader; copied anchors can resume on the remaining slice.
    template <class Visitor> bool next(Visitor && visitor) {
      if (failed_) error_detail::raise<std::logic_error>("failed sort record reader");
      if (empty()) return false;
      try {
        sort_record_control control;
        control.start = input_.position();
        auto backspace = input_.template read_count<TreeCode>();
        if (backspace > state_.path.bit_size)
          error_detail::raise<std::invalid_argument>("sort backspace exceeds path");
        control.sort_retained = state_.path.bit_size - backspace;
        bit_string path;
        bool reuse = backspace == 0 && !std::holds_alternative<std::monostate>(state_.previous);
        auto decode = [&]<class S>(std::type_identity<S> tag) {
          sort_codec_detail::validate_value_width<S>();
          auto path_bits = reuse ? state_.path.view() : path.view();
          control.sort_bits = path_bits.size();
          control.key_start = input_.position();
          using key_codec = typename sort_codec<S>::key_codec;
          using value_codec = std::conditional_t<Role == stream_role::native,
            typename sort_codec<S>::value_codec, no_value>;
          auto previous = std::get_if<sort_codec_detail::key_state<S>>(&state_.previous);
          auto key = key_codec::read(input_, previous ? &previous->key : nullptr);
          control.value_start = input_.position();
          auto value = value_codec::read(input_);
          control.end = input_.position();
          if ((!std::holds_alternative<std::monostate>(state_.previous) &&
               !reuse && compare_bits(state_.path.view(), path_bits) > 0) ||
              (previous && !key_codec::less(previous->key, key)))
            error_detail::raise<std::invalid_argument>("sort records must have unique sorted keys");
          state_.previous = sort_codec_detail::key_state<S>{std::move(key)};
          if (!reuse) state_.path = std::move(path);
          auto const & stored = std::get<sort_codec_detail::key_state<S>>(state_.previous);
          std::invoke(std::forward<Visitor>(visitor), tag, stored.key, value, control);
        };
        if (reuse) {
          // Retaining the complete code already identifies the leaf. Do not
          // walk a deep tree or copy its path again for every key of that sort.
          std::visit([&](auto const & previous) {
            using state = std::remove_cvref_t<decltype(previous)>;
            if constexpr (!std::is_same_v<state, std::monostate>)
              decode(std::type_identity<typename state::sort_type>{});
          }, state_.previous);
        } else {
          sort_bit_writer path_writer(path);
          sort_codec_detail::prefix_reader prefix{input_, state_.path.view().prefix(control.sort_retained), path_writer};
          dispatch_sort<Registry>(prefix, [&]<class S>(std::type_identity<S> tag, auto &) {
            if (prefix.at != control.sort_retained)
              error_detail::raise<std::invalid_argument>("retained sort prefix crosses leaf");
            decode(tag);
          });
        }
        return true;
      } catch (...) { failed_ = true; throw; }
    }
  private:
    sort_bit_reader input_;
    state_type state_;
    bool failed_ = false;
  };
}
