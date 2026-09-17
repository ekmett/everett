/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Extracts actual native and fractional-index offset sequences.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once
#include "../host_backend.h"
#include "../fixed_kv_compare/cases.h"
#include <everett/cola_index.h>
#include <everett/sort_profile.h>
#include <everett/typed_world.h>
#include <fstream>

namespace select_fixture {
  using u64 = std::uint64_t;
  using strings = everett::unsorted<std::optional<std::string>>;
  using typed_bits = everett_experiment::policy<everett::bin<everett::tip<strings>, everett::sort_undefined>>;
  template <bool Byte, bool Fixed> using raw_policy = everett_experiment::policy<everett::tip<everett::encoded_sort<
    std::conditional_t<Byte, everett::byte_encoding<std::conditional_t<Fixed, everett::fixed_values<16>, everett::variable_values>>,
    everett::bit_encoding<std::conditional_t<Fixed, everett::fixed_values<128>, everett::variable_values>>>>>>;
  inline void require(bool value, char const *message) { if (!value) throw std::runtime_error(message); }
  inline void number(std::ostream &out, u64 value) { for (unsigned b = 0; b < 8; ++b) out.put(char(value >> (b << 3))); }
  inline u64 number(std::istream &in) {
    u64 value = 0;
    for (unsigned b = 0; b < 8; ++b) { auto c = in.get(); require(c != EOF, "truncated sequence"); value |= u64(c) << (b << 3); }
    return value;
  }
  inline void save(std::filesystem::path const &path, std::span<u64 const> values) {
    std::ofstream out(path, std::ios::binary); number(out, values.size()); number(out, values.empty() ? 0 : values.back());
    for (auto n : values) number(out, n);
    require(bool(out), "write sequence");
  }
  inline std::vector<u64> load(std::filesystem::path const &path) {
    std::ifstream in(path, std::ios::binary); auto count = number(in), universe = number(in);
    require(count <= (std::filesystem::file_size(path) - 16) / 8, "invalid sequence extent");
    std::vector<u64> result(count); for (auto &n : result) n = number(in);
    require(in.peek() == EOF && (result.empty() ? !universe : result.back() == universe), "sequence trailing data");
    require(everett::elias_fano_detail::monotone(result), "unordered sequence"); return result;
  }
  template <class Native> void extract(std::filesystem::path const &root, std::string const &name, Native const &native) {
    auto view = native.view(); auto offsets = view.group_offsets();
    std::vector<u64> values; values.reserve(offsets.size());
    for (u64 i = 0; i < offsets.size(); ++i) values.push_back(offsets.select(i));
    auto rebuilt = everett::elias_fano::build<everett_experiment::architecture>(values);
    require(rebuilt.low_width == offsets.low_width() && rebuilt.universe == offsets.universe(), "extracted EF metadata");
    require(rebuilt.low.size() == offsets.low_words().size() && rebuilt.high.size() == offsets.high_words().size(), "extracted EF shape");
    for (u64 i = 0; i < rebuilt.low.size(); ++i) require(rebuilt.low[i] == offsets.low_words()[i], "extracted low mismatch");
    for (u64 i = 0; i < rebuilt.high.size(); ++i) require(rebuilt.high[i] == offsets.high_words()[i], "extracted high mismatch");
    require(rebuilt.samples.size() == offsets.samples().size() && rebuilt.sparse.size() == offsets.sparse_words().size(), "extracted index shape");
    for (u64 i = 0; i < rebuilt.samples.size(); ++i) {
      auto expected = offsets.samples()[i];
      require(rebuilt.samples[i].first == expected.first && rebuilt.samples[i].sparse == expected.sparse, "extracted sample mismatch");
    }
    for (u64 i = 0; i < rebuilt.sparse.size(); ++i) require(rebuilt.sparse[i] == offsets.sparse_words()[i], "extracted sparse mismatch");
    save(root / (name + ".seq"), values);
    auto meta = view.metadata();
    std::cout << name << ',' << meta.record_count << ',' << unsigned(meta.offset_unit) << ',' << meta.extent << ','
      << meta.common_value_width.value_or(0) << ',' << values.size() << ',' << offsets.universe() << ',' << offsets.low_width() << ','
      << (offsets.low_words().size() + offsets.high_words().size() + offsets.sparse_words().size()) * 8 + offsets.samples().size() * 16 << '\n';
  }
  template <class P, class Native> void chain(std::filesystem::path const &root, std::string const &name,
      std::shared_ptr<Native const> a, std::shared_ptr<Native const> b, std::shared_ptr<Native const> combined) {
    extract(root, name + ".native-a", *a); extract(root, name + ".native-output", *combined);
    using index = everett::cola_index<P, Native>;
    auto terminal = std::make_shared<index const>(index::adopt_native(combined));
    auto middle = std::make_shared<index const>(index::adopt_native(b, terminal));
    auto outer = index::adopt_native(a, middle, combined);
    extract(root, name + ".index-main", outer.borrowed(0));
    extract(root, name + ".index-secondary", outer.borrowed(1));
  }
  inline std::string value_string(fixed_fixture::record const &row) {
    return row.value.empty() ? std::string{} : std::string(reinterpret_cast<char const *>(row.value.data()), row.value.size());
  }
  template <class P, bool Typed> void raw(std::filesystem::path const &root, std::string const &name,
      fixed_fixture::fixture const &fixture) {
    using native = everett::profile_array<P>;
    auto encode = [&](auto const &rows) {
      everett::profile_native_writer<P> writer;
      for (auto const &row : rows) {
        auto key = matched_fixture::key_bytes(row.k);
        if constexpr (Typed) writer.append(everett::profile_record{everett::typed_detail::key<P, strings>(key),
          everett::typed_detail::value<P, strings>(std::optional<std::string>(value_string(row)))});
        else writer.append(everett::profile_record{everett::bit_string::from_bytes(key), everett::bit_string::from_bytes(std::span(row.value))});
      }
      return std::make_shared<native const>(writer.finish());
    };
    chain<P>(root, name, encode(fixture.a), encode(fixture.b), encode(matched_fixture::merged(fixture)));
  }
  inline void typed_bit(std::filesystem::path const &root, std::string const &name, fixed_fixture::fixture const &fixture) {
    using native = everett::sort_profile_array<typed_bits>;
    auto encode = [&](auto const &rows) {
      everett::sort_profile_writer<typed_bits> writer;
      for (auto const &row : rows) writer.append<strings>(matched_fixture::key_bytes(row.k), value_string(row));
      return std::make_shared<native const>(writer.finish());
    };
    chain<typed_bits>(root, name, encode(fixture.a), encode(fixture.b), encode(matched_fixture::merged(fixture)));
  }
  inline void export_case(std::filesystem::path const &root, unsigned index) {
    std::filesystem::create_directories(root);
    auto fixture = matched_fixture::make(index); auto name = "case-" + std::to_string(index);
    std::cout << "sequence,records,offset_unit,extent,common_value_width,offset_count,universe,low_width,ef_array_bytes\n";
    if (matched_fixture::cases[index].variable) {
      raw<raw_policy<true, false>, false>(root, name + ".raw-byte", fixture);
      raw<raw_policy<false, false>, false>(root, name + ".raw-bit", fixture);
    } else {
      raw<raw_policy<true, true>, false>(root, name + ".raw-byte", fixture);
      raw<raw_policy<false, true>, false>(root, name + ".raw-bit", fixture);
    }
    raw<everett_experiment::policy<>, true>(root, name + ".typed-byte", fixture);
    typed_bit(root, name + ".typed-bit", fixture);
  }
  inline std::vector<u64> replay(std::span<u64 const> source, u64 count) {
    require(source.size() > 1, "replay needs real gaps");
    std::vector<u64> result(count);
    for (u64 i = 1; i < count; ++i) {
      auto j = 1 + ((i - 1) % (source.size() - 1));
      require(source[j] - source[j - 1] <= std::numeric_limits<u64>::max() - result[i - 1], "replay overflow");
      result[i] = result[i - 1] + source[j] - source[j - 1];
    }
    return result;
  }
}
