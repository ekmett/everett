#include <everett/multiverse.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace {
  using namespace everett;
  using bytes = storage_policy<profile_unit::byte, variable_values, 15>;
  using bits = storage_policy<profile_unit::bit, fixed_values<3>, 7>;

  static_assert(std::same_as<multiverse<bytes>::policy_type, bytes>);
  static_assert(std::same_as<multiverse<bytes>::sort, sort<bytes>>);
  static_assert(std::same_as<multiverse<bytes>::blob, profile_blob<bytes>>);
  static_assert(std::same_as<multiverse<bytes>::file, file<bytes>>);
  static_assert(std::same_as<multiverse<bytes>::world, world<bytes>>);
  static_assert(std::same_as<multiverse<bytes>::timeline, timeline<bytes>>);
  static_assert(std::same_as<multiverse<bytes>::branch_point, branch_point<bytes>>);
  static_assert(std::same_as<multiverse<bits>::sort::policy_type, bits>);
  static_assert(std::same_as<multiverse<bits>::blob::policy_type, bits>);
  static_assert(std::same_as<multiverse<bits>::file::policy_type, bits>);
  static_assert(!std::convertible_to<sort<bytes>, sort<bits>>);
  static_assert(!std::convertible_to<multiverse<bytes>, multiverse<bits>>);
  static_assert(!std::default_initializable<multiverse<bytes>>);

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid multiverse input accepted");
  }

  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
      auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
      for (unsigned attempt = 0; attempt != 100; ++attempt) {
        auto candidate = std::filesystem::temp_directory_path() /
          ("everett-multiverse-" + std::to_string(seed) + "-" + std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) { path = std::move(candidate); return; }
        if (error && error != std::errc::file_exists) throw std::system_error(error);
      }
      throw std::runtime_error("could not allocate test directory");
    }
    temporary_directory(temporary_directory const &) = delete;
    temporary_directory & operator=(temporary_directory const &) = delete;
    ~temporary_directory() { std::error_code error; std::filesystem::remove_all(path, error); }
  };

  void write_bytes(std::filesystem::path const & path, std::span<std::byte const> data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<char const *>(data.data()), static_cast<std::streamsize>(data.size()));
    output.close();
  }

  template <class P> void fixture(std::filesystem::path const & root, object_id const & id,
                                 file_kind kind, bit_string const & body) {
    file_header<P> header;
    header.kind = kind;
    header.extent = body.bit_size / P::bits_per_unit;
    // Body bytes here exercise the envelope/mapping; they are not a serialized
    // profile_blob. Record/index section serialization has its own contract.
    if (kind == file_kind::native_blob) header.common_value_width = P::value_width;
    if (kind == file_kind::fractional_index) header.common_value_width = 0;
    auto encoded = encode_file<P>(header, body.bytes);
    write_bytes(root / object_path(id, kind), encoded);
  }

  void sort_codes() {
    auto byte_code = bit_string::from_bytes(std::string("\0\xff", 2));
    sort<bytes> a(byte_code);
    require(bit_string::copy(a.code()) == byte_code, "byte sort code roundtrip");
    require(a == sort<bytes>(byte_code), "sort equality");
    auto bit_code = bit_string::from_bits("10101");
    sort<bits> b(bit_code);
    require(bit_string::copy(b.code()) == bit_code, "partial-byte sort code roundtrip");
    rejects([&] { sort<bytes> invalid(bit_code); });
    bit_code.bytes.back() |= std::byte{1};
    rejects([&] { sort<bits> invalid(bit_code); });
    // The empty code is meaningful for a singleton prefix-free sort family;
    // individual sort descriptors do not purport to validate an entire family.
    sort<bits> empty(bit_string{});
    require(empty.code().empty(), "empty singleton sort code");
  }

  void roots_are_read_only() {
    temporary_directory temporary;
    auto root = temporary.path / "missing";
    rejects([&] { multiverse<bytes> missing(root); });
    require(!std::filesystem::exists(root), "constructing reader must not create a directory");
    rejects([&] { multiverse<bytes> empty(std::filesystem::path{}); });
    auto data = bit_string::from_bytes("not a directory");
    write_bytes(temporary.path / "regular-file", data.bytes);
    rejects([&] { multiverse<bytes> regular(temporary.path / "regular-file"); });
    multiverse<bytes> store(temporary.path / ".");
    require(store.root() == std::filesystem::canonical(temporary.path), "stable canonical backing root");
    auto id = object_id("0123456789abcdef0123456789abcdef");
    rejects([&] { store.open_object(id, file_kind::native_blob); });
    require(!std::filesystem::exists(temporary.path / "01"), "missing object open must not create shards");
  }

  void read_objects() {
    temporary_directory temporary;
    auto id = object_id("00112233445566778899aabbccddeeff");
    auto byte_body = bit_string::from_bytes(std::string("\0hello\xff", 7));
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index})
      fixture<bytes>(temporary.path, id, kind, byte_body);
    multiverse<bytes> store(temporary.path);
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      auto object = store.open_object(id, kind);
      require(object.header().kind == kind, "opened requested object kind");
      auto body = object.body();
      require(std::equal(body.bytes().begin(), body.bytes().end(), byte_body.bytes.begin(), byte_body.bytes.end()),
              "mapped body matches fixture");
    }
    auto bit_id = object_id("ffeeddccbbaa99887766554433221100");
    auto bit_body = bit_string::from_bits("10010110101");
    fixture<bits>(temporary.path, bit_id, file_kind::native_blob, bit_body);
    multiverse<bits> bit_store(temporary.path);
    auto bit_object = bit_store.open_object(bit_id, file_kind::native_blob);
    require(bit_object.header().extent == 11, "bit extent remains measured in bits");
    auto body = bit_object.body();
    require(bit_string::copy(bit_view(body.bytes(), bit_object.header().extent)) == bit_body, "bit object roundtrip");
    rejects([&] { store.open_object(bit_id, file_kind::native_blob); });
    rejects([&] { bit_store.open_object(id, file_kind::native_blob); });

    using wrong_group = storage_policy<profile_unit::byte, variable_values, 7>;
    multiverse<wrong_group> grouped(temporary.path);
    rejects([&] { grouped.open_object(id, file_kind::native_blob); });
    using wrong_width = storage_policy<profile_unit::byte, fixed_values<0>, 15>;
    multiverse<wrong_width> fixed(temporary.path);
    rejects([&] { fixed.open_object(id, file_kind::native_blob); });

    // Retained body owns its mapping after both reader and typed file die.
    auto retained = [&] {
      multiverse<bytes> local(temporary.path);
      auto object = local.open_object(id, file_kind::native_blob);
      return object.body();
    }();
    std::filesystem::remove(temporary.path / object_path(id, file_kind::native_blob));
    require(std::equal(retained.bytes().begin(), retained.bytes().end(), byte_body.bytes.begin(), byte_body.bytes.end()),
            "body pin survives reader destruction and unlink");

    // A canonical filename cannot disguise another envelope kind.
    file_header<bytes> wrong;
    wrong.kind = file_kind::fractional_index;
    wrong.common_value_width = 0;
    wrong.extent = byte_body.bytes.size();
    auto wrong_kind = encode_file<bytes>(wrong, byte_body.bytes);
    write_bytes(temporary.path / object_path(id, file_kind::native_blob), wrong_kind);
    rejects([&] { store.open_object(id, file_kind::native_blob); });
  }
}

int main() {
  try {
    sort_codes();
    roots_are_read_only();
    read_objects();
    std::cout << "multiverse tests passed\n";
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
 * \brief Tests Everett's multiverse behavior.
 */
