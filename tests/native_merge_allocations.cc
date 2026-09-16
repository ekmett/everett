/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks allocation-free equal-key donor selection for conservative tombstone merges.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/native_merge.h>
#include <everett/sections.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
  constexpr std::size_t prefix_bytes = 128 << 10;
  thread_local bool track = false;
  thread_local unsigned large_allocations = 0;
  void * allocate(std::size_t size) {
    if (track && size >= prefix_bytes / 2) ++large_allocations;
    if (auto result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
  }
  struct pause_tracking {
    bool previous = std::exchange(track, false);
    ~pause_tracking() { track = previous; }
  };
}
void * operator new(std::size_t size) { return allocate(size); }
void * operator new[](std::size_t size) { return allocate(size); }
void operator delete(void * pointer) noexcept { std::free(pointer); }
void operator delete[](void * pointer) noexcept { std::free(pointer); }
void operator delete(void * pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void * pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
  using namespace everett;
  // Count frontier allocations independently of the real output's necessary
  // buffer growth. Framing and final EF use the production native writer.
  template <class P> struct observed_output {
    using policy_type = P;
    profile_detail::native_output<P> output{8 >> P::unit_shift};
    std::uint64_t size() const noexcept { return output.size(); }
    bool finished() const noexcept { return output.finished(); }
    bool failed() const noexcept { return output.failed(); }
    auto common_value_width() const noexcept { return output.common_value_width(); }
    void append(std::uint64_t retained, bit_view literal, bit_view value) {
      pause_tracking paused;
      output.append(retained, literal, value);
    }
    auto finish() { return output.finish(); }
  };
  struct semantic {
    bit_view operator()(bit_view, bit_view newer) const { return newer; }
    bool is_tombstone(bit_view value) const { return profile_detail::load_bits(value, 0, 8) == 0; }
  };
  profile_record row(std::string const & key, unsigned value, std::optional<std::uint64_t> cap = {}) {
    return {bit_string::from_bytes(key), bit_string::from_bytes(std::string(1, char(value))), cap};
  }
  template <class P> void exercise(bool shorter_newer, bool absent, bool owned_value = false) {
    using array = profile_array<P>;
    // Nonzero caps also exercise bit positions within a byte in the bit profile.
    constexpr std::uint64_t low_cap = 37;
    std::string predecessor(prefix_bytes, 'p'); predecessor += 'a';
    auto target = predecessor; target.back() = 'b';
    auto old_cap = shorter_newer ? std::optional<std::uint64_t>{} : low_cap;
    auto new_cap = shorter_newer ? low_cap : std::optional<std::uint64_t>{};
    std::vector old_rows{row(predecessor, 1), row(target, 1, old_cap)};
    std::vector new_rows{row(predecessor, 2), row(target, absent ? 0 : 3, new_cap)};
    auto old = std::make_shared<array const>(array::build(old_rows));
    auto fresh = std::make_shared<array const>(array::build(new_rows));
    auto old_at = old->view().encoded_at(1).retained, new_at = fresh->view().encoded_at(1).retained;
    assert(shorter_newer ? new_at < old_at : old_at < new_at);
    auto expected_rows = std::vector{row(predecessor, 2), row(target, absent ? 0 : 3,
      absent ? std::optional<std::uint64_t>{low_cap} : std::nullopt)};
    auto expected = array::build(expected_rows);
    auto verify = [&](auto compose) {
      using compose_type = decltype(compose);
      using builder = native_merge_builder<P, array, compose_type, observed_output<P>>;
      static_assert(builder::encoded_keys);
      builder merge(observed_output<P>{}, old, fresh, compose);
      assert(merge.step().keys == 1);
      large_allocations = 0; track = true;
      try { assert(merge.step().keys == 1); }
      catch (...) { track = false; throw; }
      track = false;
      assert(large_allocations == 0 && merge.done());
      auto actual = merge.finish();
      assert(encode_native_sections(actual).materialize() == encode_native_sections(expected).materialize());
      auto cursor = actual.view().cursor(); cursor.advance();
      assert(compare_bits(cursor.peek().key.prefix, expected_rows[1].key.view()) == 0);
      assert(compare_bits(cursor.peek().value, expected_rows[1].value.view()) == 0);
      assert(actual.view().encoded_at(1).retained == expected.view().encoded_at(1).retained);
    };
    if (owned_value) {
      struct owned : semantic {
        bit_string operator()(bit_view, bit_view newer) const { return bit_string::copy(newer); }
      };
      verify(owned{});
    } else verify(semantic{});
    // A key-aware composer still receives reconstructed complete keys, with
    // the original chronological value arguments and identical output bytes.
    struct checked_keyed : semantic {
      std::vector<profile_record> const * expected;
      unsigned * calls;
      bit_view operator()(bit_view key, bit_view older, bit_view newer) const {
        assert(*calls < expected->size());
        assert(compare_bits(key, (*expected)[*calls].key.view()) == 0);
        assert(profile_detail::load_bits(older, 0, 8) == 1);
        ++*calls;
        return newer;
      }
    };
    unsigned calls = 0;
    checked_keyed compose{{}, &expected_rows, &calls};
    using keyed_builder = native_merge_builder<P, array, checked_keyed>;
    static_assert(!keyed_builder::encoded_keys);
    keyed_builder decoded(old, fresh, compose, 8 >> P::unit_shift);
    decoded.step(2); auto actual = decoded.finish();
    assert(calls == 2);
    assert(encode_native_sections(actual).materialize() == encode_native_sections(expected).materialize());
  }
}
int main() try {
  using bytes = storage_policy<>;
  using bits = storage_policy<tip<encoded_sort<bit_encoding<>>>>;
  for (bool newer : {false, true}) for (bool absent : {false, true}) {
    exercise<bytes>(newer, absent); exercise<bits>(newer, absent);
  }
  exercise<bytes>(true, true, true); exercise<bits>(true, true, true);
  std::cout << "native merge donors: no full-key allocation, identical bytes and key-aware composition passed\n";
} catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
