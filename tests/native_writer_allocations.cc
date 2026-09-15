/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks incremental native writer ownership and rejected-append guarantees.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/native_writer.h>

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
  using namespace diet;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class P> using array_type = profile_array<P>;
  template <class P> void same(array_type<P> const & actual, array_type<P> const & expected,
      std::span<profile_record const> records, bool exact = true) {
    if (exact) {
      require(std::equal(actual.bytes().begin(), actual.bytes().end(), expected.bytes().begin(), expected.bytes().end()),
        "native encoded bytes differ from batch");
      auto const & a = actual.metadata(); auto const & b = expected.metadata();
      require(a.record_count == b.record_count && a.extent == b.extent &&
        a.common_value_width == b.common_value_width && a.terminal_key_units == b.terminal_key_units,
        "native metadata differs from batch");
      auto const & x = actual.group_offsets(); auto const & y = expected.group_offsets();
      require(x.low == y.low && x.high == y.high && x.sparse == y.sparse && x.low_width == y.low_width &&
        x.entry_count == y.entry_count && x.universe == y.universe && x.samples.size() == y.samples.size(),
        "native EF differs from batch");
      for (std::size_t i = 0; i != x.samples.size(); ++i)
        require(x.samples[i].first == y.samples[i].first && x.samples[i].sparse == y.samples[i].sparse,
          "native EF sample differs from batch");
    }
    auto cursor = actual.view().cursor();
    for (auto const & record : records) {
      require(!cursor.done(), "native cursor ended early");
      auto key = cursor.peek().key.prefix;
      require(key.size() == record.key.bit_size, "native reconstructed length");
      for (std::uint64_t i = 0; i != key.size(); ++i)
        require(key.at(i) == record.key.view().at(i), "native reconstructed bits");
      auto value = cursor.peek().value;
      require(value.size() == record.value.bit_size, "native value length");
      for (std::uint64_t i = 0; i != value.size(); ++i)
        require(value.at(i) == record.value.view().at(i), "native value bits");
      cursor.advance();
    }
    require(cursor.done(), "native cursor has extra records");
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
  template <class P> bit_string value_for(std::uint64_t id, std::uint64_t units) {
    std::string bits(units * P::bits_per_unit, '0');
    for (std::size_t i = 0; i != bits.size(); ++i) if ((i + id) % 3) bits[i] = '1';
    return bit_string::from_bits(bits);
  }
  template <class P> void prefix_reuse() {
    for (auto common : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0},
        std::optional<std::uint64_t>{P::unit == profile_unit::byte ? 8 : 13}}) {
      std::vector<profile_record> records{{{}, value_for<P>(0, common.value_or(0))}};
      constexpr std::array<std::size_t, 8> tails{0, 1, 8192, 3, 63, 4096, 2, 0};
      for (std::size_t i = 0; i != 40; ++i)
        records.push_back({key_for<P>(i, tails[i % tails.size()]), value_for<P>(i, common.value_or((i + 1) % 9))});
      profile_native_writer<P> writer(common);
      for (auto const & record : records) {
        auto key = bit_string::from_bits("10101"), value = bit_string::from_bits("101");
        profile_detail::append(key, record.key.view()); profile_detail::append(value, record.value.view());
        writer.append(key.view().subview(5, record.key.bit_size), value.view().subview(3, record.value.bit_size));
      }
      same(writer.finish(), array_type<P>::build(records), records);
    }
  }

  template <class P> void allocation_rollback() {
    unsigned rejected = 0;
    for (auto count : std::array<std::uint64_t, 5>{0, 1, P::codec_block_size - 1,
        P::codec_block_size, P::codec_block_size + 1}) {
      for (auto common : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0},
          std::optional<std::uint64_t>{P::unit == profile_unit::byte ? 8 : 13}}) {
        std::vector<profile_record> seed;
        for (std::uint64_t i = 0; i != count; ++i)
          seed.push_back({key_for<P>(i, 0), value_for<P>(i, common.value_or(i % 9))});
        profile_record next{key_for<P>(count, 8192), value_for<P>(count, common.value_or(8192))};
        profile_record last{key_for<P>(count + 1, 1), value_for<P>(count + 1, common.value_or(1))};
        auto expected_records = seed;
        expected_records.push_back(next); expected_records.push_back(last);
        auto expected = array_type<P>::build(expected_records);
        bool reached_success = false;
        for (std::ptrdiff_t failure = 0; failure != 32; ++failure) {
          bool failed = false;
          for (bool retry : {false, true}) {
            profile_native_writer<P> writer(common);
            for (auto const & record : seed) writer.append(record);
            fail_after = failure;
            failed = false;
            try { writer.append(next); }
            catch (std::bad_alloc const &) { failed = true; }
            catch (...) { fail_after = -1; throw; }
            fail_after = -1;
            if (failed) {
              ++rejected;
              require(writer.size() == seed.size() && !writer.finished(), "failed append changed progress");
              if (!retry) {
                auto actual = writer.finish();
                require(actual.metadata().common_value_width == common, "failure changed width policy");
                auto batch = array_type<P>::build(seed);
                same(actual, batch, seed, batch.metadata().common_value_width == common);
                continue;
              }
              writer.append(next);
            }
            writer.append(last);
            same(writer.finish(), expected, expected_records);
          }
          if (!failed) { reached_success = true; break; }
        }
        require(reached_success, "allocation fault sweep did not reach success");
      }
    }
    require(rejected >= 20, "allocation rollback fixture did not inject failures");
  }

  template <class P> void encoded_cursor_allocations() {
    std::vector<profile_record> records;
    for (std::uint64_t i = 0; i != P::codec_block_size * 2 + 1; ++i)
      records.push_back({key_for<P>(i, i % 3 ? 3 : 8192), value_for<P>(i, i % 9)});
    profile_native_writer<P> writer;
    for (auto const & record : records) writer.append(record);
    auto array = writer.finish();
    auto view = array.view();
    std::uint64_t count = 0, observed_key_units = 0, expected_key_units = 0;
    for (auto const & record : records) expected_key_units += record.key.bit_size / P::bits_per_unit;
    bool borrowed = true;
    fail_after = 0;
    try {
      // Any allocation for key reconstruction or frame ownership fails here.
      auto cursor = view.encoded_cursor();
      while (!cursor.done()) {
        auto copy = cursor;
        auto const & record = cursor.peek();
        observed_key_units += record.key_units;
        borrowed = borrowed && record.suffix.storage().data() == array.bytes().data() &&
          record.value.storage().data() == array.bytes().data() && copy.ordinal() == count;
        cursor.advance();
        ++count;
      }
    } catch (...) { fail_after = -1; throw; }
    fail_after = -1;
    require(count == records.size() && borrowed && observed_key_units == expected_key_units,
            "allocation-free encoded cursor lost frames or borrowed storage");
  }
}

int main() {
  try {
    using bytes = storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>>;
    using bits = storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>>;
    prefix_reuse<bytes>(); prefix_reuse<bits>();
    allocation_rollback<bytes>(); allocation_rollback<bits>();
    encoded_cursor_allocations<bytes>(); encoded_cursor_allocations<bits>();
    std::cout << "Native predecessor reuse, exact encoding and allocation rollback passed\n";
  } catch (std::exception const & error) {
    fail_after = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
