/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares complete byte/bit native and fractional-index file space.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sort_profile_file.h>
#include <everett/typed_world.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>

namespace {
  using namespace everett;
  using u64 = std::uint64_t;
  using strings = unsorted<std::optional<std::string>>;
  void require(bool value, char const *message) { if (!value) throw std::runtime_error(message); }
  u64 mix(u64 n) {
    n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ULL;
    n ^= n >> 27; n *= 0x94d049bb133111ebULL; return n ^ (n >> 31);
  }
  std::string big(u64 n) {
    std::string out(8, '\0');
    for (unsigned i = 0; i < 8; ++i) out[i] = char(n >> (56 - 8 * i));
    return out;
  }
  u64 unbig(std::string const &s) {
    require(s.size() == 8, "integer fixture width");
    u64 n = 0; for (unsigned char c : s) n = (n << 8) | c; return n;
  }
  struct row { std::string key, value; };
  struct fixture { std::string name; bool integer, variable; std::vector<row> rows; };
  fixture make(unsigned shape, bool variable, u64 count) {
    fixture result{{}, shape < 2, variable, {}};
    result.name = std::array{"integer-ordered", "integer-random", "string-structured", "string-binary", "string-prefix"}[shape];
    result.name += variable ? "-variable" : "-fixed";
    result.rows.reserve(count);
    for (u64 i = 0; i < count; ++i) {
      std::string key;
      if (shape < 2) key = big(shape ? mix(i + 1) : i);
      else if (shape == 2 || shape == 4) {
        auto number = std::to_string(i);
        key = "tenant/000042/object/" + std::string(12 - number.size(), '0') + number;
        if (shape == 2) key += "/state";
      } else key = big(mix(i + 1)) + big(mix(i + 0x123456789abcdef0ULL));
      u64 length = variable ? mix(i + 0x76543210) % 513 : shape < 2 ? 8 : 16;
      std::string value(length, '\0');
      for (u64 j = 0; j < length; ++j) value[j] = char(mix(i * 513 + j + 1) >> 56);
      result.rows.push_back({std::move(key), std::move(value)});
    }
    std::sort(result.rows.begin(), result.rows.end(), [](auto const &a, auto const &b) { return a.key < b.key; });
    for (u64 i = 1; i < count; ++i) require(result.rows[i - 1].key < result.rows[i].key, "fixture unique keys");
    return result;
  }
  void write(std::filesystem::path const &path, std::span<std::byte const> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
    out.close();
    require(bool(out), "write fixture file");
    require(std::filesystem::file_size(path) == bytes.size(), "complete serialized file size");
  }
  void logical(std::filesystem::path const &path, fixture const &f) {
    std::ofstream out(path, std::ios::binary);
    auto number = [&](u64 n) { for (unsigned i = 0; i < 8; ++i) out.put(char(n >> (8 * i))); };
    number(f.rows.size());
    for (auto const &r : f.rows) { number(r.key.size()); out.write(r.key.data(), r.key.size()); number(r.value.size()); out.write(r.value.data(), r.value.size()); }
    require(bool(out), "write logical oracle");
  }
  template <bool Fixed> struct integer_sort {
    using encoding = byte_encoding<std::conditional_t<Fixed, fixed_values<8>, variable_values>>;
    using key_codec = unsigned_key<64>;
    using value_codec = std::conditional_t<Fixed, unsigned_value<64>, string_value<>>;
  };
  template <bool Byte, bool Fixed, unsigned Width> using raw_policy = storage_policy<tip<encoded_sort<
    std::conditional_t<Byte, byte_encoding<std::conditional_t<Fixed, fixed_values<Width>, variable_values>>,
      bit_encoding<std::conditional_t<Fixed, fixed_values<Width * 8>, variable_values>>>>>>;
  template <bool Byte, class S> using typed_policy = storage_policy<
    std::conditional_t<Byte, sort_list<S>, bin<tip<S>, sort_undefined>>>;

  template <class P, bool Typed, class S, bool KnownWidth = false> struct byte_or_raw_codec {
    using policy = P;
    using native = profile_array<P>;
    using mapped = mapped_native<P>;
    static constexpr bool sorted = false;
    static bit_string key(row const &r) {
      if constexpr (!Typed) return bit_string::from_bytes(r.key);
      else if constexpr (std::is_same_v<S, strings>) return typed_detail::key<P, S>(r.key);
      else return typed_detail::key<P, S>(unbig(r.key));
    }
    static bit_string value(row const &r) {
      if constexpr (!Typed) return bit_string::from_bytes(r.value);
      else if constexpr (std::is_same_v<S, strings>) return typed_detail::value<P, S>(std::optional(r.value));
      else if constexpr (std::is_same_v<typename S::value_codec, unsigned_value<64>>) return typed_detail::value<P, S>(unbig(r.value));
      else return typed_detail::value<P, S>(r.value);
    }
    static auto build(std::span<row const> rows) {
      profile_native_writer<P> writer(KnownWidth ? std::optional<u64>(17) : P::value_width);
      for (auto const &r : rows) writer.append(profile_record{key(r), value(r)});
      return std::make_shared<native const>(writer.finish());
    }
    static auto serialize(native const &n) { return encode_native_sections(n).materialize(); }
  };
  template <class S> struct bit_sort_codec {
    using policy = typed_policy<false, S>;
    using native = sort_profile_array<policy>;
    using mapped = mapped_sort_profile<policy>;
    static constexpr bool sorted = true;
    static bit_string key(row const &r) {
      if constexpr (std::is_same_v<S, strings>) return sort_profile_query<policy, S>(r.key);
      else return sort_profile_query<policy, S>(unbig(r.key));
    }
    static bit_string value(row const &r) {
      if constexpr (std::is_same_v<S, strings>) return typed_detail::value<policy, S>(std::optional(r.value));
      else if constexpr (std::is_same_v<typename S::value_codec, unsigned_value<64>>) return typed_detail::value<policy, S>(unbig(r.value));
      else return typed_detail::value<policy, S>(r.value);
    }
    static auto build(std::span<row const> rows) {
      sort_profile_writer<policy> writer;
      for (auto const &r : rows) {
        if constexpr (std::is_same_v<S, strings>) writer.template append<S>(r.key, std::optional(r.value));
        else if constexpr (std::is_same_v<typename S::value_codec, unsigned_value<64>>) writer.template append<S>(unbig(r.key), unbig(r.value));
        else writer.template append<S>(unbig(r.key), r.value);
      }
      return std::make_shared<native const>(writer.finish());
    }
    static auto serialize(native const &n) { return encoded_sort_sections<policy>::from(n).materialize(); }
  };

  struct accounting {
    u64 records = 0, file_bytes = 0, data_bytes = 0, data_bits = 0, key_bits = 0, value_bits = 0;
    u64 low = 0, high = 0, samples = 0, sparse = 0, ranks = 0, flags = 0, cuts = 0, sort_metadata = 0;
    u64 directory = 0, padding = 0, universe = 0, offset_count = 0, common_value_bits = 0;
    u64 suffix_length_bytes = 0, suffix_zero_bytes = 0, suffix_nul_safe_bytes = 0;
    void print(std::string const &mode, fixture const &f, std::string const &name) const {
      require(data_bits >= key_bits + value_bits, "data bit components");
      require(file_bytes == data_bytes + low + high + samples + sparse + ranks + flags + cuts + sort_metadata + directory + padding,
        "complete file accounting");
      std::cout << mode << ',' << f.name << ',' << f.rows.size() << ',' << name << ',' << records << ',' << file_bytes
        << ',' << data_bytes << ',' << data_bits << ',' << key_bits << ',' << value_bits << ',' << data_bits - key_bits - value_bits
        << ',' << low << ',' << high << ',' << samples << ',' << sparse << ',' << ranks << ',' << flags << ',' << cuts
        << ',' << sort_metadata << ',' << directory << ',' << padding << ',' << universe << ',' << offset_count << ',' << common_value_bits
        << ',' << suffix_length_bytes << ',' << suffix_zero_bytes << ',' << suffix_nul_safe_bytes << '\n';
    }
  };
  u64 leb_bytes(u64 n) { u64 bytes = 1; while (n >= 128) { n >>= 7; ++bytes; } return bytes; }
  template <class P, class View> void raw_stats(accounting &a, View view) {
    a.records += view.size(); a.data_bits += view.metadata().extent << P::unit_shift;
    auto ef = view.group_offsets(); a.universe += ef.universe(); a.offset_count += ef.size();
    a.common_value_bits = view.metadata().common_value_width.value_or(0) << P::unit_shift;
    a.low += ef.low_words().size() * 8; a.high += ef.high_words().size() * 8;
    a.samples += ef.samples().size() * 16; a.sparse += ef.sparse_words().size() * 8;
    if (!view.size()) return;
    auto cursor = view.encoded_cursor();
    for (u64 i = 0; i < view.size(); ++i) {
      auto const &record = cursor.peek();
      a.key_bits += record.suffix.size(); a.value_bits += record.value.size();
      if constexpr (P::unit == profile_unit::byte) {
        a.suffix_length_bytes += leb_bytes(record.suffix.size() >> 3);
        for (u64 j = 0; j < record.suffix.size(); j += 8)
          if (profile_detail::load_bits(record.suffix, j, 8) == 0) { ++a.suffix_zero_bytes; ++a.suffix_nul_safe_bytes; }
        a.suffix_nul_safe_bytes += (record.suffix.size() >> 3) + 2;
      }
      cursor.advance();
    }
  }
  template <class Codec> accounting native_stats(typename Codec::native const &n, std::span<std::byte const> file) {
    using P = typename Codec::policy;
    accounting a; a.file_bytes = file.size();
    auto body = file.subspan(file_detail::header_bytes);
    if constexpr (Codec::sorted) {
      auto view = n.view(); a.records = n.size(); a.data_bits = view.data().size();
      auto ef = view.group_offsets(); a.universe = ef.universe(); a.offset_count = ef.size();
      a.common_value_bits = view.metadata().common_value_width.value_or(0);
      a.low = ef.low_words().size() * 8; a.high = ef.high_words().size() * 8;
      a.samples = ef.samples().size() * 16; a.sparse = ef.sparse_words().size() * 8;
      for (u64 i = 0; i < n.size(); ++i) {
        auto frame = view.encoded_at(i); a.key_bits += frame.literal[1].size(); a.value_bits += frame.value.size();
      }
      a.directory = file_detail::header_bytes + sort_profile_file_detail::directory_bytes;
      a.data_bytes = n.data().bytes.size();
      for (unsigned slot : {5u, 6u, 7u}) a.sort_metadata += file_detail::get(body, 72 + 16 * slot, 8);
    } else {
      raw_stats<P>(a, n.view()); a.directory = file_detail::header_bytes + section_detail::native_directory_bytes;
      a.data_bytes = n.bytes().size();
    }
    a.padding = a.file_bytes - a.directory - a.data_bytes - a.low - a.high - a.samples - a.sparse - a.sort_metadata;
    return a;
  }
  template <class P, class Native> accounting index_stats(cola_index<P, Native> const &n, std::span<std::byte const> file) {
    accounting a; a.file_bytes = file.size(); a.directory = file_detail::header_bytes + cola_section_detail::directory_bytes;
    for (unsigned route = 0; route < 2; ++route) {
      raw_stats<P>(a, n.borrowed(route).view()); a.data_bytes += n.borrowed(route).bytes().size();
      a.ranks += 8 * (n.interleave(route).classes.size() + n.interleave(route).checkpoints.size());
      a.flags += n.false_borrow_bits(route).size(); a.cuts += 8 * n.cut_lcps(route).size();
    }
    a.padding = a.file_bytes - a.directory - a.data_bytes - a.low - a.high - a.samples - a.sparse - a.ranks - a.flags - a.cuts;
    return a;
  }
  object_id id(u64 n) {
    std::string code(32, '0');
    for (unsigned i = 0; i < 16; ++i) code[31 - i] = "0123456789abcdef"[(n >> (i * 4)) & 15];
    return object_id(code);
  }
  template <class Codec> void run(std::filesystem::path const &root, std::string const &mode, fixture const &f) {
    using P = typename Codec::policy;
    using native = typename Codec::native;
    using index = cola_index<P, native>;
    auto directory = root / (f.name + "-" + std::to_string(f.rows.size()) + "-" + mode);
    std::filesystem::create_directories(directory);
    logical(directory / "logical.bin", f);
    std::array<std::vector<row>, 4> parts;
    for (u64 i = 0; i < f.rows.size(); ++i) parts[i % 4 < 2 ? i % 4 : 2].push_back(f.rows[i]);
    std::array<std::shared_ptr<native const>, 4> natives;
    for (unsigned i = 0; i < 4; ++i) {
      natives[i] = Codec::build(parts[i]);
      auto bytes = Codec::serialize(*natives[i]); auto name = "native-" + std::to_string(i) + ".kv";
      write(directory / name, bytes);
      auto mapped = Codec::mapped::open(directory / name); mapped.scan();
      auto cursor = mapped.view().cursor();
      for (auto const &r : parts[i]) {
        require(!cursor.done(), "short decoded native");
        auto expected_key = Codec::key(r), expected_value = Codec::value(r);
        require(compare_bits(cursor.peek().key.prefix, expected_key.view()) == 0, "native logical key mismatch");
        require(compare_bits(cursor.peek().value, expected_value.view()) == 0, "native logical value mismatch");
        cursor.advance();
      }
      require(cursor.done(), "long decoded native");
      native_stats<Codec>(*natives[i], bytes).print(mode, f, name);
    }
    std::vector<std::shared_ptr<index const>> chain;
    chain.push_back(std::make_shared<index const>(index::adopt_native(natives[2])));
    chain.push_back(std::make_shared<index const>(index::adopt_native(natives[1], chain.back())));
    chain.push_back(std::make_shared<index const>(index::adopt_native(natives[3], chain.back(), natives[0])));
    while (chain.back()->virtual_size() > P::group_size)
      chain.push_back(std::make_shared<index const>(index::adopt_native(natives[3], chain.back())));
    require(chain.back()->virtual_size() <= 15, "complete prepared root");
    auto query_root = cola_query_root<P, index>::adopt_prepared(chain.back());
    for (u64 q = 0; q < std::min<u64>(f.rows.size(), 129); ++q) {
      auto const &r = f.rows[q * (f.rows.size() - 1) / (std::min<u64>(f.rows.size(), 129) - 1)];
      auto key = Codec::key(r), value = Codec::value(r);
      cola_query_cursor<P, index> query(query_root, key.view());
      unsigned matches = 0;
      while (!query.done()) {
        query.step(64);
        if (query.has_match()) { auto found = query.take_match(); require(compare_bits(found.value.view(), value.view()) == 0, "cascade query value"); ++matches; }
      }
      require(matches == 1, "cascade must represent each logical record exactly once");
    }
    for (u64 i = 0; i < chain.size(); ++i) {
      auto native_id = id(i == 0 ? 3 : i == 1 ? 2 : 4);
      auto main = i ? std::optional(blob_identity{id(i == 1 ? 3 : i == 2 ? 2 : 4), id(100 + i - 1)}) : std::nullopt;
      auto side = i == 2 ? std::optional(id(1)) : std::nullopt;
      auto bytes = encoded_cola_sections<P>::from(*chain[i], native_id, main, side).materialize();
      auto name = "route-" + std::to_string(i) + ".index"; write(directory / name, bytes);
      auto mapped = mapped_cola_index<P>::open(directory / name); mapped.scan();
      require(mapped.native_id() == native_id && mapped.main_id() == main && mapped.secondary_id() == side, "mapped dependency identities");
      for (unsigned route = 0; route < 2; ++route) {
        auto actual = mapped.borrowed(route).cursor(), expected = chain[i]->borrowed(route).view().cursor();
        while (!expected.done()) {
          require(!actual.done() && compare_bits(actual.peek().key.prefix, expected.peek().key.prefix) == 0, "mapped borrowed key mismatch");
          actual.advance(); expected.advance();
        }
        require(actual.done(), "extra borrowed key");
      }
      index_stats(*chain[i], bytes).print(mode, f, name);
    }
  }
  template <bool Byte, bool Fixed, unsigned Width> void raw(std::filesystem::path const &root, std::string const &mode, fixture const &f) {
    run<byte_or_raw_codec<raw_policy<Byte, Fixed, Width>, false, strings>>(root, mode, f);
  }
  template <bool Byte, class S, bool KnownWidth = false> void typed(std::filesystem::path const &root, std::string const &mode, fixture const &f) {
    if constexpr (Byte) run<byte_or_raw_codec<typed_policy<true, S>, true, S, KnownWidth>>(root, mode, f);
    else run<bit_sort_codec<S>>(root, mode, f);
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 4, "usage: profile-space OUTPUT MODE ROWS");
    std::filesystem::path root(argv[1]); std::string mode(argv[2]); auto count = std::stoull(argv[3]);
    require(count && count % 4 == 0 && count <= 1'048'576, "fixture count must be a positive multiple of four <=2^20");
    require(mode == "raw-byte" || mode == "raw-bit" || mode == "typed-byte" || mode == "typed-bit" || mode == "typed-byte-known", "unknown mode");
    std::cout << "mode,fixture,logical_records,file,records,file_bytes,data_bytes,data_bits,key_literal_bits,encoded_value_bits,framing_bits,ef_low_bytes,ef_high_bytes,ef_sample_bytes,ef_sparse_bytes,rank_bytes,flag_bytes,cut_bytes,sort_metadata_bytes,header_directory_bytes,alignment_bytes,universe,offset_count,common_value_bits,suffix_length_bytes,suffix_zero_bytes,suffix_nul_safe_bytes\n";
    for (unsigned shape = 0; shape < 5; ++shape) for (bool variable : {false, true}) {
      auto f = make(shape, variable, count);
      if (mode == "typed-byte" || mode == "typed-byte-known") {
        if (!f.integer && !variable && mode == "typed-byte-known") typed<true, strings, true>(root, mode, f);
        else if (!f.integer) typed<true, strings>(root, mode, f);
        else if (variable) typed<true, integer_sort<false>>(root, mode, f);
        else typed<true, integer_sort<true>>(root, mode, f);
      } else if (mode == "typed-bit") {
        if (!f.integer) typed<false, strings>(root, mode, f);
        else if (variable) typed<false, integer_sort<false>>(root, mode, f);
        else typed<false, integer_sort<true>>(root, mode, f);
      } else if (mode == "raw-byte") {
        if (variable) raw<true, false, 8>(root, mode, f);
        else if (f.integer) raw<true, true, 8>(root, mode, f);
        else raw<true, true, 16>(root, mode, f);
      } else {
        if (variable) raw<false, false, 8>(root, mode, f);
        else if (f.integer) raw<false, true, 8>(root, mode, f);
        else raw<false, true, 16>(root, mode, f);
      }
    }
  } catch (std::exception const &e) { std::cerr << e.what() << '\n'; return 1; }
}
