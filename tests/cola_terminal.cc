/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks payload-free terminal routing against the ordinary three-way builder.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/mapped_cola.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace { bool fail_allocation = false; }
void * operator new(std::size_t bytes) {
  if (fail_allocation) throw std::bad_alloc();
  if (auto result = std::malloc(bytes ? bytes : 1)) return result;
  throw std::bad_alloc();
}
void * operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void * pointer) noexcept { std::free(pointer); }
void operator delete[](void * pointer) noexcept { std::free(pointer); }
void operator delete(void * pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void * pointer, std::size_t) noexcept { std::free(pointer); }

#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
namespace {
  using namespace diet;
  void require(bool condition, char const * message) { if (!condition) throw std::runtime_error(message); }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid terminal builder operation accepted");
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto name = (std::filesystem::temp_directory_path() / "diet-cola-terminal-XXXXXX").string();
      auto result = ::mkdtemp(name.data());
      if (!result) throw std::runtime_error("mkdtemp");
      root = result;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  struct unreadable_mapping {
    std::shared_ptr<void const> owner;
    void * base;
    std::size_t bytes;
    explicit unreadable_mapping(std::shared_ptr<void const> source, std::byte const * fc, std::size_t file_bytes)
      : owner(std::move(source)) {
      auto page = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
      // Canonical native FC starts after the 96-byte envelope and 128-byte
      // directory, so its first pointer is still in the mapping's first page.
      auto address = reinterpret_cast<std::uintptr_t>(fc);
      require(address % page == 224, "native mapping has unexpected FC offset");
      base = reinterpret_cast<void *>(address - 224);
      bytes = (file_bytes + page - 1) / page * page;
      require(::mprotect(base, bytes, PROT_NONE) == 0, "protect complete native mapping");
    }
    ~unreadable_mapping() { if (::mprotect(base, bytes, PROT_READ)) std::terminate(); }
  };
  template <class P> std::vector<profile_record> rows(std::uint64_t count) {
    std::vector<profile_record> result;
    for (std::uint64_t i = 0; i < count; ++i) {
      char suffix[32]; std::snprintf(suffix, sizeof suffix, "%012llu", static_cast<unsigned long long>(i));
      auto key = bit_string::from_bytes(std::string(31, 'x') + suffix);
      if constexpr (P::unit == profile_unit::bit) {
        key.bytes.push_back(std::byte(i & 1 ? 0x80 : 0)); ++key.bit_size;
      }
      auto value = P::fixed_width ? bit_string{} : bit_string::from_bytes(std::string(i % 11, 'v'));
      result.push_back({std::move(key), std::move(value)});
    }
    return result;
  }
  template <class P> void fixture(std::uint64_t count) {
    temporary directory;
    object_id native_id(std::string(32, '1')), main_id(std::string(32, '2'));
    object_attempt_id attempt(std::string(32, '3'));
    auto native = std::make_shared<profile_array<P> const>(profile_array<P>::build(rows<P>(count)));
    auto empty = std::make_shared<cola_index<P> const>(cola_index<P>::build({}));
    // A present but empty target forces the unchanged ordinary key walk. Its
    // only wire difference from a terminal is the declared target identity.
    cola_index_builder<P> ordinary(native, empty);
    while (!ordinary.done()) ordinary.step(13);
    auto reference = ordinary.finish();
    auto encoded_reference = encode_cola_sections(reference, native_id, blob_identity{main_id, main_id});
    auto bytes = encoded_reference.materialize();
    auto header = decode_file_header<P>(bytes);
    auto body = std::span(bytes).subspan(96);
    body[82] = std::byte{0};
    std::fill(body.begin() + 104, body.begin() + 136, std::byte{0});
    auto expected = encode_file(header, body);
    auto receipt = encode_native_sections(*native).seal(directory.root, native_id, attempt);
    auto mapped = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(receipt.path));
    auto fc = mapped->view().bytes().data();
    auto file_size = static_cast<std::size_t>(std::filesystem::file_size(receipt.path));
    std::weak_ptr<mapped_native<P> const> pin = mapped;
    {
      unreadable_mapping inaccessible(mapped, fc, file_size);
      mapped_cola_index_builder<P> builder(mapped);
      require(builder.done() == !count && builder.size() == 0, "wrong initial terminal state");
      require(builder.step(0) == 0 && builder.size() == 0, "zero budget changed terminal metadata");
      auto first = builder.step(1);
      require(first == std::min<std::uint64_t>(1, count), "wrong initial charged count");
      auto moved = std::move(builder);
      rejects([&] { builder.step(1); });
      mapped.reset();
      require(!pin.expired(), "terminal move lost native owner");
      std::array<std::uint64_t, 5> budgets{0, 1, P::group_size - 1, P::group_size + 1,
        std::numeric_limits<std::uint64_t>::max()};
      for (auto budget : budgets) {
        auto before = moved.size();
        auto work = moved.step(budget);
        require(work == std::min(budget, count - before) && moved.size() == before + work,
          "terminal step did not charge exact bounded occurrence count");
      }
      require(moved.done(), "unbounded terminal step left input");
      auto output = moved.finish();
      require(output.virtual_size() == count && output.native_owner() == pin.lock(), "terminal output lost native identity");
      require(encode_cola_sections(output, native_id).materialize() == expected,
        "terminal metadata path differs from ordinary key-walk wire");
      rejects([&] { moved.step(1); });
      rejects([&] { (void)moved.finish(); });
      if (count) {
        mapped_cola_index_builder<P> failing(output.native_owner());
        fail_allocation = true;
        bool failed = false;
        try { failing.step(1); } catch (std::bad_alloc const &) { failed = true; }
        fail_allocation = false;
        require(failed && failing.failed() && !pin.expired(), "allocation failure did not poison/retain terminal source");
        rejects([&] { failing.step(1); });
        rejects([&] { (void)failing.finish(); });
      }
    }
    require(pin.expired(), "finished terminal fixtures leaked source ownership");
  }
  template <class P> void run() {
    for (auto count : {std::uint64_t{0}, std::uint64_t{1}, P::group_size - 1, P::group_size,
                       P::group_size + 1, P::group_size * 129 + 1}) fixture<P>(count);
  }
}
#endif
int main() {
#if defined(__APPLE__) || defined(__linux__)
  try {
    run<diet::storage_policy<diet::profile_unit::byte, diet::variable_values, 3, diet::exponential_golomb<0>, 16>>();
    run<diet::storage_policy<diet::profile_unit::bit, diet::fixed_values<0>, 15, diet::exponential_golomb<0>, 16>>();
    std::cout << "Payload-free COLA terminal checks passed\n";
  } catch (std::exception const & error) { fail_allocation = false; std::cerr << error.what() << '\n'; return 1; }
#endif
}
