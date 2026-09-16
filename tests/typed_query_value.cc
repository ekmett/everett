/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks pinned value decoding and unchanged owning query enumeration.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sort_runtime.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace {
  using namespace everett;
  void check(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && fn) {
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected query rejection");
  }
  struct observed_value : tombstone_value<string_value<>> {
    inline static unsigned reads = 0;
    inline static std::byte const * storage = nullptr;
    inline static bool fail = false;
    inline static std::function<void()> on_read;
    static value_type read(sort_bit_reader & input) {
      ++reads;
      storage = input.take_bits(0).storage().data();
      if (fail) throw std::runtime_error("injected leaf decode failure");
      if (on_read) on_read();
      return tombstone_value<string_value<>>::read(input);
    }
  };
  struct replacement_sort : sort_semantics<unsorted<std::optional<std::string>>> {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = observed_value;
  };
  struct composition {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = observed_value;
    using state_type = std::string;
    static constexpr bool replacement = false;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type state, std::optional<std::string> const & arrow) {
      return state + (arrow ? *arrow : "<tombstone>");
    }
  };
  template <class S> using policy = storage_policy<bin<tip<S>, sort_undefined>, 3>;
  template <class Q> concept scoped_query = requires(Q && query) {
    cola_detail::query_access::borrow_query<policy<replacement_sort>>(std::forward<Q>(query));
  };
  static_assert(scoped_query<bit_string &> && scoped_query<bit_string const &>);
  static_assert(!scoped_query<bit_string> && !scoped_query<bit_string const>);
  using rows = std::map<std::string, std::optional<std::string>>;
  template <class S> auto native(rows const & input) {
    sort_profile_writer<policy<S>> writer;
    for (auto const & [key, value] : input) writer.template append<S>(key, value);
    return std::make_shared<sort_profile_array<policy<S>> const>(writer.finish());
  }
  template <class S> using node = cola_index<policy<S>, sort_profile_array<policy<S>>>;
  template <class S> auto bounded(typename node<S>::pair_type root) {
    auto empty = native<S>({});
    while (root->virtual_size() > policy<S>::group_size)
      root = std::make_shared<node<S> const>(node<S>::adopt_native(empty, root));
    return cola_query_root<policy<S>, node<S>>::adopt_prepared(std::move(root));
  }

  // A runtime can expose only the legacy cursor. It must retain that fallback.
  template <class P, class Blob, bool Direct> struct family {
    using key_transport = sort_key_transport<P>;
    struct snapshot_type {
      cola_query_root<P, Blob> root;
      std::uint64_t mass;
      std::uint64_t admissions() const noexcept { return mass; }
      auto const & query_root() const requires Direct { return root; }
      auto cursor(bit_view query) const { return root.cursor(query); }
      auto cursor_owned(bit_string query) const { return root.cursor_owned(std::move(query)); }
    };
  };
  template <bool Direct = true, class P, class Blob> auto typed(cola_query_root<P, Blob> root,
      std::uint64_t mass, std::uint64_t live) {
    using f = family<P, Blob, Direct>;
    typed_world_metadata<> metadata;
    metadata.schema_id = "query-value-fixture/1";
    metadata.live_count = live;
    return typed_world<P, wrapping_fingerprint_algebra, f>::restore(
      typename f::snapshot_type{std::move(root), mass}, std::move(metadata), "query-value-fixture/1");
  }
  template <class S> auto three(rows const & local, rows const & side, rows const & older) {
    auto main = std::make_shared<node<S> const>(node<S>::adopt_native(native<S>(older)));
    auto result = std::make_shared<node<S> const>(node<S>::adopt_native(native<S>(local), main, native<S>(side)));
    return bounded<S>(std::move(result));
  }
  template <class P, class Blob> std::vector<std::optional<std::string>> all(
      cola_query_root<P, Blob> const & root, bit_string key) {
    auto cursor = root.cursor_owned(std::move(key));
    auto copy = cursor;
    cursor = std::move(copy);
    std::vector<std::optional<std::string>> result;
    while (!cursor.done()) {
      cursor.step(1);
      while (cursor.has_match()) {
        auto match = cursor.take_match();
        sort_bit_reader in(match.value.view());
        result.push_back(observed_value::read(in));
        check(in.empty(), "owning match value framing changed");
      }
    }
    return result;
  }

  // This valid public cursor view deliberately does not expose the private
  // capture implementation. Merely having query_root must not opt it in.
  struct foreign_view {
    using p = policy<replacement_sort>;
    typename node<replacement_sort>::view_type source;
    auto borrowed(unsigned route) const { return source.borrowed(route); }
    auto search_window(std::uint64_t group, profile_query_context<p> const & context) const {
      return source.search_window(group, context);
    }
  };
  struct foreign_blob {
    using native_pointer = typename node<replacement_sort>::native_pointer;
    using pair_type = std::shared_ptr<foreign_blob const>;
    typename node<replacement_sort>::pair_type source;
    foreign_view view() const { return {source->view()}; }
    std::uint64_t virtual_size() const { return source->virtual_size(); }
    std::uint64_t group_count() const { return source->group_count(); }
    pair_type main_target() const { return {}; }
    native_pointer secondary_target() const { return {}; }
  };

  // Unlike the foreign view above, this family exposes the private capture
  // path through a real cola_index_view, but retains its comparison argument.
  struct retaining_view {
    using p = policy<replacement_sort>;
    sort_profile_view<p> source;
    inline static std::optional<profile_query_context<p>> retained;
    std::uint64_t size() const { return source.size(); }
    auto encoded_at(std::uint64_t ordinal) const { return source.encoded_at(ordinal); }
    template <class F> auto compare_window(std::uint64_t first, std::uint64_t last,
        profile_query_context<p> context, F && fn, profile_comparison_work * work = nullptr) const {
      retained = context;
      return source.compare_window(first, last, std::move(context), std::forward<F>(fn), work);
    }
  };
  struct retaining_family {
    using native_view = retaining_view;
    using borrowed_view = profile_view<policy<replacement_sort>, stream_role::borrowed>;
  };
  struct retaining_blob {
    using p = policy<replacement_sort>;
    using native_pointer = typename node<replacement_sort>::native_pointer;
    using pair_type = std::shared_ptr<retaining_blob const>;
    typename node<replacement_sort>::pair_type source;
    auto view() const {
      auto v = source->view();
      return cola_index_view<p, retaining_family>({v.native()}, {v.borrowed(0), v.borrowed(1)},
        {v.interleave(0), v.interleave(1)}, {v.false_borrow_bits(0), v.false_borrow_bits(1)},
        {v.cut_lcps(0), v.cut_lcps(1)}, v.virtual_size());
    }
    std::uint64_t virtual_size() const { return source->virtual_size(); }
    std::uint64_t group_count() const { return source->group_count(); }
    pair_type main_target() const { return {}; }
    native_pointer secondary_target() const { return {}; }
  };
  static_assert(cola_detail::query_access::scoped_views<node<replacement_sort>>);
  static_assert(!cola_detail::query_access::scoped_views<retaining_blob>);

  void custom_context_retention() {
    using p = policy<replacement_sort>;
    std::string key(4096, 'k');
    auto expected = sort_profile_query<p, replacement_sort>(key);
    {
      auto native_node = std::make_shared<node<replacement_sort> const>(
        node<replacement_sort>::adopt_native(native<replacement_sort>({{key, "retained"}})));
      auto source = std::make_shared<retaining_blob const>(retaining_blob{native_node});
      auto root = cola_query_root<p, retaining_blob>::adopt_prepared(source);
      auto state = typed(root, 1, 1);
      check(state.get(key) == "retained", "custom retaining view query");
      auto borrowed = expected;
      auto value = cola_detail::first_value(root, std::as_const(borrowed), [](bit_view bits) {
        sort_bit_reader input(bits);
        return observed_value::read(input);
      });
      check(value && *value == "retained", "custom view borrowed-key query");
      borrowed = {};
      check(retaining_view::retained.has_value(), "custom view never retained a comparison");
      check(!compare_common_bits(retaining_view::retained->query(), expected.view()).order,
        "custom view's retained context outlived its query storage");
    }
    // The retained query owner also survives snapshot and graph destruction.
    auto kept = std::move(*retaining_view::retained);
    retaining_view::retained.reset();
    check(!compare_common_bits(kept.query(), expected.view()).order, "moved retained comparison lost its owner");
  }

  void replacement_reads() {
    using p = policy<replacement_sort>;
    auto root = three<replacement_sort>({{"k", "new"}}, {{"k", "side"}}, {{"k", "old"}});
    auto state = typed(root, 3, 1);
    observed_value::reads = 0;
    auto result = state.get("k");
    check(result == "new" && observed_value::reads == 1, "replacement did not decode exactly its first hit");
    check(observed_value::storage == root.head()->native().view().data().storage().data(),
      "replacement decoded a copied native value");
    auto legacy = typed<false>(root, 3, 1);
    check(legacy.get("k") == result && observed_value::storage != root.head()->native().view().data().storage().data(),
      "custom runtime owning fallback changed");
    auto single = std::make_shared<node<replacement_sort> const>(node<replacement_sort>::adopt_native(native<replacement_sort>({{"k", "single"}})));
    auto foreign = std::make_shared<foreign_blob const>(foreign_blob{single});
    auto foreign_root = cola_query_root<p, foreign_blob>::adopt_prepared(foreign);
    check(typed(foreign_root, 1, 1).get("k") == "single" &&
      observed_value::storage != single->native().view().data().storage().data(),
      "incompatible custom view did not keep its owning cursor fallback");
    observed_value::reads = 0;
    check(!state.get("absent") && !observed_value::reads, "miss decoded a value");
    observed_value::fail = true;
    rejects([&] { (void)state.get("k"); });
    observed_value::fail = false;
    check(state.get("k") == "new", "failed decode damaged immutable state");
    auto key = sort_profile_query<p, replacement_sort>("k");
    check(all(root, key) == std::vector<std::optional<std::string>>{"new", "side", "old"},
      "owning cursor lost occurrences or changed order");

    auto deleted = three<replacement_sort>({{"k", std::nullopt}}, {{"k", "side"}}, {{"k", "old"}});
    observed_value::reads = 0;
    check(!typed(deleted, 3, 0).get("k") && observed_value::reads == 1,
      "first tombstone continued to older values");
    auto side = three<replacement_sort>({{"z", "local"}}, {{"k", "side"}}, {{"k", "old"}});
    observed_value::reads = 0;
    check(typed(side, 3, 2).get("k") == "side" && observed_value::reads == 1, "secondary first match changed");
    check(observed_value::storage == side.head()->secondary_target()->view().data().storage().data(),
      "replacement decoded a copied secondary value");
  }

  void boundary_oracle() {
    using p = policy<replacement_sort>;
    rows local, side, older, expected;
    for (unsigned i = 0; i != 73; ++i) {
      std::string key(2, '\0'); key[0] = char(i / 9); key[1] = char(i % 9);
      older[key] = "old/" + std::to_string(i);
      if (i % 3) side[key] = "side/" + std::to_string(i);
      if (i % 4) local[key] = i % 7 ? std::optional("new/" + std::to_string(i)) : std::nullopt;
    }
    expected = older;
    for (auto const & x : side) expected[x.first] = x.second;
    for (auto const & x : local) expected[x.first] = x.second;
    auto root = three<replacement_sort>(local, side, older);
    auto live = std::count_if(expected.begin(), expected.end(), [](auto const & x) { return x.second.has_value(); });
    auto state = typed(root, local.size() + side.size() + older.size(), static_cast<std::uint64_t>(live));
    bool false_boundary = false;
    for (auto const & [key, value] : expected) {
      observed_value::reads = 0;
      check(state.get(key) == value && observed_value::reads == 1, "binary boundary replacement oracle");
      auto query = sort_profile_query<p, replacement_sort>(key);
      auto context = profile_query_context<p>::from_owned(std::move(query));
      auto current = root.head();
      std::uint64_t group = 0;
      while (current) {
        auto window = current->view().project(group);
        auto result = current->view().search_window(group, context);
        if (result.native && result.native->ordinal < window.native_first) false_boundary = true;
        if (auto & next = result.predecessors[0]) {
          group = next->target_ordinal / p::group_size;
          context = std::move(next->comparison);
          current = current->main_target();
        } else current.reset();
      }
    }
    check(false_boundary, "fixture never recovered a native across a false-borrow boundary");
    auto compose_root = three<composition>({{"k", "N"}}, {{"k", "S"}}, {{"k", "O"}});
    observed_value::reads = 0;
    check(typed(compose_root, 3, 1).get("k") == "OSN" && observed_value::reads == 3,
      "all-occurrence composition order changed");
  }

  void owning_lifetime() {
    using p = policy<replacement_sort>;
    auto root = three<replacement_sort>({{"k", std::string(64, 'x')}}, {}, {});
    auto cursor = root.cursor_owned(sort_profile_query<p, replacement_sort>("k"));
    cursor.step(1);
    auto match = cursor.take_match();
    auto source = match.source;
    check(match.value.bytes.data() != source->native().view().data().storage().data(), "public match stopped owning value");
    auto copy = match;
    match.value.bytes.assign(match.value.bytes.size(), std::byte{0});
    sort_bit_reader input(copy.value.view());
    check(observed_value::read(input) == std::string(64, 'x'), "public match copy aliased value storage");
  }

  void reentrant_reads() {
    // Different long queries force independent allocations. After decoding a
    // native hit, its secondary window still uses the outer comparison context.
    std::string outer(4096, 'a'), inner(4096, 'z');
    inner.back() = '\0';
    auto root = three<replacement_sort>({{outer, "outer"}},
      {{outer, "older-outer"}, {inner, "inner"}}, {{inner, "old-inner"}});
    auto state = typed(root, 4, 2);
    unsigned depth = 0, nested_reads = 0;
    struct depth_guard {
      unsigned & depth;
      explicit depth_guard(unsigned & value) : depth(value) { ++depth; }
      ~depth_guard() { --depth; }
    };
    observed_value::on_read = [&] {
      depth_guard guard(depth);
      if (depth != 1) return;
      ++nested_reads;
      check(state.get(inner) == "inner", "nested query lost its independent key");
    };
    auto answer = state.get(outer);
    observed_value::on_read = {};
    check(answer == "outer" && nested_reads == 1 && depth == 0, "reentrant query damaged its caller");

    // A nested decoder exception unwinds both contexts. No shared scratch or
    // retained stack alias may affect the next query against the same snapshot.
    observed_value::on_read = [&] {
      depth_guard guard(depth);
      if (depth == 2) throw std::runtime_error("nested decode failure");
      (void)state.get(inner);
    };
    rejects([&] { (void)state.get(outer); });
    observed_value::on_read = {};
    check(depth == 0 && state.get(outer) == "outer" && state.get(inner) == "inner",
      "nested exception retained a borrowed query context");
  }

  void rejected_query_storage() {
    using p = policy<replacement_sort>;
    auto root = three<replacement_sort>({}, {}, {});
    auto decode = [](bit_view) { return false; };
    rejects([&] { (void)cola_detail::first_value(root, bit_string{{}, 1}, decode); });
    rejects([&] { (void)cola_detail::first_value(root, bit_string{{std::byte{1}}, 1}, decode); });
    // Both invalid extents and noncanonical padding are rejected even before
    // an empty-root shortcut, just as for public owned query contexts.
    (void)cola_detail::first_value(root, sort_profile_query<p, replacement_sort>(""), decode);
  }

  void mapped_lifetime() {
#if defined(__unix__) || defined(__APPLE__)
    using p = policy<replacement_sort>;
    using mapped = mapped_sort_profile<p>;
    using blob = mapped_sort_cola<p>;
    auto directory = std::filesystem::temp_directory_path() / ("everett-query-value-" + std::to_string(::getpid()));
    std::filesystem::create_directory(directory);
    auto write = [](auto const & path, auto const & bytes) {
      std::ofstream file(path, std::ios::binary);
      file.write(reinterpret_cast<char const *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!file) throw std::runtime_error("fixture write failed");
    };
    std::optional<std::string> answer;
    {
      auto owned = native<replacement_sort>({{"", "empty"}, {std::string("k\0", 2), std::string(128, 'v')}});
      object_id native_id("10000000000000000000000000000000"), index_id("20000000000000000000000000000000");
      auto native_path = directory / "fixture.kv", index_path = directory / "fixture.index";
      write(native_path, encoded_sort_sections<p>::from(*owned).materialize());
      auto n = std::make_shared<mapped const>(mapped::open(native_path));
      auto built = cola_index<p, mapped, blob>::adopt_native(n);
      write(index_path, encode_cola_sections(built, native_id).materialize());
      auto index = std::make_shared<mapped_cola_index<p> const>(mapped_cola_index<p>::open(index_path));
      auto pair = blob::bind({native_id, index_id}, n, index);
      auto root = cola_query_root<p, blob>::adopt_prepared(pair);
      auto state = typed(root, 2, 2);
      auto storage = n->view().data().storage().data();
      n.reset(); index.reset(); pair.reset();
      std::filesystem::remove(native_path); std::filesystem::remove(index_path);
      observed_value::reads = 0;
      answer = state.get(std::string("k\0", 2));
      check(answer == std::string(128, 'v') && observed_value::reads == 1 && observed_value::storage == storage,
        "mapped replacement did not decode directly under its pin");
      check(state.get("") == "empty", "empty mapped key");
    }
    std::filesystem::remove_all(directory);
    check(answer == std::string(128, 'v'), "returned value depended on a dead mapping");
#endif
  }
}

int main() {
  try {
    replacement_reads();
    custom_context_retention();
    boundary_oracle();
    owning_lifetime();
    reentrant_reads();
    rejected_query_storage();
    mapped_lifetime();
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
