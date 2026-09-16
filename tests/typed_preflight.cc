/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks encoded contribution lookup, conditional bases and chronological semantics.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/replacement_rebuild.h>
#include <everett/sort_runtime.h>

#include <cassert>
#include <stdexcept>
#include <string>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  template <class P, class Base> struct observed_family : Base {
    struct key_transport : typed_detail::transport_t<P, Base> {
      using transport = typed_detail::transport_t<P, Base>;
      inline static unsigned encodes = 0;
      template <class S> static bit_string encode(typed_detail::key_t<S> const & key) {
        ++encodes;
        return transport::template encode<S>(key);
      }
    };
  };
  // A custom cursor-only runtime can require an rvalue owner and expose no
  // bit_view cursor. Preflight must copy its borrowed key into that interface.
  template <class P> struct owned_only_family : binary_runtime_family<P> {
    using original = binary_runtime_family<P>;
    struct snapshot_type {
      typename original::snapshot_type source;
      struct query_type {
        typename original::snapshot_type::query_type source;
        auto head() const { return source.head(); }
      };
      auto query_root() const { return query_type{source.query_root()}; }
      auto cursor_owned(bit_string && key) const { return source.cursor_owned(std::move(key)); }
      auto admissions() const { return source.admissions(); }
      bool same_layout(snapshot_type const & other) const { return source.same_layout(other.source); }
    };
    template <class Compose> struct runtime_type : original::template runtime_type<Compose> {
      using base = typename original::template runtime_type<Compose>;
      runtime_type() = default;
      static runtime_type from_snapshot(snapshot_type state) {
        return runtime_type(base::from_snapshot(std::move(state.source)));
      }
      snapshot_type snapshot() const { return {base::snapshot()}; }
      snapshot_type advance(std::uint64_t budget) { return {base::advance(budget)}; }
    private:
      explicit runtime_type(base value) : base(std::move(value)) {}
    };
  };
  template <class F> void rejects(F && fn) {
    bool rejected = false;
    try { fn(); } catch (std::invalid_argument const &) { rejected = true; }
    assert(rejected);
  }
  template <class E> void drain(E & engine) { while (engine.pending()) engine.advance(1'000'000); }

  template <class P, class Base> void replacements() {
    using family = observed_family<P, Base>;
    using engine = typed_engine<P, wrapping_fingerprint_algebra, 256, family>;
    using transport = typename family::key_transport;
    engine live("encoded-preflight/1");
    auto key = std::string(1024, 'k') + std::string("\0a", 2);
    auto other = std::string(1024, 'k') + std::string("\0b", 2);
    auto first = engine::put(key, "before");
    assert(transport::encodes == 1);
    transport::encodes = 0;
    auto base = live.contribute(std::move(first));
    assert(transport::encodes == 0);
    auto conditional = base.put(key, "after");
    auto unrelated = engine::put(other, "untouched");
    live.contribute(std::move(unrelated));
    transport::encodes = 0;
    auto updated = live.contribute(std::move(conditional));
    assert(transport::encodes == 0); // Both current and base use the existing key.
    assert(updated.get(key) == "after" && base.get(key) == "before");
    assert(transport::encodes == 2); // Public reads still encode their logical keys.
    auto expected = sort_semantics<strings>::hash_key(key) * sort_semantics<strings>::hash_value(key, "after") +
      sort_semantics<strings>::hash_key(other) * sort_semantics<strings>::hash_value(other, "untouched");
    assert(updated.signature() == expected && updated.live_count() == 2);

    auto stale = base.put(key, "stale");
    transport::encodes = 0;
    rejects([&] { live.contribute(std::move(stale)); });
    assert(transport::encodes == 0 && !live.failed() && live.snapshot().metadata() == updated.metadata());
    auto batch = engine::batch();
    batch.put(key, "must-not-appear").erase("zz/absent");
    auto invalid = std::move(batch).finish();
    transport::encodes = 0;
    rejects([&] { live.contribute(std::move(invalid)); });
    assert(transport::encodes == 0 && !live.failed() && live.snapshot().metadata() == updated.metadata());
    auto deletion = updated.erase(key);
    transport::encodes = 0;
    auto erased = live.contribute(std::move(deletion));
    assert(transport::encodes == 0 && !erased.get(key) && erased.get(other) == "untouched");
    drain(live);
    assert(live.snapshot().metadata() == erased.metadata() && base.get(key) == "before");
  }

  void rebuilding() {
    using family = observed_family<string_policy, sort_runtime_family<string_policy>>;
    using engine = replacement_rebuild_engine<string_policy, wrapping_fingerprint_algebra, 256, family>;
    using transport = family::key_transport;
    engine live("encoded-rebuild/1");
    auto base = live.snapshot();
    auto first = engine::put("a", "first");
    transport::encodes = 0;
    auto current = live.contribute(std::move(first));
    assert(transport::encodes == 1); // The independent inner command remains validated.
    auto disjoint = base.put("b", "second");
    transport::encodes = 0;
    auto second = live.contribute(std::move(disjoint));
    assert(transport::encodes == 1 && second.metadata().clean_base == 2);
    auto stale = base.put("a", "stale");
    transport::encodes = 0;
    rejects([&] { live.contribute(std::move(stale)); });
    assert(transport::encodes == 0 && !live.failed() && live.snapshot().metadata() == second.metadata());
    assert(current.get("a") == "first" && !current.get("b") && second.get("b") == "second");
  }

  struct append_sort {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<>;
    using value_codec = string_value<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type old, state_type const & arrow) { return old + arrow; }
    static state_type compose(std::string const &, state_type old, state_type const & arrow) { return old + arrow; }
    static bool present(std::string const &, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::string const & key) { return u64_table_hash{}.key(key); }
    static std::uint64_t hash_value(std::string const &, state_type const & value) { return u64_table_hash{}.key(value); }
  };
  void chronological() {
    using policy = storage_policy<bin<tip<append_sort>, sort_undefined>, 3>;
    using family = observed_family<policy, sort_runtime_family<policy>>;
    using engine = typed_engine<policy, wrapping_fingerprint_algebra, 256, family>;
    using transport = family::key_transport;
    engine live("encoded-arrows/1");
    auto initial = engine::change("key", "A");
    transport::encodes = 0;
    auto first = live.contribute(std::move(initial));
    assert(transport::encodes == 0);
    std::string expected = "A";
    for (unsigned i = 0; i != 17; ++i) {
      auto arrow = "/" + std::to_string(i);
      auto input = live.snapshot().change("key", arrow);
      transport::encodes = 0;
      auto current = live.contribute(std::move(input));
      assert(transport::encodes == 0);
      expected += arrow;
      assert(current.get("key") == expected && current.live_count() == 1);
      assert(current.signature() == append_sort::hash_key("key") * append_sort::hash_value("key", expected));
    }
    auto stale = first.change("key", "stale");
    transport::encodes = 0;
    rejects([&] { live.contribute(std::move(stale)); });
    assert(transport::encodes == 0 && !live.failed());
    drain(live);
    assert(live.snapshot().get("key") == expected && first.get("key") == "A");
  }
}
int main() {
  replacements<string_policy, binary_runtime_family<string_policy>>();
  replacements<string_policy, sort_runtime_family<string_policy>>();
  replacements<string_policy, owned_only_family<string_policy>>();
  using bytes = storage_policy<unsorted<std::optional<std::string>>>;
  replacements<bytes, binary_runtime_family<bytes>>();
  rebuilding();
  chronological();
}
