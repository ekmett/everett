#include <everett/file.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

namespace {
  using namespace everett;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F action) {
    bool threw = false;
    try { action(); } catch (std::exception const &) { threw = true; }
    require(threw, "invalid file or object path accepted");
  }
  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
      std::random_device random;
      for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto candidate = std::filesystem::temp_directory_path() /
                         ("everett-file-test-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
      }
      throw std::runtime_error("cannot create file test directory");
    }
    temporary_directory(temporary_directory const &) = delete;
    ~temporary_directory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  };
  void write(std::filesystem::path const & path, std::span<std::byte const> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<char const *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
  }
  void rehash_header(std::vector<std::byte> & bytes) {
    file_detail::put(bytes, 68, 4, 0);
    file_detail::put(bytes, 68, 4, crc32c(std::span<std::byte const>(bytes).first(96)));
  }

  template <class P> void roundtrip(std::filesystem::path const & directory) {
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      file_header<P> header{kind, 25, 3, std::nullopt};
      if (kind == file_kind::fractional_index) header.common_value_width = 0;
      if (kind == file_kind::native_blob) header.common_value_width = P::value_width.value_or(3);
      std::vector<std::byte> body(static_cast<std::size_t>(file_detail::body_bytes<P>(25)), std::byte{0xab});
      if constexpr (P::unit == profile_unit::bit) body.back() &= std::byte{0x80};
      auto encoded = encode_file(header, body);
      require(validate_file<P>(encoded) == header, "typed file roundtrip");
      require(encoded[8] == std::byte{1} && encoded[9] == std::byte{0} &&
              encoded[10] == std::byte{96} && encoded[11] == std::byte{0}, "header little-endian version");
      require(file_detail::get(encoded, 24, 8) == P::group_size, "group policy missing");
      if (kind == file_kind::fractional_index) {
        require(file_detail::get(encoded, 40, 8) == 0, "borrowed values not zero-width");
        require(file_detail::get(encoded, 32, 8) == P::value_width.value_or(0), "borrowed stream lost policy width");
      }
      for (std::size_t length = 0; length < encoded.size(); ++length)
        rejects([&] { validate_file<P>(std::span<std::byte const>(encoded).first(length)); });
      for (std::size_t position = 0; position < 96; ++position) {
        auto corrupt = encoded;
        corrupt[position] ^= std::byte{1};
        rejects([&] { validate_file<P>(corrupt); });
      }
      auto changed = encoded;
      changed[96] ^= std::byte{0x80};
      rejects([&] { validate_file<P>(changed); });
      changed = encoded; changed.push_back(std::byte{0});
      rejects([&] { validate_file<P>(changed); });

      auto path = directory / ("object" + std::string(file_extension(kind)));
      write(path, encoded);
      mapped_slice retained;
      {
        auto stored = file<P>::open(path);
        require(stored.header() == header, "mapped file header mismatch");
        retained = stored.body();
      }
      require(std::ranges::equal(retained.bytes(), body), "mapped body pin did not retain bytes");
      auto suffix_wrong = directory / "bad.extension";
      write(suffix_wrong, encoded);
      rejects([&] { file<P>::open(suffix_wrong); });
    }
  }

  template <std::uint64_t K> void policy_matrix(std::filesystem::path const & directory) {
    roundtrip<storage_policy<profile_unit::byte, variable_values, K>>(directory);
    roundtrip<storage_policy<profile_unit::bit, variable_values, K>>(directory);
    roundtrip<storage_policy<profile_unit::byte, fixed_values<5>, K>>(directory);
    roundtrip<storage_policy<profile_unit::bit, fixed_values<5>, K>>(directory);
  }

  void test_descriptors() {
    using byte_fixed = storage_policy<profile_unit::byte, fixed_values<5>, 15>;
    using bit_fixed = storage_policy<profile_unit::bit, fixed_values<5>, 15>;
    auto encoded = encode_file(file_header<byte_fixed>{file_kind::native_blob, 25, 3, 5},
                               std::vector<std::byte>(25));
    rejects([&] { validate_file<bit_fixed>(encoded); });
    rejects([&] { validate_file<storage_policy<profile_unit::byte, fixed_values<6>>>(encoded); });
    rejects([&] { validate_file<storage_policy<profile_unit::byte, variable_values>>(encoded); });
    rejects([&] { validate_file<storage_policy<profile_unit::byte, fixed_values<5>, 7>>(encoded); });
    for (auto patch : std::array<std::pair<std::size_t, std::uint64_t>, 11>{
           {{8, 2}, {10, 80}, {12, 7}, {16, 2}, {17, 2}, {18, 1},
            {24, 31}, {32, 6}, {40, 4}, {72, 97}, {88, 1}}}) {
      auto bad = encoded;
      file_detail::put(bad, patch.first, 1, patch.second);
      rehash_header(bad);
      rejects([&] { validate_file<byte_fixed>(bad); });
    }
    auto bad = encoded;
    file_detail::put(bad, 80, 8, std::numeric_limits<std::uint64_t>::max());
    rehash_header(bad);
    rejects([&] { validate_file<byte_fixed>(bad); });
    bad = encoded;
    file_detail::put(bad, 48, 8, std::numeric_limits<std::uint64_t>::max());
    rehash_header(bad);
    rejects([&] { validate_file<byte_fixed>(bad); });
    rejects([] { encode_file(file_header<byte_fixed>{file_kind::native_blob, 5, 2, 5}, std::vector<std::byte>(5)); });
    rejects([] { encode_file(file_header<byte_fixed>{file_kind::fractional_index, 0, 0, std::nullopt}, {}); });
    rejects([] { encode_file(file_header<byte_fixed>{file_kind::fractional_index, 0, 0, 5}, {}); });
    rejects([] { encode_file(file_header<byte_fixed>{static_cast<file_kind>(2), 0, 0, 0}, {}); });

    using bits = storage_policy<profile_unit::bit>;
    auto bit_header = file_header<bits>{file_kind::native_blob, 3, 1, std::nullopt};
    auto padded = encode_file(bit_header, std::array<std::byte, 1>{std::byte{0xa0}});
    padded.back() |= std::byte{1};
    file_detail::put(padded, 64, 4, crc32c(std::span<std::byte const>(padded).subspan(96)));
    rehash_header(padded);
    rejects([&] { validate_file<bits>(padded); }); // CRC valid; padding still noncanonical.
    rejects([&] { encode_file(bit_header, std::array<std::byte, 1>{std::byte{0xa1}}); });
    auto empty = encode_file(file_header<bits>{}, {});
    require(empty.size() == 96 && validate_file<bits>(empty).extent == 0, "empty body file");
    using zero_fixed = storage_policy<profile_unit::byte, fixed_values<0>>;
    auto zero = encode_file(file_header<zero_fixed>{file_kind::native_blob, 0, 3, 0}, {});
    require(validate_file<zero_fixed>(zero).common_value_width == 0, "zero fixed width lost");
  }

  void test_paths() {
    auto id = object_id::from_hex("abcdef0123456789abcdef0123456789");
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      auto path = object_path(id, kind).generic_string();
      require(path == "ab/cd/ef0123456789abcdef0123456789" + std::string(file_extension(kind)), "shard prefix repeated in leaf");
      auto parsed = parse_object_path(path);
      require(parsed.id == id && parsed.kind == kind, "sharded ID reconstruction");
      rejects([&] { parse_object_path("ab/cd/" + id.hex() + std::string(file_extension(kind))); });
      rejects([&] { parse_object_path("/" + path); });
      rejects([&] { parse_object_path("../" + path); });
      rejects([&] { parse_object_path("ab//" + path.substr(3)); });
      auto bad = path; bad[2] = '\\';
      rejects([&] { parse_object_path(bad); });
      bad = path; bad.back() = '!';
      rejects([&] { parse_object_path(bad); });
      bad = path; bad[6] = 'G';
      rejects([&] { parse_object_path(bad); });
    }
    rejects([] { object_id::from_hex("ABCDEF0123456789abcdef0123456789"); });
    rejects([] { object_id::from_hex("abcdef0123456789abcdef012345678"); });
    rejects([] { object_id::from_hex("player/entity/position"); });
    rejects([] { parse_object_path("ab/cd/ef0123456789abcdef0123456789.world"); });
    rejects([] { parse_object_path("ab/cd/ef0123456789abcdef0123456789.merge"); });
    require(crc32c(std::as_bytes(std::span("123456789", std::size_t{9}))) == 0xe3069283u, "CRC32C check vector");
    require(crc32c({}) == 0, "empty CRC32C");
  }
}

int main() {
  try {
    temporary_directory directory;
    policy_matrix<3>(directory.path); policy_matrix<7>(directory.path);
    policy_matrix<15>(directory.path); policy_matrix<31>(directory.path);
    test_descriptors();
    test_paths();
    std::cout << "Typed binary files, CRC32C, mapped bodies, and canonical sharded paths passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
 * \endlicense
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's files behavior.
 */
