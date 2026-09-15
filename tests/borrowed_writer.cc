/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/profile.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  // Only append itself is faulted. Fixture/oracle construction and assertions
  // run with allocation enabled, including while inspecting rejected appends.
  thread_local std::ptrdiff_t fail_after = -1;
  void * allocate(std::size_t size) {
    if (fail_after == 0) throw std::bad_alloc();
    if (fail_after > 0) --fail_after;
    if (auto result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
  }
}

void * operator new(std::size_t size) { return allocate(size); }
void * operator new[](std::size_t size) { return allocate(size); }
void operator delete(void * pointer) noexcept { std::free(pointer); }
void operator delete[](void * pointer) noexcept { std::free(pointer); }
void operator delete(void * pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void * pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
  using namespace everett;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class P> using array_type = profile_array<P, stream_role::borrowed>;
  template <class P> void same(array_type<P> const & actual, array_type<P> const & expected,
      std::span<profile_record const> records) {
    require(std::equal(actual.bytes().begin(), actual.bytes().end(), expected.bytes().begin(), expected.bytes().end()),
      "borrowed encoded bytes differ from batch");
    auto const & a = actual.metadata(); auto const & b = expected.metadata();
    require(a.record_count == b.record_count && a.extent == b.extent &&
      a.common_value_width == b.common_value_width && a.terminal_key_units == b.terminal_key_units,
      "borrowed metadata differs from batch");
    auto const & x = actual.group_offsets(); auto const & y = expected.group_offsets();
    require(x.low == y.low && x.high == y.high && x.sparse == y.sparse && x.low_width == y.low_width &&
      x.entry_count == y.entry_count && x.universe == y.universe && x.samples.size() == y.samples.size(),
      "borrowed EF differs from batch");
    for (std::size_t i = 0; i != x.samples.size(); ++i)
      require(x.samples[i].first == y.samples[i].first && x.samples[i].sparse == y.samples[i].sparse,
        "borrowed EF sample differs from batch");
    auto cursor = actual.view().cursor();
    for (auto const & record : records) {
      require(!cursor.done(), "borrowed cursor ended early");
      auto key = cursor.peek().key.prefix;
      require(key.size() == record.key.bit_size, "borrowed reconstructed length");
      for (std::uint64_t i = 0; i != key.size(); ++i)
        require(key.at(i) == record.key.view().at(i), "borrowed reconstructed bits");
      cursor.advance();
    }
    require(cursor.done(), "borrowed cursor has extra records");
  }

  template <class P> bit_string key_for(std::uint64_t id, std::size_t tail) {
    std::string bytes(2048, 'p');
    for (unsigned i = 8; i; --i) bytes.push_back(char(id >> (8 * (i - 1))));
    bytes.append(tail, char(id * 19));
    auto key = bit_string::from_bytes(bytes);
    if constexpr (P::unit == profile_unit::bit) {
      key.bytes.push_back(std::byte((id & 1) ? 0xa0 : 0x40));
      key.bit_size += 3;
    }
    return key;
  }
  template <class P> void prefix_reuse() {
    std::vector<profile_record> records{{{}, {}}};
    constexpr std::array<std::size_t, 8> tails{0, 1, 8192, 3, 63, 4096, 2, 0};
    for (std::size_t i = 0; i != 40; ++i) {
      auto key = key_for<P>(i, tails[i % tails.size()]);
      records.push_back({key, {}});
      records.push_back({std::move(key), {}});
    }
    for (unsigned mode = 0; mode != 3; ++mode) {
      std::vector<std::uint64_t> ceilings;
      for (std::size_t i = 0; i != records.size(); ++i)
        ceilings.push_back(mode == 0 ? std::numeric_limits<std::uint64_t>::max() : mode == 1 ? 0 : i % 7);
      profile_borrowed_writer<P> writer;
      for (std::size_t i = 0; i != records.size(); ++i) {
        // Independently shifted input and immediate scratch destruction test
        // both unaligned suffix copies and predecessor ownership.
        auto scratch = bit_string::from_bits("10101");
        profile_detail::append(scratch, records[i].key.view());
        writer.append(scratch.view().subview(5, records[i].key.bit_size), ceilings[i]);
      }
      same(writer.finish(), array_type<P>::build(records, ceilings), records);
    }
  }

  template <class P> void allocation_rollback() {
    unsigned rejected = 0;
    for (auto count : std::array<std::uint64_t, 5>{0, 1, P::codec_block_size - 1,
        P::codec_block_size, P::codec_block_size + 1}) {
      std::vector<profile_record> seed;
      for (std::uint64_t i = 0; i != count; ++i) seed.push_back({key_for<P>(i, 0), {}});
      auto next = key_for<P>(count, 8192);
      auto last = key_for<P>(count + 1, 1);
      for (auto ceiling : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
        auto expected_records = seed;
        expected_records.push_back({next, {}}); expected_records.push_back({last, {}});
        std::vector<std::uint64_t> ceilings(expected_records.size(), ceiling);
        auto expected = array_type<P>::build(expected_records, ceilings);
        bool reached_success = false;
        for (std::ptrdiff_t failure = 0; failure != 32; ++failure) {
          profile_borrowed_writer<P> writer;
          for (auto const & record : seed) writer.append(record.key.view(), ceiling);
          fail_after = failure;
          bool failed = false;
          try { writer.append(next.view(), ceiling); }
          catch (std::bad_alloc const &) { failed = true; }
          catch (...) { fail_after = -1; throw; }
          fail_after = -1;
          if (failed) {
            ++rejected;
            require(writer.size() == seed.size() && !writer.finished(), "failed append changed progress");
            auto checkpoint = writer;
            same(checkpoint.finish(), array_type<P>::build(seed, std::span(ceilings).first(seed.size())), seed);
            writer.append(next.view(), ceiling);
          }
          writer.append(last.view(), ceiling);
          same(writer.finish(), expected, expected_records);
          if (!failed) { reached_success = true; break; }
        }
        require(reached_success, "allocation fault sweep did not reach success");
      }
    }
    require(rejected >= 10, "allocation rollback fixture did not inject failures");
  }
}

int main() {
  try {
    using bytes = storage_policy<profile_unit::byte>;
    using bits = storage_policy<profile_unit::bit>;
    prefix_reuse<bytes>(); prefix_reuse<bits>();
    allocation_rollback<bytes>(); allocation_rollback<bits>();
    std::cout << "Borrowed predecessor reuse, exact encoding and allocation rollback passed\n";
  } catch (std::exception const & error) {
    fail_after = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks incremental borrowed writer ownership and rejected-append guarantees.
 */
