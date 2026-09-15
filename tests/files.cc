/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/file.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <type_traits>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

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
  template <class P> void rejects_open(std::filesystem::path const & path, std::span<std::byte const> bytes) {
    write(path, bytes);
    rejects([&] { file<P>::open(path); });
    auto mapping = mapped_file::open(path);
    rejects([&] { file<P>::from_slice(mapping.slice(0, mapping.size())); });
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
      require(file_detail::get(encoded, 18, 1) == static_cast<unsigned>(P::backspace_code) &&
              file_detail::get(encoded, 88, 8) == P::backspace_parameter, "backspace policy missing");
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
        stored.scan();
        retained = stored.body();
        auto mapping = mapped_file::open(path);
        auto whole = mapping.slice(0, mapping.size());
        auto sliced = file<P>::from_slice(whole);
        require(sliced.header() == header, "sliced file header mismatch");
        sliced.scan();
        for (std::uint64_t length = 0; length < whole.size(); ++length)
          rejects([&] { file<P>::from_slice(whole.slice(0, length)); });
      }
      require(std::ranges::equal(retained.bytes(), body), "mapped body pin did not retain bytes");
      auto suffix_wrong = directory / "bad.extension";
      write(suffix_wrong, encoded);
      rejects([&] { file<P>::open(suffix_wrong); });

      // The header remains valid when a payload byte is corrupt. Both entry
      // points retain the object; only deliberate full-body scanning rejects it.
      auto corrupt_path = directory / ("corrupt" + std::string(file_extension(kind)));
      changed = encoded;
      changed[96] ^= std::byte{0x80};
      write(corrupt_path, changed);
      {
        auto corrupt = file<P>::open(corrupt_path);
        require(corrupt.header() == header, "payload corruption changed decoded header");
        rejects([&] { corrupt.scan(); });
        auto mapping = mapped_file::open(corrupt_path);
        auto sliced = file<P>::from_slice(mapping.slice(0, mapping.size()));
        rejects([&] { sliced.scan(); });
      }
      changed = encoded; changed.push_back(std::byte{0});
      rejects_open<P>(corrupt_path, changed);
      changed = encoded; changed[68] ^= std::byte{1};
      rejects_open<P>(corrupt_path, changed);
    }
  }

  template <std::uint64_t K> void policy_matrix(std::filesystem::path const & directory) {
    roundtrip<storage_policy<profile_unit::byte, variable_values, K>>(directory);
    roundtrip<storage_policy<profile_unit::bit, variable_values, K>>(directory);
    roundtrip<storage_policy<profile_unit::byte, fixed_values<5>, K>>(directory);
    roundtrip<storage_policy<profile_unit::bit, fixed_values<5>, K>>(directory);
  }

  template <class P> void test_trusted_validation(std::filesystem::path const & directory) {
    static_assert(std::is_same_v<decltype(std::declval<file<P> const &>().header()), file_header<P>>);
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      file_header<P> header{kind, 25, 1, std::nullopt};
      if (kind == file_kind::fractional_index) header.common_value_width = 0;
      std::vector<std::byte> payload(static_cast<std::size_t>(file_detail::body_bytes<P>(25)), std::byte{0xa8});
      if constexpr (P::unit == profile_unit::bit) payload.back() &= std::byte{0x80};
      auto encoded = encode_file(header, payload);
      auto path = directory / ("trusted" + std::string(file_extension(kind)));
      auto check = [&](std::span<std::byte const> data, bool valid_header, bool valid_body) {
        write(path, data);
        if (!valid_header) rejects([&] { file<P>::open(path); });
        auto opened = file<P>::open(path, file_open_mode::trusted);
        auto mapping = mapped_file::open(path);
        auto sliced = file<P>::from_slice(mapping.slice(0, mapping.size()), file_open_mode::trusted);
        for (auto const * object : {&opened, &sliced}) {
          require(object->body().size() == data.size() - 96, "trusted body did not use physical extent");
          if (valid_header) {
            require(object->header() == header && object->header() == header, "explicit trusted header mismatch");
            auto copy = object->header();
            copy.extent = 0;
            require(copy.extent == 0 && object->header().extent == header.extent, "header copy changed the reader");
          } else rejects([&] { object->header(); });
          if (valid_body) object->scan();
          else rejects([&] { object->scan(); });
        }
      };
      check(encoded, true, true);
      auto bad = encoded;
      bad[0] ^= std::byte{1};
      check(bad, false, false);
      bad = encoded; bad[68] ^= std::byte{1};
      check(bad, false, false);
      bad = encoded; file_detail::put(bad, 24, 8, P::group_size == 15 ? 7 : 15);
      rehash_header(bad);
      check(bad, false, false); // Valid CRC, incompatible policy.
      bad = encoded; bad.push_back(std::byte{0});
      check(bad, false, false); // Physical body includes the trailing byte.
      bad = encoded; bad.pop_back();
      check(bad, false, false);
      check(std::span<std::byte const>(encoded).first(96), false, false);
      bad = encoded; bad[96] ^= std::byte{0x80};
      check(bad, true, false);
      if constexpr (P::unit == profile_unit::bit) {
        bad = encoded; bad.back() |= std::byte{1};
        file_detail::put(bad, 64, 4, crc32c(std::span<std::byte const>(bad).subspan(96)));
        rehash_header(bad);
        check(bad, true, false); // Explicit scanning checks canonical padding too.
      }
      for (std::size_t length = 0; length < 96; ++length) {
        write(path, std::span<std::byte const>(encoded).first(length));
        rejects([&] { file<P>::open(path, file_open_mode::trusted); });
      }
      write(path, encoded);
      rejects([&] { file<P>::open(path, static_cast<file_open_mode>(2)); });
      using wrong_policy = storage_policy<P::unit, typename P::value_layout, P::group_size == 15 ? 7 : 15,
                                          typename P::backspace_encoding>;
      auto wrong = file<wrong_policy>::open(path, file_open_mode::trusted);
      require(wrong.body().size() == payload.size(), "wrong-policy trusted open inspected metadata");
      rejects([&] { wrong.header(); });
      rejects([&] { wrong.scan(); });

      // Trusted opening accepts the caller's filename assertion. Explicit
      // validation concerns the retained object bytes, not its external name.
      auto other_path = directory / "trusted.other-extension";
      write(other_path, encoded);
      rejects([&] { file<P>::open(other_path); });
      auto other = file<P>::open(other_path, file_open_mode::trusted);
      require(other.header() == header, "trusted filename assumption changed header");
      other.scan();
    }
  }

  template <profile_unit Unit> void test_default_headers(std::array<std::uint32_t, 2> expected_crc) {
    using policy = storage_policy<Unit>;
    std::size_t i = 0;
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      file_header<policy> header{kind, 0, 0, std::nullopt};
      if (kind == file_kind::fractional_index) header.common_value_width = 0;
      auto encoded = encode_file(header, {});
      require(encoded.size() == 96 && file_detail::get(encoded, 18, 6) == 0 &&
              file_detail::get(encoded, 88, 8) == 0, "default reserved fields changed");
      // These CRCs were recorded from the original version-1 encoder, before
      // assigning its reserved bytes to the optional backspace descriptor.
      require(file_detail::get(encoded, 68, 4) == expected_crc[i++], "default version-1 header changed");
    }
  }

  template <class Code> void test_backspace_policy(std::filesystem::path const & directory) {
    using policy = storage_policy<profile_unit::bit, variable_values, 15, Code>;
    using defaults = storage_policy<profile_unit::bit>;
    roundtrip<policy>(directory);
    roundtrip<storage_policy<profile_unit::bit, fixed_values<5>, 7, Code>>(directory);
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      auto path = directory / ("backspace" + std::string(file_extension(kind)));
      file_header<policy> header{kind, 0, 0, std::nullopt};
      if (kind == file_kind::fractional_index) header.common_value_width = 0;
      auto encoded = encode_file(header, {});
      require(validate_file<policy>(encoded) == header, "empty nondefault file roundtrip");
      rejects_open<defaults>(path, encoded);
      file_header<defaults> default_header{kind, 0, 0, header.common_value_width};
      auto original = encode_file(default_header, {});
      rejects_open<policy>(path, original);

      // Valid descriptors for a different policy remain incompatible, even
      // when their selector agrees and only the 64-bit parameter differs.
      constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
      for (auto [code, parameter] : std::array<std::pair<std::uint64_t, std::uint64_t>, 7>{
             {{0, 0}, {0, 1}, {0, 63}, {1, 1}, {1, 3}, {1, 256}, {1, maximum}}}) {
        if (code == static_cast<unsigned>(policy::backspace_code) && parameter == policy::backspace_parameter)
          continue;
        auto bad = encoded;
        file_detail::put(bad, 18, 1, code);
        file_detail::put(bad, 88, 8, parameter);
        rehash_header(bad);
        rejects([&] { validate_file<policy>(bad); });
        rejects_open<policy>(path, bad);
      }
      // A valid CRC does not make an unknown selector, impossible code
      // parameter or nonzero remaining reserved byte a valid descriptor.
      for (auto [code, parameter] : std::array<std::pair<std::uint64_t, std::uint64_t>, 5>{
             {{2, 0}, {255, 0}, {0, 64}, {0, maximum}, {1, 0}}}) {
        auto bad = encoded;
        file_detail::put(bad, 18, 1, code);
        file_detail::put(bad, 88, 8, parameter);
        rehash_header(bad);
        rejects([&] { validate_file<policy>(bad); });
        rejects_open<policy>(path, bad);
      }
      for (std::size_t at = 19; at < 24; ++at) {
        auto bad = encoded;
        bad[at] = std::byte{1};
        rehash_header(bad);
        rejects([&] { validate_file<policy>(bad); });
        rejects_open<policy>(path, bad);
      }
    }
  }

  void test_descriptors(std::filesystem::path const & directory) {
    auto path = directory / "descriptor.kv";
    using byte_fixed = storage_policy<profile_unit::byte, fixed_values<5>, 15>;
    using bit_fixed = storage_policy<profile_unit::bit, fixed_values<5>, 15>;
    auto encoded = encode_file(file_header<byte_fixed>{file_kind::native_blob, 25, 3, 5},
                               std::vector<std::byte>(25));
    rejects([&] { validate_file<bit_fixed>(encoded); });
    rejects_open<bit_fixed>(path, encoded);
    rejects_open<storage_policy<profile_unit::byte, fixed_values<6>>>(path, encoded);
    rejects_open<storage_policy<profile_unit::byte, variable_values>>(path, encoded);
    rejects_open<storage_policy<profile_unit::byte, fixed_values<5>, 7>>(path, encoded);
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
      rejects_open<byte_fixed>(path, bad);
    }
    auto bad = encoded;
    file_detail::put(bad, 80, 8, std::numeric_limits<std::uint64_t>::max());
    rehash_header(bad);
    rejects([&] { validate_file<byte_fixed>(bad); });
    rejects_open<byte_fixed>(path, bad);
    bad = encoded;
    file_detail::put(bad, 48, 8, std::numeric_limits<std::uint64_t>::max());
    rehash_header(bad);
    rejects([&] { validate_file<byte_fixed>(bad); });
    rejects_open<byte_fixed>(path, bad);
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
    write(path, padded);
    {
      auto stored = file<bits>::open(path);
      require(stored.header() == bit_header, "padding checked during header-only open");
      rejects([&] { stored.scan(); });
      auto mapping = mapped_file::open(path);
      auto sliced = file<bits>::from_slice(mapping.slice(0, mapping.size()));
      rejects([&] { sliced.scan(); });
    }
    rejects([&] { encode_file(bit_header, std::array<std::byte, 1>{std::byte{0xa1}}); });
    auto empty = encode_file(file_header<bits>{}, {});
    require(empty.size() == 96 && validate_file<bits>(empty).extent == 0, "empty body file");
    write(path, empty);
    file<bits>::open(path).scan();
    using zero_fixed = storage_policy<profile_unit::byte, fixed_values<0>>;
    auto zero = encode_file(file_header<zero_fixed>{file_kind::native_blob, 0, 3, 0}, {});
    require(validate_file<zero_fixed>(zero).common_value_width == 0, "zero fixed width lost");
    write(path, zero);
    file<zero_fixed>::open(path).scan();
  }

#if defined(__unix__) || defined(__APPLE__)
  struct inaccessible_pages {
    void * address;
    std::size_t size;
    explicit inaccessible_pages(std::span<std::byte const> bytes)
      : address(const_cast<std::byte *>(bytes.data())), size(bytes.size()) {
      if (::mprotect(address, size, PROT_NONE) != 0)
        throw std::system_error(errno, std::generic_category(), "protect test payload");
    }
    inaccessible_pages(inaccessible_pages const &) = delete;
    ~inaccessible_pages() {
      if (::mprotect(address, size, PROT_READ) != 0) std::terminate();
    }
  };

  template <class P> void test_inaccessible_body(std::filesystem::path const & directory, file_kind kind) {
    auto page_size = ::sysconf(_SC_PAGESIZE);
    require(page_size > static_cast<long>(file_detail::header_bytes), "invalid test page size");
    auto page = static_cast<std::size_t>(page_size);
    std::vector<std::byte> payload(page + 1, std::byte{0xa8});
    std::uint64_t extent = payload.size();
    if constexpr (P::unit == profile_unit::bit) extent = extent * 8 - 3;
    file_header<P> header{kind, extent, 1, std::nullopt};
    if (kind == file_kind::fractional_index) header.common_value_width = 0;
    auto encoded = encode_file(header, payload);
    auto prefix = page - file_detail::header_bytes;
    std::vector<std::byte> container(prefix);
    container.insert(container.end(), encoded.begin(), encoded.end());
    container.push_back(std::byte{0}); // Permit testing a too-long slice too.
    auto path = directory / ("guarded" + std::string(file_extension(kind)));
    write(path, container);
    auto mapping = mapped_file::open(path);
    auto whole = mapping.slice(0, mapping.size());
    auto guarded = whole.slice(page, payload.size());
    std::optional<file<P>> stored;
    {
      // Put all 96 header bytes before the page boundary. Every payload byte,
      // including bit-profile tail padding, lies on inaccessible pages.
      inaccessible_pages guard(guarded.bytes());
      stored.emplace(file<P>::from_slice(whole.slice(prefix, encoded.size())));
      require(stored->header() == header, "guarded header decode mismatch");
      auto body = stored->body();
      require(body.size() == payload.size() && body.bytes().data() == guarded.bytes().data(),
              "body access copied or changed the mapped payload");
      rejects([&] { file<P>::from_slice(whole.slice(prefix, encoded.size() - 1)); });
      rejects([&] { file<P>::from_slice(whole.slice(prefix, encoded.size() + 1)); });
      if constexpr (P::backspace_code != bit_backspace_code::exponential_golomb || P::backspace_parameter != 0) {
        using defaults = storage_policy<P::unit, typename P::value_layout, P::group_size>;
        rejects([&] { file<defaults>::from_slice(whole.slice(prefix, encoded.size())); });
      }
    }
    stored->scan();
    auto body = stored->body();
    require(std::ranges::equal(body.bytes(), payload), "guard restoration changed payload");
  }

  template <class P> void test_inaccessible_mapping(std::filesystem::path const & directory, file_kind kind) {
    auto page_size = ::sysconf(_SC_PAGESIZE);
    require(page_size > 0, "invalid test page size");
    std::vector<std::byte> payload(static_cast<std::size_t>(page_size) + 1, std::byte{0xa8});
    std::uint64_t extent = payload.size();
    if constexpr (P::unit == profile_unit::bit) extent = extent * 8 - 3;
    file_header<P> header{kind, extent, 1, std::nullopt};
    if (kind == file_kind::fractional_index) header.common_value_width = 0;
    auto encoded = encode_file(header, payload);
    auto path = directory / ("inaccessible" + std::string(file_extension(kind)));
    write(path, encoded);
    mapped_slice retained;
    {
      auto mapping = mapped_file::open(path);
      auto whole = mapping.slice(0, mapping.size());
      std::optional<file<P>> stored;
      {
        // Unlike the checked-open test above, this protects every byte of the
        // mapping, including the header. open() delegates to this same helper.
        inaccessible_pages guard(whole.bytes());
        stored.emplace(file<P>::from_slice(whole, file_open_mode::trusted));
        retained = stored->body();
        require(retained.size() == payload.size() && retained.bytes().data() == whole.bytes().data() + 96,
                "trusted opening or body slicing touched or changed mapped bytes");
        auto copied = *stored;
        require(copied.body().size() == payload.size(), "trusted copy did not preserve mapping");
        auto header_only = file<P>::from_slice(whole.slice(0, 96), file_open_mode::trusted);
        require(header_only.body().empty(), "trusted minimum extent did not give an empty body");
        for (std::uint64_t size = 0; size < 96; ++size)
          rejects([&] { file<P>::from_slice(whole.slice(0, size), file_open_mode::trusted); });
        rejects([&] { file<P>::from_slice(whole, static_cast<file_open_mode>(2)); });
      }
      require(stored->header() == header, "trusted metadata failed after access restored");
      stored->scan();
    }
    require(std::ranges::equal(retained.bytes(), payload), "trusted body pin did not retain mapping");
  }
#endif

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
    rejects([] { object_id::from_hex("tenant/document/field"); });
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
    test_default_headers<profile_unit::byte>({2989498560u, 13510922u});
    test_default_headers<profile_unit::bit>({3121237548u, 150226918u});
    test_backspace_policy<exponential_golomb<1>>(directory.path);
    test_backspace_policy<exponential_golomb<63>>(directory.path);
    test_backspace_policy<golomb<1>>(directory.path);
    test_backspace_policy<golomb<3>>(directory.path);
    test_backspace_policy<golomb<256>>(directory.path);
    test_backspace_policy<golomb<std::numeric_limits<std::uint64_t>::max()>>(directory.path);
    test_descriptors(directory.path);
    test_trusted_validation<storage_policy<profile_unit::byte>>(directory.path);
    test_trusted_validation<storage_policy<profile_unit::bit>>(directory.path);
    test_trusted_validation<storage_policy<profile_unit::bit, variable_values, 15, golomb<3>>>(directory.path);
#if defined(__unix__) || defined(__APPLE__)
    for (auto kind : {file_kind::native_blob, file_kind::fractional_index}) {
      test_inaccessible_body<storage_policy<profile_unit::byte>>(directory.path, kind);
      test_inaccessible_body<storage_policy<profile_unit::bit>>(directory.path, kind);
      test_inaccessible_body<storage_policy<profile_unit::bit, variable_values, 15, exponential_golomb<3>>>(directory.path, kind);
      test_inaccessible_body<storage_policy<profile_unit::bit, variable_values, 15, golomb<3>>>(directory.path, kind);
      test_inaccessible_mapping<storage_policy<profile_unit::byte>>(directory.path, kind);
      test_inaccessible_mapping<storage_policy<profile_unit::bit>>(directory.path, kind);
      test_inaccessible_mapping<storage_policy<profile_unit::bit, variable_values, 15, golomb<3>>>(directory.path, kind);
    }
#endif
    test_paths();
    std::cout << "Header-only files, explicit body scans, retained mappings, and canonical sharded paths passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's files behavior.
 */
