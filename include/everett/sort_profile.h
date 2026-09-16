/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Navigates sort-owned bit records using shared sort seeds and sparse offsets.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once

#include <everett/sort_codec.h>
#include <everett/cola_index.h>

namespace everett {
  // Selection is a protocol: the declarative registry is one implementation.
  // A custom selector may use a generated table rather than a recursive tree.
  template <class Registry> struct registry_selector {
    template <class S> static constexpr std::uint64_t code_size = sort_codec_detail::code<Registry, S>::size;
    template <class Input, class Visitor> static decltype(auto) select(Input & in, Visitor && visitor) {
      return dispatch_sort<Registry>(in, std::forward<Visitor>(visitor));
    }
    template <class S, class Output> static void write(Output & out) { write_sort_code<Registry, S>(out); }
  };

  // A navigable key codec supplies an order-bit representation and frames that
  // expose inherited prefix length plus literal spans. Values keep their own
  // complete grammar; value_codec::skip must find their end without allocation.
  // Optional order_view borrows the caller's key for synchronous query encoding.
  template <class Codec> struct sort_profile_key;
  template <class C> struct sort_profile_key<fc_string_key<C>> {
    static constexpr bool front_coded = true;
    static bit_view order_view(std::string const & key) { return sort_codec_detail::string_bits(key); }
    static bit_string order(std::string const & key) { return bit_string::copy(sort_codec_detail::string_bits(key)); }
    static std::string decode_order(bit_view key) {
      if (key.size() & 7) throw std::invalid_argument("string order key ends inside byte");
      std::string result(static_cast<std::size_t>(key.size() >> 3), '\0');
      profile_detail::copy_bits(reinterpret_cast<std::byte *>(result.data()), 0, key);
      return result;
    }
    static fc_key_frame read(sort_bit_reader & in, std::uint64_t retained) {
      auto literal = in.take_bits(in.template read_count<C>());
      if ((retained + literal.size()) & 7) throw std::invalid_argument("string key ends inside byte");
      return {retained, literal};
    }
    template <class Output> static void header(Output & out, std::uint64_t retained, std::uint64_t suffix_bits) {
      if ((retained + suffix_bits) & 7) throw std::invalid_argument("string key ends inside byte");
      out.template write_count<C>(suffix_bits);
    }
    template <class Output> static void write(Output & out, std::uint64_t retained, bit_view suffix) {
      header(out, retained, suffix.size()); out.append(suffix);
    }
  };
  template <class C> struct sort_profile_key<fc_bit_key<C>> {
    static constexpr bool front_coded = true;
    static bit_view order_view(bit_string const & key) { return key.view(); }
    static bit_string order(bit_string const & key) { return key; }
    static bit_string decode_order(bit_view key) { return bit_string::copy(key); }
    static fc_key_frame read(sort_bit_reader & in, std::uint64_t retained) {
      return {retained, in.take_bits(in.template read_count<C>())};
    }
    template <class Output> static void header(Output & out, std::uint64_t, std::uint64_t suffix_bits) {
      out.template write_count<C>(suffix_bits);
    }
    template <class Output> static void write(Output & out, std::uint64_t retained, bit_view suffix) {
      header(out, retained, suffix.size()); out.append(suffix);
    }
  };
  template <unsigned N> struct sort_profile_key<unsigned_key<N>> {
    static constexpr bool front_coded = false;
    static bit_string order(std::uint64_t key) {
      bit_string result; sort_bit_writer out(result); out.write_bits(key, N); return result;
    }
    static std::uint64_t decode_order(bit_view key) {
      if (key.size() != N) throw std::invalid_argument("integer order key width");
      sort_bit_reader input(key); return input.read_bits(N);
    }
    static fc_key_frame read(sort_bit_reader & in, std::uint64_t retained) {
      if (retained) throw std::invalid_argument("raw integer has an inherited prefix");
      return {0, in.take_bits(N)};
    }
    template <class Output> static void header(Output &, std::uint64_t retained, std::uint64_t suffix_bits) {
      if (retained || suffix_bits != N) throw std::invalid_argument("raw integer frame width");
    }
    template <class Output> static void write(Output & out, std::uint64_t retained, bit_view suffix) {
      header(out, retained, suffix.size()); out.append(suffix);
    }
  };
  template <class C> struct sort_profile_key<raw_string_key<C>> {
    static constexpr bool front_coded = false;
    static bit_view order_view(std::string const & key) { return sort_codec_detail::string_bits(key); }
    static bit_string order(std::string const & key) { return bit_string::copy(sort_codec_detail::string_bits(key)); }
    static std::string decode_order(bit_view key) {
      if (key.size() & 7) throw std::invalid_argument("string order key ends inside byte");
      std::string result(static_cast<std::size_t>(key.size() >> 3), '\0');
      profile_detail::copy_bits(reinterpret_cast<std::byte *>(result.data()), 0, key);
      return result;
    }
    static fc_key_frame read(sort_bit_reader & in, std::uint64_t retained) {
      if (retained) throw std::invalid_argument("raw string has an inherited prefix");
      return {0, in.take_bits(profile_detail::multiply(in.template read_count<C>(), 8))};
    }
    template <class Output> static void header(Output & out, std::uint64_t retained, std::uint64_t suffix_bits) {
      if (retained || (suffix_bits & 7)) throw std::invalid_argument("raw string frame width");
      out.template write_count<C>(suffix_bits >> 3);
    }
    template <class Output> static void write(Output & out, std::uint64_t retained, bit_view suffix) {
      header(out, retained, suffix.size()); out.append(suffix);
    }
  };

  namespace sort_profile_detail {
    inline bit_comparison compare_spans(std::span<bit_view const> a, std::span<bit_view const> b,
                                       std::uint64_t first_a = 0, std::uint64_t first_b = 0) {
      std::size_t i = 0, j = 0;
      while (i != a.size() && first_a >= a[i].size()) first_a -= a[i++].size();
      while (j != b.size() && first_b >= b[j].size()) first_b -= b[j++].size();
      std::uint64_t common = 0;
      while (i != a.size() && j != b.size()) {
        auto width = std::min(a[i].size() - first_a, b[j].size() - first_b);
        auto cmp = compare_common_bits(a[i].subview(first_a, width), b[j].subview(first_b, width));
        common += cmp.common_bits;
        if (cmp.order) return {common, cmp.order};
        first_a += width; first_b += width;
        if (first_a == a[i].size()) { ++i; first_a = 0; }
        if (first_b == b[j].size()) { ++j; first_b = 0; }
      }
      while (i != a.size() && !a[i].size()) ++i;
      while (j != b.size() && !b[j].size()) ++j;
      return {common, i == a.size() ? j == b.size() ? 0 : -1 : 1};
    }
    template <class List> struct visit;
    template <class S, class... Rest> struct visit<registry_detail::sorts<S, Rest...>> {
      template <class F> static decltype(auto) at(std::size_t index, F && f) {
        if (!index) return f(std::type_identity<S>{});
        if constexpr (sizeof...(Rest)) return visit<registry_detail::sorts<Rest...>>::at(index - 1, std::forward<F>(f));
        else throw std::invalid_argument("sort profile leaf index");
      }
    };
    template <class List, class S> struct ordinal;
    template <class S, class... Rest> struct ordinal<registry_detail::sorts<S, Rest...>, S>
      : std::integral_constant<std::size_t, 0> {};
    template <class T, class... Rest, class S> struct ordinal<registry_detail::sorts<T, Rest...>, S>
      : std::integral_constant<std::size_t, 1 + ordinal<registry_detail::sorts<Rest...>, S>::value> {};
    inline bit_string concatenate(bit_view a, bit_view b) {
      auto result = bit_string::copy(a); profile_detail::append(result, b); return result;
    }
  }

  template <class P, class Selector = registry_selector<typename P::registry_type>> struct sort_profile_array;
  template <class P, class Selector = registry_selector<typename P::registry_type>> struct sort_profile_view;
  template <class P, class Selector = registry_selector<typename P::registry_type>> struct sort_profile_cursor;
  template <class P, class Selector = registry_selector<typename P::registry_type>> struct sort_profile_writer;
  namespace sort_profile_detail { template <class P, class Selector> struct encoder; }

  struct sort_profile_payload {
    fc_key_frame key;
    std::uint64_t value_start = 0, end = 0;
  };
  struct sort_profile_frame {
    std::uint64_t ordinal = 0, retained = 0, key_units = 0, next_offset = 0;
    std::array<bit_view, 2> literal;
    bit_view value;
    bit_view path;
    std::size_t leaf = 0;
    bool front_coded = false;
    // Transient output hint, not a field in the encoded grammar.
    std::optional<std::uint64_t> retained_limit_bits{};
    sort_profile_payload (*parse)(sort_bit_reader &, std::uint64_t) = nullptr;
    std::uint64_t continuation() const noexcept { return front_coded ? key_units : path.size(); }
  };

  template <class P, class Selector> struct sort_profile_view {
    static_assert(P::unit == profile_unit::bit, "sort profiles use bit addresses");
    using policy_type = P;
    using leaves = typename registry_detail::info<typename P::registry_type>::leaves;
    using cursor_type = sort_profile_cursor<P, Selector>;
    // dictionary_offsets contains the start and EOF bit positions of prefix-free
    // selector codes. Seeds are packed IDs; one occupied sort takes zero bits.
    sort_profile_view(bit_view data, elias_fano_view offsets, profile_metadata metadata,
        bit_view dictionary, word_view dictionary_offsets, bit_view seeds)
      : data_(data), offsets_(offsets), metadata_(metadata), dictionary_(dictionary),
        dictionary_offsets_(dictionary_offsets), seeds_(seeds) {
      if (metadata.version != 3 || metadata.key_unit != profile_unit::bit || metadata.group_size != P::group_size ||
          metadata.codec_block_size != P::codec_block_size || metadata.extent != data.size() ||
          metadata.role != stream_role::native || dictionary_offsets_.size() == 0)
        throw std::invalid_argument("sort profile metadata mismatch");
      if ((!metadata.record_count && (metadata.extent || metadata.terminal_key_units)) ||
          (!metadata.policy_fixed_values && metadata.policy_value_width) ||
          (metadata.policy_fixed_values && metadata.common_value_width != metadata.policy_value_width) ||
          metadata.backspace_code != P::backspace_code || metadata.backspace_parameter != P::backspace_parameter ||
          metadata.value_unit != profile_unit::bit || metadata.count_unit != profile_unit::bit ||
          metadata.offset_unit != profile_unit::bit || metadata.count_code != profile_count_code::exp_golomb_zero ||
          metadata.bit_order != profile_bit_order::msb_first)
        throw std::invalid_argument("noncanonical sort profile metadata");
      auto m = dictionary_size();
      if (bool(size()) != bool(m) || offsets.size() != block_count() + 1 ||
          seeds.size() != profile_detail::multiply(block_count(), seed_width()) ||
          profile_detail::multiply(size(), metadata.common_value_width.value_or(0)) > metadata.extent ||
          offsets.universe() != metadata.extent - profile_detail::multiply(size(), metadata.common_value_width.value_or(0)))
        throw std::invalid_argument("sort profile section shape mismatch");
    }
    std::uint64_t size() const noexcept { return metadata_.record_count; }
    std::uint64_t block_count() const noexcept { return size() / P::codec_block_size + (size() % P::codec_block_size != 0); }
    std::uint64_t dictionary_size() const noexcept { return dictionary_offsets_.size() - 1; }
    unsigned seed_width() const noexcept { return dictionary_size() < 2 ? 0 : std::bit_width(dictionary_size() - 1); }
    std::uint64_t block_offset(std::uint64_t block) const {
      if (block > block_count()) throw std::out_of_range("sort profile block");
      auto ordinal = block == block_count() ? size() : block * P::codec_block_size;
      return offsets_.select(block) + ordinal * metadata_.common_value_width.value_or(0);
    }
    profile_metadata const & metadata() const noexcept { return metadata_; }
    bit_view data() const noexcept { return data_; }
    bit_view dictionary() const noexcept { return dictionary_; }
    word_view dictionary_offsets() const noexcept { return dictionary_offsets_; }
    bit_view seeds() const noexcept { return seeds_; }
    elias_fano_view group_offsets() const noexcept { return offsets_; }
    cursor_type cursor() const { return cursor_type(*this); }
    sort_profile_frame encoded_at(std::uint64_t ordinal) const {
      if (ordinal >= size()) throw std::out_of_range("sort profile ordinal");
      auto block = ordinal / P::codec_block_size;
      auto result = start_block(block);
      while (result.ordinal < ordinal) result = next(result);
      return result;
    }
    sort_profile_frame next(sort_profile_frame const & previous) const {
      auto ordinal = previous.ordinal + 1;
      if (ordinal >= size()) throw std::out_of_range("sort profile successor");
      if (!(ordinal % P::codec_block_size)) {
        if (block_offset(ordinal / P::codec_block_size) != previous.next_offset)
          throw std::invalid_argument("sort profile block offset mismatch");
        return start_block(ordinal / P::codec_block_size, &previous);
      }
      auto at = previous.next_offset;
      auto backspace = profile_detail::read_backspace<P>(data_, at);
      if (backspace > previous.continuation()) throw std::invalid_argument("sort profile backspace exceeds cursor");
      auto retained = previous.continuation() - backspace;
      sort_profile_frame result;
      result.ordinal = ordinal;
      result.leaf = previous.leaf;
      result.path = previous.path;
      result.parse = previous.parse; result.front_coded = previous.front_coded;
      bool same = retained >= previous.path.size();
      if (!same) {
        sort_bit_reader input(data_.subview(at, data_.size() - at));
        bit_string path; sort_bit_writer out(path);
        sort_codec_detail::prefix_reader prefix{input, previous.path.prefix(retained), out};
        Selector::select(prefix, [&]<class S>(std::type_identity<S>, auto &) {
          select<S>(result);
        });
        if (prefix.at != retained) throw std::invalid_argument("sort profile prefix crosses selector leaf");
        at += input.position();
        bool found = false;
        for (std::uint64_t i = 0; i != dictionary_size(); ++i) {
          auto first = dictionary_offsets_[i], last = dictionary_offsets_[i + 1];
          if (first > last || last > dictionary_.size()) throw std::invalid_argument("sort profile dictionary range");
          auto candidate = dictionary_.subview(first, last - first);
          if (!compare_bits(candidate, path.view())) { result.path = candidate; found = true; break; }
        }
        if (!found) throw std::invalid_argument("sort transition absent from dictionary");
      }
      return payload(at, retained, std::move(result), same);
    }
    template <class F> void compare_window(std::uint64_t first, std::uint64_t last,
        profile_query_context<P> context, F && callback, profile_comparison_work * work = nullptr) const {
      if (first > last || last > size() || last - first > P::group_size)
        throw std::out_of_range("sort profile query window");
      if (first == last) return;
      auto record = encoded_at(first);
      if (work) work->skipped_headers += first % P::codec_block_size;
      for (auto i = first; i != last; ++i) {
        auto count = context.advance_parts(record.retained, record.key_units, record.literal);
        if (work) { ++work->visited_headers; work->compared_bits += count; }
        if (!callback(profile_comparison_item<P>{i, context, record.value})) return;
        if (i + 1 != last) record = next(record);
      }
    }
    void scan() const {
      if (dictionary_offsets_[0] || dictionary_offsets_[dictionary_size()] != dictionary_.size() ||
          block_offset(0) || block_offset(block_count()) != data_.size())
        throw std::invalid_argument("sort profile section endpoints");
      if (!size()) return;
      auto c = cursor();
      while (!c.done()) {
        auto comparison = c.advance_comparison();
        if (comparison && comparison->order >= 0) throw std::invalid_argument("unsorted sort profile");
      }
    }
  private:
    bit_view data_;
    elias_fano_view offsets_;
    profile_metadata metadata_;
    bit_view dictionary_;
    word_view dictionary_offsets_;
    bit_view seeds_;
    sort_profile_frame start_block(std::uint64_t block, sort_profile_frame const * previous = nullptr) const {
      auto id = seed_width() ? profile_detail::load_bits(seeds_, block * seed_width(), seed_width()) : 0;
      if (id >= dictionary_size()) throw std::invalid_argument("sort profile seed ID");
      auto first = dictionary_offsets_[id], last = dictionary_offsets_[id + 1];
      if (first > last || last > dictionary_.size()) throw std::invalid_argument("sort profile dictionary range");
      auto path = dictionary_.subview(first, last - first);
      sort_bit_reader code(path);
      sort_profile_frame result;
      if (previous && previous->path.storage().data() == path.storage().data() &&
          previous->path.offset() == path.offset() && previous->path.size() == path.size()) {
        result.leaf = previous->leaf; result.parse = previous->parse; result.front_coded = previous->front_coded;
      } else {
        Selector::select(code, [&]<class S>(std::type_identity<S>, auto &) { select<S>(result); });
        if (!code.empty()) throw std::invalid_argument("trailing selector code bits");
      }
      result.path = path;
      auto at = block_offset(block);
      auto retained = profile_detail::read_count<P>(data_, at);
      result.ordinal = block * P::codec_block_size;
      return payload(at, retained, std::move(result), true, true);
    }
    template <class S> static void select(sort_profile_frame & frame) {
      frame.leaf = sort_profile_detail::ordinal<leaves, S>::value;
      frame.front_coded = sort_profile_key<typename sort_codec<S>::key_codec>::front_coded;
      frame.parse = [](sort_bit_reader & in, std::uint64_t retained) {
        auto key = sort_profile_key<typename sort_codec<S>::key_codec>::read(in, retained);
        auto start = in.position(); sort_codec<S>::value_codec::skip(in);
        return sort_profile_payload{key, start, in.position()};
      };
    }
    sort_profile_frame payload(std::uint64_t at, std::uint64_t retained, sort_profile_frame result, bool same, bool restart = false) const {
      sort_bit_reader in(data_.subview(at, data_.size() - at));
      auto local = same ? retained : result.path.size();
      if (local < result.path.size()) throw std::invalid_argument("sort profile retained position precedes leaf");
      // The selected handler survives every same-sort frame. Selector dispatch
      // occurs only on a sort transition or independently entered block.
      auto parsed = result.parse(in, local - result.path.size());
      auto frame = parsed.key;
      bool inherit = same && (frame.retained_bits || !restart);
      result.retained = inherit ? retained : same ? 0 : retained;
      result.literal = {inherit ? bit_view{} :
        result.path.subview(result.retained, result.path.size() - result.retained), frame.literal};
      result.key_units = profile_detail::add(result.path.size(), frame.size());
      result.value = data_.subview(at + parsed.value_start, parsed.end - parsed.value_start);
      if (metadata_.common_value_width && result.value.size() != *metadata_.common_value_width)
        throw std::invalid_argument("sort profile fixed value width mismatch");
      result.next_offset = at + parsed.end;
      return result;
    }
  };

  template <class P, class Selector> struct sort_profile_cursor {
    explicit sort_profile_cursor(sort_profile_view<P, Selector> view) : view_(view) {
      if (view.size()) { frame_ = view_.encoded_at(0); decode(); }
    }
    bool done() const noexcept { return ordinal_ == view_.size(); }
    profile_item<P> peek() const {
      if (done()) throw std::out_of_range("sort profile cursor end");
      return {ordinal_, profile_anchor<P>::complete(key_.view()), frame_.value};
    }
    std::uint64_t retained_bits() const {
      if (done()) throw std::out_of_range("sort profile cursor end");
      return frame_.retained;
    }
    void advance() { (void)advance_comparison(); }
    std::optional<bit_comparison> advance_comparison() {
      if (done()) throw std::out_of_range("sort profile cursor end");
      ++ordinal_;
      if (done()) {
        if (frame_.next_offset != view_.data().size() || frame_.key_units != view_.metadata().terminal_key_units)
          throw std::invalid_argument("sort profile terminal framing mismatch");
        return std::nullopt;
      }
      frame_ = view_.next(frame_);
      if (frame_.retained > key_.bit_size) throw std::invalid_argument("sort cursor missing inherited prefix");
      std::array<bit_view, 1> before{key_.view()};
      auto comparison = sort_profile_detail::compare_spans(before, frame_.literal, frame_.retained);
      comparison.common_bits += frame_.retained;
      profile_detail::resize(key_, frame_.retained);
      for (auto part : frame_.literal) profile_detail::append(key_, part);
      return comparison;
    }
  private:
    sort_profile_view<P, Selector> view_;
    sort_profile_frame frame_;
    bit_string key_;
    std::uint64_t ordinal_ = 0;
    void decode() {
      if (frame_.retained) throw std::invalid_argument("unseeded sort profile begins inside a key");
      for (auto part : frame_.literal) profile_detail::append(key_, part);
    }
  };

  template <class P, class Selector> struct sort_profile_family {
    using selector_type = Selector;
    using native_view = sort_profile_view<P, Selector>;
    using borrowed_view = profile_view<P, stream_role::borrowed>;
    using borrowed_array = profile_array<P, stream_role::borrowed>;
    using borrowed_writer = profile_borrowed_writer<P>;
  };

  template <class P, class Selector> struct sort_profile_array {
    using policy_type = P;
    using stream_family = sort_profile_family<P, Selector>;
    sort_profile_array() = default;
    auto view() const & {
      return sort_profile_view<P, Selector>(data_.view(), offsets_.view(), metadata_, dictionary_.view(),
        word_view(std::span<std::uint64_t const>(dictionary_offsets_)), seeds_.view());
    }
    auto view() const && = delete;
    std::uint64_t size() const noexcept { return metadata_.record_count; }
    auto const & metadata() const noexcept { return metadata_; }
    auto const & group_offsets() const & noexcept { return offsets_; }
    auto const & data() const & noexcept { return data_; }
    auto const & dictionary() const & noexcept { return dictionary_; }
    std::span<std::uint64_t const> dictionary_offsets() const noexcept { return dictionary_offsets_; }
    auto const & seeds() const & noexcept { return seeds_; }
    // Allocated output capacities, excluding allocator/control-block overhead.
    std::size_t retained_bytes() const noexcept {
      return sizeof(*this) + data_.bytes.capacity() + dictionary_.bytes.capacity() + seeds_.bytes.capacity() +
        8 * (dictionary_offsets_.capacity() + offsets_.low.capacity() + offsets_.high.capacity() + offsets_.sparse.capacity()) +
        sizeof(elias_fano_sample) * offsets_.samples.capacity();
    }
  private:
    friend struct sort_profile_writer<P, Selector>;
    friend struct sort_profile_detail::encoder<P, Selector>;
    bit_string data_, dictionary_, seeds_;
    std::vector<std::uint64_t> dictionary_offsets_{0};
    elias_fano offsets_ = elias_fano::build(std::array<std::uint64_t, 1>{0});
    profile_metadata metadata_ = [] { auto m = profile_detail::initial_metadata<P, stream_role::native>(); m.version = 3; return m; }();
    sort_profile_array(bit_string data, bit_string dictionary, bit_string seeds,
        std::vector<std::uint64_t> dictionary_offsets, elias_fano offsets, profile_metadata metadata)
      : data_(std::move(data)), dictionary_(std::move(dictionary)), seeds_(std::move(seeds)),
        dictionary_offsets_(std::move(dictionary_offsets)), offsets_(std::move(offsets)), metadata_(metadata) {}
  };

  namespace sort_profile_detail {
    // Shared wire framing. A sink supplies position, append and write_count;
    // payload ownership and sealing stay outside this small navigation state.
    template <class P, class Selector> struct encoder {
      static_assert(P::unit == profile_unit::bit, "sort profile framing uses bit addresses");
      using leaves = typename registry_detail::info<typename P::registry_type>::leaves;
      profile_metadata metadata = [] { auto m = profile_detail::initial_metadata<P, stream_role::native>(); m.version = 3; return m; }();
      bit_string dictionary, seeds;
      std::vector<std::uint64_t> dictionary_offsets{0};
      elias_fano offsets;
      std::uint64_t size() const noexcept { return metadata.record_count; }
      template <class Output> void append_frame(Output & out, sort_profile_frame const & frame,
          std::span<bit_view const> spans, std::uint64_t common, bit_view value) {
        visit<leaves>::at(frame.leaf, [&]<class S>(std::type_identity<S>) {
          append<S>(out, frame.path, frame.key_units - frame.path.size(), spans, value, common, frame.retained_limit_bits);
        });
      }
      template <class S, class Output> void append(Output & out, bit_view path, std::uint64_t key_bits,
          std::span<bit_view const> key, bit_view value, std::uint64_t common,
          std::optional<std::uint64_t> retained_limit_bits = {}) {
        using codec = sort_profile_key<typename sort_codec<S>::key_codec>;
        sort_bit_reader validate(value);
        sort_codec<S>::value_codec::skip(validate);
        if (!validate.empty()) throw std::invalid_argument("trailing sort profile value bits");
        auto total = std::uint64_t{0};
        for (auto part : key) total = profile_detail::add(total, part.size());
        if (total != profile_detail::add(path.size(), key_bits) || common > total)
          throw std::invalid_argument("sort merge key spans");
        constexpr auto leaf = ordinal<leaves, S>::value;
        auto found = std::find(ids_.begin(), ids_.end(), leaf);
        auto id = std::size_t(found - ids_.begin());
        if (found == ids_.end()) {
          ids_.push_back(leaf); profile_detail::append(dictionary, path);
          dictionary_offsets.push_back(dictionary.bit_size);
        }
        auto same = size() && compare_bits(previous_path_.view(), path) == 0;
        auto retained = same && codec::front_coded ? common : path.size();
        if (retained < path.size() || retained > total) throw std::invalid_argument("sort retained prefix outside key");
        if (retained_limit_bits)
          retained = std::min(retained, std::max(path.size(), *retained_limit_bits));
        if (!(size() % P::codec_block_size)) {
          raw_offsets_.push_back(out.position()); block_seeds_.push_back(id);
          out.template write_count<exponential_golomb<0>>(retained);
        } else {
          auto joint = same ? retained : compare_common_bits(previous_path_.view(), path).common_bits;
          if (joint > previous_continuation_) throw std::invalid_argument("sort backspace exceeds continuation");
          out.template write_count<typename P::backspace_encoding>(previous_continuation_ - joint);
          if (!same) out.append(path.subview(joint, path.size() - joint));
        }
        auto local = retained - path.size();
        codec::header(out, local, key_bits - local);
        auto skip = retained;
        for (auto part : key) {
          auto prefix = std::min(skip, part.size()); skip -= prefix;
          out.append(part.subview(prefix, part.size() - prefix));
        }
        out.append(value);
        if (!size()) metadata.common_value_width = value.size();
        else if (metadata.common_value_width != value.size()) metadata.common_value_width.reset();
        ++metadata.record_count;
        metadata.terminal_key_units = total;
        previous_path_ = bit_string::copy(path);
        previous_continuation_ = path.size() + (codec::front_coded ? key_bits : 0);
      }
      void finish(std::uint64_t extent) {
        raw_offsets_.push_back(extent);
        auto common = metadata.common_value_width.value_or(0);
        for (std::size_t i = 0; i != raw_offsets_.size(); ++i) {
          auto ordinal = i + 1 == raw_offsets_.size() ? size() : i * P::codec_block_size;
          raw_offsets_[i] -= ordinal * common;
        }
        offsets = elias_fano::build(raw_offsets_);
        auto width = ids_.size() < 2 ? 0u : std::bit_width(ids_.size() - 1);
        sort_bit_writer out(seeds);
        for (auto id : block_seeds_) out.write_bits(id, unsigned(width));
        metadata.extent = extent;
      }
      // The same completed framing state supplies owning and streamed output.
      // Neither adoption nor serialization repeats key/value composition.
      sort_profile_array<P, Selector> take(bit_string data) {
        return {std::move(data), std::move(dictionary), std::move(seeds),
          std::move(dictionary_offsets), std::move(offsets), metadata};
      }
    private:
      bit_string previous_path_;
      std::vector<std::size_t> ids_;
      std::vector<std::uint64_t> raw_offsets_, block_seeds_;
      std::uint64_t previous_continuation_ = 0;
    };
  }

  template <class P, class Selector> struct sort_profile_writer {
    using policy_type = P;
    using array_type = sort_profile_array<P, Selector>;
    template <class S> void append(typename sort_codec<S>::key_codec::value_type const & key,
                                  typename sort_codec<S>::value_codec::value_type const & value,
                                  std::optional<std::uint64_t> retained_limit_bits = {}) {
      require_active();
      if (encoded_only_) throw std::logic_error("typed append after encoded sort frames");
      sort_codec_detail::validate_value_width<S>();
      bit_string path, encoded_value;
      sort_bit_writer path_out(path), value_out(encoded_value);
      Selector::template write<S>(path_out);
      sort_codec<S>::value_codec::write(value_out, value);
      auto bits = sort_profile_key<typename sort_codec<S>::key_codec>::order(key);
      auto logical = sort_profile_detail::concatenate(path.view(), bits.view());
      auto comparison = compare_common_bits(previous_.view(), logical.view());
      if (size() && comparison.order >= 0) throw std::invalid_argument("sort profile requires unique sorted keys");
      std::array<bit_view, 1> spans{logical.view()};
      try {
        sort_bit_writer out(data_);
        encoder_.template append<S>(out, path.view(), bits.bit_size, spans, encoded_value.view(), comparison.common_bits, retained_limit_bits);
        previous_ = std::move(logical);
      } catch (...) { failed_ = true; throw; }
    }
    // Trusted sorted merge output: spans describe the full logical key, but
    // only the suffix after the known output LCP is copied to the file.
    void append_frame(sort_profile_frame const & frame, std::span<bit_view const> spans,
                      std::uint64_t common, bit_view value) {
      require_active();
      try {
        sort_bit_writer out(data_);
        encoder_.append_frame(out, frame, spans, common, value);
        encoded_only_ = true;
      } catch (...) { failed_ = true; throw; }
    }
    std::uint64_t size() const noexcept { return encoder_.size(); }
    bool failed() const noexcept { return failed_; }
    bool finished() const noexcept { return finished_; }
    array_type finish() {
      require_active();
      try {
        encoder_.finish(data_.bit_size);
        auto result = encoder_.take(std::move(data_));
        finished_ = true; return result;
      } catch (...) { failed_ = true; throw; }
    }
  private:
    bit_string data_;
    sort_profile_detail::encoder<P, Selector> encoder_;
    bit_string previous_;
    bool failed_ = false, finished_ = false, encoded_only_ = false;
    void require_active() const {
      if (failed_ || finished_) throw std::logic_error("inactive sort profile writer");
    }
  };

  template <class P, class S, class Selector = registry_selector<typename P::registry_type>>
  bit_string sort_profile_query(typename sort_codec<S>::key_codec::value_type const & key) {
    using key_codec = sort_profile_key<typename sort_codec<S>::key_codec>;
    auto encode = [](bit_view bits) {
      bit_string result;
      // Width is an optional selector hint. Selection still runs for each query.
      if constexpr (requires { typename std::integral_constant<std::uint64_t, Selector::template code_size<S>>; }) {
        auto size = profile_detail::add(Selector::template code_size<S>, bits.size());
        auto bytes = profile_detail::add(size, 7) >> 3;
        if (bytes > result.bytes.max_size()) throw std::length_error("sort query key too large");
        result.bytes.reserve(static_cast<std::size_t>(bytes));
      }
      sort_bit_writer out(result);
      Selector::template write<S>(out);
      out.append(bits);
      return result;
    };
    if constexpr (requires { key_codec::order_view(key); }) return encode(key_codec::order_view(key));
    else {
      auto bits = key_codec::order(key);
      return encode(bits.view());
    }
  }
}
