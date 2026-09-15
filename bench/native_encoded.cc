/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/native_merge.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace allocation_probe {
  struct totals { std::uint64_t requested = 0, peak = 0, live = 0, calls = 0; };
#if defined(DIET_BENCH_ALLOCATIONS)
  thread_local totals counts;
  thread_local std::uint64_t epoch = 0;
  thread_local bool active = false;
  struct header { void * raw; std::size_t size; std::uint64_t epoch; };
  void begin() { ++epoch; counts = {}; active = true; }
  totals end() { active = false; return counts; }
  void * allocate(std::size_t size, std::size_t alignment) {
    alignment = std::max(alignment, alignof(header));
    auto overhead = sizeof(header) + alignment - 1;
    auto extent = std::max(size, std::size_t{1});
    if (extent > std::numeric_limits<std::size_t>::max() - overhead) throw std::bad_alloc();
    auto raw = std::malloc(extent + overhead);
    if (!raw) throw std::bad_alloc();
    auto address = (reinterpret_cast<std::uintptr_t>(raw) + overhead) & ~(alignment - 1);
    ::new (reinterpret_cast<void *>(address - sizeof(header))) header{raw, size, active ? epoch : 0};
    if (active) {
      counts.requested += size; counts.live += size; ++counts.calls;
      counts.peak = std::max(counts.peak, counts.live);
    }
    return reinterpret_cast<void *>(address);
  }
  void release(void * pointer) noexcept {
    if (!pointer) return;
    auto stored = reinterpret_cast<header *>(reinterpret_cast<std::uintptr_t>(pointer) - sizeof(header));
    if (stored->epoch && stored->epoch == epoch) counts.live -= stored->size;
    std::free(stored->raw);
  }
#else
  void begin() {}
  totals end() { return {}; }
#endif
}
#if defined(DIET_BENCH_ALLOCATIONS)
void * operator new(std::size_t size) { return allocation_probe::allocate(size, alignof(std::max_align_t)); }
void * operator new[](std::size_t size) { return allocation_probe::allocate(size, alignof(std::max_align_t)); }
void * operator new(std::size_t size, std::align_val_t align) { return allocation_probe::allocate(size, std::size_t(align)); }
void * operator new[](std::size_t size, std::align_val_t align) { return allocation_probe::allocate(size, std::size_t(align)); }
void operator delete(void * p) noexcept { allocation_probe::release(p); }
void operator delete[](void * p) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::size_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::size_t) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::size_t, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::size_t, std::align_val_t) noexcept { allocation_probe::release(p); }
#endif

namespace {
  using namespace diet;
  using clock_type = std::chrono::steady_clock;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  struct digest {
    std::uint64_t value = 0xcbf29ce484222325ull;
    void byte(unsigned v) { value = (value ^ v) * 0x100000001b3ull; }
    void integer(std::uint64_t v) { for (unsigned i = 0; i != 8; ++i) byte(unsigned(v >> (i * 8)) & 255); }
    template <class T> void words(T const & values) { integer(values.size()); for (auto v : values) integer(v); }
  };
  template <class P> using array_type = profile_array<P>;
  template <class P> std::uint64_t fingerprint(array_type<P> const & array) {
    digest d;
    auto const & m = array.metadata();
    d.integer(m.record_count); d.integer(m.extent); d.integer(m.terminal_key_units);
    d.integer(m.common_value_width.has_value()); d.integer(m.common_value_width.value_or(0));
    d.integer(array.bytes().size()); for (auto b : array.bytes()) d.byte(std::to_integer<unsigned>(b));
    auto const & ef = array.group_offsets();
    d.words(ef.low); d.words(ef.high); d.words(ef.sparse);
    d.integer(ef.entry_count); d.integer(ef.universe); d.integer(ef.low_width); d.integer(ef.samples.size());
    for (auto sample : ef.samples) { d.integer(sample.first); d.integer(sample.sparse); }
    return d.value;
  }
  template <class P> void verify(array_type<P> const & array, array_type<P> const & expected,
      std::span<profile_record const> records) {
    require(fingerprint(array) == fingerprint(expected), "merge wire digest differs from batch");
    require(std::equal(array.bytes().begin(), array.bytes().end(), expected.bytes().begin(), expected.bytes().end()),
      "merge payload differs from batch");
    auto const & a = array.metadata(); auto const & b = expected.metadata();
    require(a.record_count == b.record_count && a.extent == b.extent &&
      a.common_value_width == b.common_value_width && a.terminal_key_units == b.terminal_key_units,
      "merge metadata differs from batch");
    auto const & x = array.group_offsets(); auto const & y = expected.group_offsets();
    require(x.low == y.low && x.high == y.high && x.sparse == y.sparse && x.entry_count == y.entry_count && x.low_width == y.low_width &&
      x.universe == y.universe && x.samples.size() == y.samples.size(),
      "merge EF differs from batch");
    for (std::size_t i = 0; i != x.samples.size(); ++i)
      require(x.samples[i].first == y.samples[i].first && x.samples[i].sparse == y.samples[i].sparse,
        "merge EF sample differs from batch");
    auto cursor = array.view().cursor();
    for (auto const & record : records) {
      require(!cursor.done(), "merge cursor ended early");
      auto key = cursor.peek().key.prefix;
      require(key.offset() == 0 && key.size() == record.key.bit_size && key.storage().size() == record.key.bytes.size(),
        "merge reconstructed key shape");
      require(std::equal(key.storage().begin(), key.storage().end(), record.key.bytes.begin()),
        "merge reconstructed key content");
      auto value = cursor.peek().value;
      require(value.size() == record.value.bit_size, "merge reconstructed value length");
      for (std::uint64_t i = 0; i != value.size(); ++i) {
        auto expected_bit = (std::to_integer<unsigned>(record.value.bytes[i / 8]) >> (7 - i % 8)) & 1;
        require(value.at(i) == bool(expected_bit), "merge reconstructed value content");
      }
      cursor.advance();
    }
    require(cursor.done(), "merge cursor has extra records");
  }
  template <class P> void run(char const * name, unsigned count, unsigned prefix, unsigned rounds, std::string const & fixture, unsigned tail) {
    std::vector<profile_record> records;
    records.reserve(count);
    for (std::uint64_t i = 0; i != count; ++i) {
      bit_string key;
      if (fixture == "fragments") {
        auto depth = (count + 1) / 2;
        auto units = i < depth ? i + 1 : count - i;
        key.bit_size = units << P::unit_shift;
        key.bytes.resize(static_cast<std::size_t>((key.bit_size + 7) >> 3));
        if (i >= depth) {
          auto at = key.bit_size - 1;
          key.bytes[at >> 3] |= std::byte(1u << (7 - (at & 7)));
        }
      } else {
        std::string bytes(prefix, 'p');
        // Fixed-width integer order is independent of the comparison being timed.
        for (unsigned j = 8; j; --j) bytes.push_back(char(i >> ((j - 1) << 3)));
        bytes.append(tail, char(0x5a ^ (i & 255)));
        key = bit_string::from_bytes(bytes);
        if constexpr (P::unit == profile_unit::bit) {
          key.bytes.push_back((i & 1) ? std::byte{0xa0} : std::byte{0x40});
          key.bit_size += 3;
        }
      }
      auto units = P::value_width.value_or(8 + i % 17);
      auto bit_count = units * P::bits_per_unit;
      bit_string value;
      value.bit_size = bit_count;
      value.bytes.resize(static_cast<std::size_t>((bit_count + 7) / 8));
      for (std::uint64_t j = 0; j != bit_count; ++j)
        if ((i + j) % 3) value.bytes[j / 8] |= std::byte(1u << (7 - j % 8));
      records.push_back({std::move(key), std::move(value)});
    }
    std::vector<profile_record> older_records, newer_records;
    for (std::size_t i = 0; i != records.size(); ++i) {
      if (i % 3 != 1) {
        auto old = records[i];
        // Overlapping keys have distinguishable older values. The newer
        // value wins according to the fixture, independently of merge order.
        if (i % 3 == 2) old.value.bytes.front() ^= std::byte{0x80};
        older_records.push_back(std::move(old));
      }
      if (i % 3 != 0) newer_records.push_back(records[i]);
    }
    auto expected = array_type<P>::build(records);
    auto older = std::make_shared<array_type<P> const>(array_type<P>::build(older_records));
    auto newer = std::make_shared<array_type<P> const>(array_type<P>::build(newer_records));
    auto literals = [](array_type<P> const & source) {
      std::uint64_t bits = 0;
      auto cursor = source.view().encoded_cursor();
      while (!cursor.done()) { bits += cursor.peek().suffix.size(); cursor.advance(); }
      return bits;
    };
    auto input_literals = literals(*older) + literals(*newer);
    auto output_literals = literals(expected);
    auto input_encoded_bytes = older->bytes().size() + newer->bytes().size();
    auto build = [&] {
      native_merge_builder<P> merger(older, newer);
      while (!merger.done()) merger.step(128);
      return merger.finish();
    };
    auto warm = build(); verify(warm, expected, records);
    for (unsigned round = 0; round != rounds; ++round) {
      allocation_probe::begin();
      auto start = clock_type::now();
      auto result = build();
      auto elapsed = std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
      auto allocations = allocation_probe::end();
      verify(result, expected, records);
      std::cout << name << ',' << fixture << ',' << count << ',' << prefix << ',' << tail << ',' << round << ',' << elapsed << ','
        << elapsed / count << ',' << result.bytes().size() << ',' << fingerprint(result) << ','
        << input_literals << ',' << output_literals << ',' << input_encoded_bytes << ','
        << allocations.requested << ',' << allocations.peak << ',' << allocations.live << ',' << allocations.calls << '\n';
    }
  }
}

int main(int argc, char ** argv) {
  try {
    auto count = argc > 1 ? unsigned(std::stoul(argv[1])) : 16384;
    auto prefix = argc > 2 ? unsigned(std::stoul(argv[2])) : 64;
    auto rounds = argc > 3 ? unsigned(std::stoul(argv[3])) : 3;
    require(count && rounds, "positive count and rounds required");
#if defined(DIET_BENCH_ALLOCATIONS)
    allocation_probe::begin();
    auto probe = ::operator new(33, std::align_val_t{64});
    require((reinterpret_cast<std::uintptr_t>(probe) & 63) == 0, "allocation probe alignment");
    ::operator delete(probe, std::align_val_t{64});
    auto measured = allocation_probe::end();
    require(measured.requested == 33 && measured.peak == 33 && measured.live == 0 && measured.calls == 1,
            "allocation probe accounting");
#endif
    std::string fixture = argc > 4 ? argv[4] : "ids";
    require(fixture == "ids" || fixture == "fragments", "unknown fixture");
    auto tail = argc > 5 ? unsigned(std::stoul(argv[5])) : 0;
    require(fixture != "fragments" || (prefix == 0 && tail == 0), "fragment fixture has no added prefix or tail");
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    std::cout << std::fixed << std::setprecision(3)
      << "profile,fixture,records,prefix_bytes,tail_bytes,round,build_ns,record_ns,payload_bytes,wire_digest,input_literal_bits,output_literal_bits,input_encoded_bytes,requested_bytes,peak_bytes,live_bytes,allocation_calls\n";
    run<storage_policy<profile_unit::byte, fixed_values<8>>>("byte_fixed", count, prefix, rounds, fixture, tail);
    run<storage_policy<profile_unit::byte>>("byte_variable", count, prefix, rounds, fixture, tail);
    run<storage_policy<profile_unit::bit, fixed_values<13>>>("bit_fixed", count, prefix, rounds, fixture, tail);
    run<storage_policy<profile_unit::bit>>("bit_variable", count, prefix, rounds, fixture, tail);
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures encoded native merges, fragment stacks and requested allocation sizes.
 */
