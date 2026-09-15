/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures complete incremental borrowed-profile construction with exact wire and key oracles.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include "policy_compat.h"

#include <diet/profile.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
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
  template <class P> using array_type = profile_array<P, stream_role::borrowed>;
  template <class P> std::uint64_t fingerprint(array_type<P> const & array) {
    digest d;
    auto const & m = array.metadata();
    d.integer(m.record_count); d.integer(m.extent); d.integer(m.terminal_key_units);
    d.integer(m.common_value_width.has_value()); d.integer(m.common_value_width.value_or(0));
    d.integer(array.bytes().size()); for (auto b : array.bytes()) d.byte(std::to_integer<unsigned>(b));
    auto const & ef = array.group_offsets();
    d.words(ef.low); d.words(ef.high); d.words(ef.sparse);
    d.integer(m.record_count); d.integer(ef.universe); d.integer(ef.low_width); d.integer(ef.samples.size());
    for (auto sample : ef.samples) { d.integer(sample.first); d.integer(sample.sparse); }
    return d.value;
  }
  template <class P> void verify(array_type<P> const & array, array_type<P> const & expected,
      std::span<profile_record const> records) {
    require(fingerprint(array) == fingerprint(expected), "writer wire digest differs from batch");
    require(std::equal(array.bytes().begin(), array.bytes().end(), expected.bytes().begin(), expected.bytes().end()),
      "writer payload differs from batch");
    auto const & a = array.metadata(); auto const & b = expected.metadata();
    require(a.record_count == b.record_count && a.extent == b.extent &&
      a.common_value_width == b.common_value_width && a.terminal_key_units == b.terminal_key_units,
      "writer metadata differs from batch");
    auto const & x = array.group_offsets(); auto const & y = expected.group_offsets();
    require(x.low == y.low && x.high == y.high && x.sparse == y.sparse && x.low_width == y.low_width &&
      x.universe == y.universe && x.samples.size() == y.samples.size(),
      "writer EF differs from batch");
    for (std::size_t i = 0; i != x.samples.size(); ++i)
      require(x.samples[i].first == y.samples[i].first && x.samples[i].sparse == y.samples[i].sparse,
        "writer EF sample differs from batch");
    auto cursor = array.view().cursor();
    for (auto const & record : records) {
      require(!cursor.done(), "writer cursor ended early");
      auto key = cursor.peek().key.prefix;
      require(key.offset() == 0 && key.size() == record.key.bit_size && key.storage().size() == record.key.bytes.size(),
        "writer reconstructed key shape");
      require(std::equal(key.storage().begin(), key.storage().end(), record.key.bytes.begin()),
        "writer reconstructed key content");
      cursor.advance();
    }
    require(cursor.done(), "writer cursor has extra records");
  }
  template <class P> void run(char const * name, unsigned count, unsigned prefix, unsigned rounds) {
    std::vector<profile_record> records;
    records.reserve(count);
    for (std::uint64_t i = 0; i != count; ++i) {
      std::string bytes(prefix, 'p');
      // Fixed-width integer order is independent of the comparison being timed.
      for (unsigned j = 8; j; --j) bytes.push_back(char(i >> ((j - 1) * 8)));
      auto key = bit_string::from_bytes(bytes);
      if constexpr (P::unit == profile_unit::bit) {
        key.bytes.push_back((i & 1) ? std::byte{0xa0} : std::byte{0x40});
        key.bit_size += 3;
      }
      records.push_back({std::move(key), {}});
    }
    auto expected = array_type<P>::build(records);
    auto build = [&] {
      profile_borrowed_writer<P> writer;
      for (auto const & record : records) writer.append(record.key.view());
      return writer.finish();
    };
    auto warm = build(); verify(warm, expected, records);
    for (unsigned round = 0; round != rounds; ++round) {
      auto start = clock_type::now();
      auto result = build();
      auto elapsed = std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
      verify(result, expected, records);
      std::cout << name << ',' << count << ',' << prefix << ',' << round << ',' << elapsed << ','
        << elapsed / count << ',' << result.bytes().size() << ',' << fingerprint(result) << '\n';
    }
  }
}

int main(int argc, char ** argv) {
  try {
    auto count = argc > 1 ? unsigned(std::stoul(argv[1])) : 16384;
    auto prefix = argc > 2 ? unsigned(std::stoul(argv[2])) : 64;
    auto rounds = argc > 3 ? unsigned(std::stoul(argv[3])) : 3;
    require(count && rounds, "positive count and rounds required");
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    std::cout << std::fixed << std::setprecision(3)
      << "profile,records,prefix_bytes,round,build_ns,record_ns,payload_bytes,wire_digest\n";
    run<diet_bench::policy<profile_unit::byte>>("byte", count, prefix, rounds);
    run<diet_bench::policy<profile_unit::bit>>("bit", count, prefix, rounds);
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
