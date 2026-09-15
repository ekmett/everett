/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures bounded key and bit primitives against identical profile fixtures.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/profile.h>
#if __has_include(<diet/front.h>)
#include <diet/front.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace {
  using clock_type = std::chrono::steady_clock;
  using bit_policy = diet::storage_policy<diet::profile_unit::bit>;
  using byte_policy = diet::storage_policy<diet::profile_unit::byte>;
  void require(bool condition) { if (!condition) throw std::runtime_error("key/bit benchmark oracle"); }
  std::uint64_t mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
  }
  unsigned bit(std::span<std::byte const> bytes, std::uint64_t at) {
    return (std::to_integer<unsigned>(bytes[at / 8]) >> (7 - at % 8)) & 1;
  }
  void put(std::span<std::byte> bytes, std::uint64_t at, unsigned value) {
    auto mask = std::byte(1u << (7 - at % 8));
    if (value) bytes[at / 8] |= mask; else bytes[at / 8] &= ~mask;
  }
  std::uint64_t digest(std::span<std::byte const> bytes) {
    std::uint64_t value = 0xcbf29ce484222325ull;
    for (auto byte : bytes) value = (value ^ std::to_integer<unsigned>(byte)) * 0x100000001b3ull;
    return value;
  }
  void row(std::string const & operation, std::uint64_t bits, unsigned offset, std::uint64_t iterations,
      clock_type::time_point start, clock_type::time_point stop, std::uint64_t checksum, std::uint64_t encoding) {
    auto ns = std::chrono::duration<double, std::nano>(stop - start).count() / double(iterations);
    std::cout << operation << ',' << bits << ',' << offset << ',' << iterations << ',' << ns << ',' << checksum << ',' << encoding << '\n';
  }
  // Keep historical fixture revisions usable with their original byte helpers.
  // Current headers express the same request through the typed profile API.
  int compare_text(std::string_view a, std::string_view b) {
#if __has_include(<diet/front.h>)
    return diet::compare_keys(a, b);
#else
    return diet::compare_bits(
      diet::bit_view(std::as_bytes(std::span(a.data(), a.size())), a.size() * 8),
      diet::bit_view(std::as_bytes(std::span(b.data(), b.size())), b.size() * 8));
#endif
  }
  std::uint64_t prefix_text(std::string_view a, std::string_view b) {
#if __has_include(<diet/front.h>)
    return diet::common_prefix(a, b);
#else
    return diet::common_prefix_units<byte_policy>(
      diet::bit_view(std::as_bytes(std::span(a.data(), a.size())), a.size() * 8),
      diet::bit_view(std::as_bytes(std::span(b.data(), b.size())), b.size() * 8));
#endif
  }
  void primitive(unsigned bytes, unsigned offset, std::uint64_t work) {
    auto count = std::uint64_t(bytes) * 8 + (offset ? 1 : 0);
    auto iterations = std::max<std::uint64_t>(512, work / count);
    std::array<diet::bit_string, 16> sources, others;
    std::array<diet::bit_view, 16> views, alternate;
    for (unsigned k = 0; k != 16; ++k) {
      sources[k].bit_size = count + offset;
      sources[k].bytes.resize(std::size_t((count + offset + 7) / 8));
      for (std::size_t i = 0; i != sources[k].bytes.size(); ++i) sources[k].bytes[i] = std::byte(mix(i + 71 * k));
      if (sources[k].bit_size % 8) sources[k].bytes.back() &= std::byte(255u << (8 - sources[k].bit_size % 8));
      others[k] = sources[k];
      put(others[k].bytes, offset + count - 1, 1 - bit(others[k].bytes, offset + count - 1));
      views[k] = sources[k].view().subview(offset, count);
      alternate[k] = others[k].view().subview(offset, count);
      auto expected_order = bit(sources[k].bytes, offset + count - 1) ? 1 : -1;
      require(diet::compare_bits(views[k], alternate[k]) == expected_order);
      require(diet::common_prefix_units<bit_policy>(views[k], alternate[k]) == count - 1);
    }
    std::uint64_t checksum = 0;
    auto start = clock_type::now();
    for (std::uint64_t i = 0; i != iterations; ++i) {
      auto k = unsigned(i & 15);
      checksum += diet::compare_bits(views[k], alternate[k]) + 1;
      checksum += diet::common_prefix_units<bit_policy>(views[k], alternate[k]);
    }
    auto end = clock_type::now();
    row("order_and_lcp", count, offset, iterations, start, end, checksum, 0);
    for (unsigned k = 0; k != 16; ++k) {
      others[k] = sources[k];
      put(others[k].bytes, offset, 1 - bit(others[k].bytes, offset));
      alternate[k] = others[k].view().subview(offset, count);
    }
    auto early_iterations = std::max<std::uint64_t>(32768, iterations);
    checksum = 0; start = clock_type::now();
    for (std::uint64_t i = 0; i != early_iterations; ++i) {
      auto k = unsigned(i & 15);
      checksum += diet::compare_bits(views[k], alternate[k]) + 1;
      checksum += diet::common_prefix_units<bit_policy>(views[k], alternate[k]);
    }
    end = clock_type::now();
    row("order_and_lcp_early", count, offset, early_iterations, start, end, checksum, 0);
    for (unsigned k = 0; k != 16; ++k) {
      others[k] = sources[k];
      put(others[k].bytes, offset + count - 1, 1 - bit(others[k].bytes, offset + count - 1));
    }
    diet::bit_string target;
    target.bit_size = count + offset + 7;
    target.bytes.resize(std::size_t((target.bit_size + 7) / 8));
    for (unsigned k = 0; k != 16; ++k) {
      diet::profile_detail::copy_into(target, offset, views[k]);
      for (std::uint64_t i = 0; i != count; ++i) require(bit(target.bytes, offset + i) == bit(sources[k].bytes, offset + i));
    }
    checksum = 0; start = clock_type::now();
    for (std::uint64_t i = 0; i != iterations; ++i) {
      diet::profile_detail::copy_into(target, 0, views[unsigned(i & 15)]);
      checksum += std::to_integer<unsigned>(target.bytes[(i >> 4) % target.bytes.size()]);
    }
    end = clock_type::now();
    row("copy_to_aligned", count, offset, iterations, start, end, checksum, digest(target.bytes));
    checksum = 0; start = clock_type::now();
    for (std::uint64_t i = 0; i != iterations; ++i) {
      diet::profile_detail::copy_into(target, 3, views[unsigned(i & 15)]);
      checksum += std::to_integer<unsigned>(target.bytes[(i >> 4) % target.bytes.size()]);
    }
    end = clock_type::now();
    row("copy_to_shifted", count, offset, iterations, start, end, checksum, digest(target.bytes));
    if (!offset) {
      std::array<std::string_view, 16> text, other_text;
      for (unsigned k = 0; k != 16; ++k) {
        text[k] = {reinterpret_cast<char const *>(sources[k].bytes.data()), bytes};
        other_text[k] = {reinterpret_cast<char const *>(others[k].bytes.data()), bytes};
      }
      checksum = 0; start = clock_type::now();
      for (std::uint64_t i = 0; i != iterations; ++i) {
        auto k = unsigned(i & 15);
        checksum += compare_text(text[k], other_text[k]) + 1;
        checksum += prefix_text(text[k], other_text[k]);
      }
      end = clock_type::now();
      row("front_order_and_lcp", count, offset, iterations, start, end, checksum, 0);
    }
  }
  void counts(unsigned width, unsigned offset, std::uint64_t work) {
    std::array<std::uint64_t, 256> values;
    diet::bit_string encoded;
    encoded.bit_size = offset; encoded.bytes.resize(offset != 0);
    std::vector<std::uint64_t> positions;
    for (unsigned i = 0; i != values.size(); ++i) {
      values[i] = width == 64 ? mix(i) : mix(i) & ((std::uint64_t{1} << width) - 1);
      if (width == 64 && i == 0) values[i] = ~std::uint64_t{0};
      positions.push_back(encoded.bit_size);
      diet::profile_detail::write_count<bit_policy>(encoded, values[i]);
    }
    for (unsigned i = 0; i != values.size(); ++i) {
      auto at = positions[i];
      require(diet::profile_detail::read_count<bit_policy>(encoded.view(), at) == values[i]);
      require(at == (i + 1 == values.size() ? encoded.bit_size : positions[i + 1]));
    }
    auto iterations = std::max<std::uint64_t>(1024, work / (2 * width + 1));
    std::uint64_t checksum = 0;
    auto data = encoded.view();
    auto start = clock_type::now();
    for (std::uint64_t i = 0; i != iterations; ++i) {
      auto at = positions[i & 255];
      checksum += diet::profile_detail::read_count<bit_policy>(data, at);
    }
    auto end = clock_type::now();
    row("read_count", width, offset, iterations, start, end, checksum, digest(encoded.bytes));
    diet::bit_string out;
    out.bytes.reserve(std::size_t(iterations * (2 * width + 3) / 8 + 32));
    out.bit_size = offset; out.bytes.resize(offset != 0);
    start = clock_type::now();
    for (std::uint64_t i = 0; i != iterations; ++i)
      diet::profile_detail::write_count<bit_policy>(out, values[i & 255]);
    auto stop = clock_type::now();
    auto ns = std::chrono::duration<double, std::nano>(stop - start).count() / double(iterations);
    std::cout << "write_count," << width << ',' << offset << ',' << iterations << ',' << ns << ',' << out.bit_size << ',' << digest(out.bytes) << '\n';
  }
  template <class P> void profiles(char const * name, unsigned prefix, std::uint64_t work) {
    std::vector<diet::profile_record> records;
    for (unsigned i = 0; i != 1024; ++i) {
      std::string key(prefix, 'x');
      for (unsigned j = 4; j; --j) key += char((i >> (8 * (j - 1))) & 255);
      auto packed = diet::bit_string::from_bytes(key);
      if constexpr (P::unit == diet::profile_unit::bit) {
        packed.bytes.push_back((i & 1) ? std::byte{128} : std::byte{0}); ++packed.bit_size;
      }
      records.push_back({packed, diet::bit_string::from_bytes("value")});
    }
    auto source = diet::profile_array<P>::build(records, {}, 18);
    auto view = source.view();
    for (unsigned i = 0; i != 1024; ++i) {
      auto got = view.reconstruct_at(i, records[i].key.bit_size / P::bits_per_unit);
      require(got.prefix == records[i].key && got.value == records[i].value);
    }
    auto iterations = std::max<std::uint64_t>(2, work / (1024 * (prefix + 4) * 8));
    std::uint64_t checksum = 0;
    auto start = clock_type::now();
    for (std::uint64_t i = 0; i != iterations; ++i) {
      auto result = diet::profile_array<P>::build(records, {}, 18);
      checksum += result.bytes().size() + std::to_integer<unsigned>(result.bytes().back());
    }
    auto end = clock_type::now();
    row(std::string(name) + "_build", prefix * 8, 0, iterations, start, end, checksum, digest(source.bytes()));
    iterations = std::max<std::uint64_t>(512, work / ((prefix + 4) * 8));
    checksum = 0; start = clock_type::now();
    for (std::uint64_t i = 0; i != iterations; ++i) {
      auto ordinal = mix(i) % 1024;
      auto result = view.reconstruct_at(ordinal, records[ordinal].key.bit_size / P::bits_per_unit);
      checksum += std::to_integer<unsigned>(result.prefix.bytes.back());
    }
    end = clock_type::now();
    row(std::string(name) + "_reconstruct", prefix * 8, 0, iterations, start, end, checksum, digest(source.bytes()));
  }
}
int main(int argc, char ** argv) {
  try {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    auto work = argc > 1 ? std::stoull(argv[1]) : 8388608ull;
    std::cout << "operation,bits,source_offset,iterations,ns,checksum,encoding\n";
    for (unsigned bytes : {8u,64u,4096u}) for (unsigned offset : {0u,3u}) primitive(bytes, offset, work);
    for (unsigned width : {4u,32u,64u}) for (unsigned offset : {0u,3u}) counts(width, offset, work);
    for (unsigned prefix : {8u,128u}) { profiles<byte_policy>("byte_profile", prefix, work); profiles<bit_policy>("bit_profile", prefix, work); }
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
