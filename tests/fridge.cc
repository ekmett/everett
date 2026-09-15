/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Diet's fridge behavior.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/fridge.h>

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
  using namespace diet;
  using bytes = storage_policy<profile_unit::byte, variable_values, 15>;
  using bits = storage_policy<profile_unit::bit, fixed_values<3>, 7>;

  static_assert(std::same_as<fridge<bytes>::policy_type, bytes>);
  static_assert(std::same_as<fridge<bytes>::sort, sort<bytes>>);
  static_assert(std::same_as<fridge<bytes>::blob, profile_blob<bytes>>);
  static_assert(std::same_as<fridge<bytes>::file, file<bytes>>);
  static_assert(std::same_as<fridge<bytes>::cola_index, cola_index<bytes>>);
  static_assert(std::same_as<fridge<bits>::cola_local_merge_job<>, cola_local_merge_job<bits>>);
  static_assert(std::same_as<fridge<bytes>::mapped_cola_artifact, mapped_cola_artifact<bytes>>);
  static_assert(std::same_as<fridge<bits>::mapped_cola_index_builder, mapped_cola_index_builder<bits>>);
  static_assert(std::same_as<fridge<bits>::mapped_cola_query_root, mapped_cola_query_root<bits>>);
  static_assert(std::same_as<fridge<bytes>::object_writer, object_writer<bytes>>);
  static_assert(std::same_as<fridge<bytes>::cola, cola<bytes>>);
  static_assert(std::same_as<fridge<bytes>::timeline, timeline<bytes>>);
  static_assert(std::same_as<fridge<bytes>::branch_point, branch_point<bytes>>);
  static_assert(std::same_as<fridge<bits>::sort::policy_type, bits>);
  static_assert(std::same_as<fridge<bits>::blob::policy_type, bits>);
  static_assert(std::same_as<fridge<bits>::file::policy_type, bits>);
  static_assert(!std::convertible_to<sort<bytes>, sort<bits>>);
  static_assert(!std::convertible_to<fridge<bytes>, fridge<bits>>);
  static_assert(!std::default_initializable<fridge<bytes>>);

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid fridge input accepted");
  }

  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
      auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
      for (unsigned attempt = 0; attempt != 100; ++attempt) {
        auto candidate = std::filesystem::temp_directory_path() /
          ("diet-fridge-" + std::to_string(seed) + "-" + std::to_string(attempt));
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
    rejects([&] { fridge<bytes> missing(root); });
    require(!std::filesystem::exists(root), "constructing reader must not create a directory");
    rejects([&] { fridge<bytes> empty(std::filesystem::path{}); });
    auto data = bit_string::from_bytes("not a directory");
    write_bytes(temporary.path / "regular-file", data.bytes);
    rejects([&] { fridge<bytes> regular(temporary.path / "regular-file"); });
    fridge<bytes> store(temporary.path / ".");
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
    fridge<bytes> store(temporary.path);
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
    fridge<bits> bit_store(temporary.path);
    auto bit_object = bit_store.open_object(bit_id, file_kind::native_blob);
    require(bit_object.header().extent == 11, "bit extent remains measured in bits");
    auto body = bit_object.body();
    require(bit_string::copy(bit_view(body.bytes(), bit_object.header().extent)) == bit_body, "bit object roundtrip");
    rejects([&] { store.open_object(bit_id, file_kind::native_blob); });
    rejects([&] { bit_store.open_object(id, file_kind::native_blob); });

    using wrong_group = storage_policy<profile_unit::byte, variable_values, 7>;
    fridge<wrong_group> grouped(temporary.path);
    rejects([&] { grouped.open_object(id, file_kind::native_blob); });
    using wrong_width = storage_policy<profile_unit::byte, fixed_values<0>, 15>;
    fridge<wrong_width> fixed(temporary.path);
    rejects([&] { fixed.open_object(id, file_kind::native_blob); });

    // Retained body owns its mapping after both reader and typed file die.
    auto retained = [&] {
      fridge<bytes> local(temporary.path);
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
    auto trusted_kind = store.open_object(id, file_kind::native_blob, file_open_mode::trusted);
    require(trusted_kind.header().kind == file_kind::fractional_index,
            "trusted open unexpectedly enforced filename/header agreement");
    trusted_kind.scan(); // Object bytes are valid; the trusted filename assertion was wrong.
  }

  template <class P> void seal_objects() {
#if defined(__APPLE__) || defined(__linux__)
    temporary_directory temporary;
    fridge<P> store(temporary.path);
    auto id = object_id("123456789abcdef0123456789abcdef0");
    auto attempt = object_attempt_id("fedcba9876543210fedcba9876543210");
    auto payload = bit_string::from_bytes("opaque encoded body");
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      file_header<P> header{kind, payload.bit_size / P::bits_per_unit, 0,
        kind == file_kind::fractional_index ? std::optional<std::uint64_t>{0} : P::value_width};
      auto body = std::span<std::byte const>(payload.bytes);
      std::array chunks{body.first(3), body.subspan(3)};
      auto receipt = kind == file_kind::native_blob ? store.seal_object(id, attempt, header, body) :
        store.seal_object(id, attempt, header, chunks);
      auto object = store.open_object(id, kind);
      object.scan();
      auto mapped = object.body();
      require(object.header() == header && std::ranges::equal(mapped.bytes(), body),
              "fridge sealing changed envelope or body");
      require(receipt.path == store.root() / object_path(id, kind), "fridge sealed outside its root");
      rejects([&] { store.seal_object(id, attempt, header, body); });
      require(std::ranges::equal(mapped.bytes(), body), "fridge collision changed retained object");
    }
#endif
  }

  template <class P> void trusted_objects() {
    temporary_directory temporary;
    auto id = object_id("0123456789abcdef0123456789abcdef");
    auto payload = bit_string::from_bytes("retained object body");
    fridge<P> store(temporary.path);
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      fixture<P>(temporary.path, id, kind, payload);
      mapped_slice retained;
      {
        auto object = store.open_object(id, kind, file_open_mode::trusted);
        require(object.header().kind == kind, "trusted object kind on explicit access");
        object.scan();
        retained = object.body();
      }
      auto path = temporary.path / object_path(id, kind);
      std::filesystem::remove(path);
      require(std::ranges::equal(retained.bytes(), payload.bytes), "trusted object pin failed after unlink");

      // A malformed header must reach the caller intact in trusted mode.
      // This catches accidental header() calls inside open_object itself.
      std::vector<std::byte> malformed(128, std::byte{0xab});
      write_bytes(path, malformed);
      rejects([&] { store.open_object(id, kind); });
      {
        auto object = store.open_object(id, kind, file_open_mode::trusted);
        require(object.body().size() == malformed.size() - 96, "trusted object decoded a physical extent");
        rejects([&] { object.header(); });
        rejects([&] { object.scan(); });
      }
      malformed.resize(95);
      write_bytes(path, malformed);
      rejects([&] { store.open_object(id, kind, file_open_mode::trusted); });
      fixture<P>(temporary.path, id, kind, payload);
      using wrong_policy = storage_policy<P::unit, typename P::value_layout, P::group_size == 15 ? 7 : 15,
                                          typename P::backspace_encoding>;
      fridge<wrong_policy> wrong_store(temporary.path);
      rejects([&] { wrong_store.open_object(id, kind); });
      auto object = wrong_store.open_object(id, kind, file_open_mode::trusted);
      require(object.body().size() == payload.bytes.size(), "trusted object checked its policy implicitly");
      rejects([&] { object.header(); });
      rejects([&] { object.scan(); });
    }
  }
}

int main() {
  try {
    sort_codes();
    roots_are_read_only();
    read_objects();
    seal_objects<bytes>();
    seal_objects<bits>();
    trusted_objects<bytes>();
    trusted_objects<bits>();
    trusted_objects<storage_policy<profile_unit::bit, variable_values, 15, golomb<3>>>();
    std::cout << "fridge tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
