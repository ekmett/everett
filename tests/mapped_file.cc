/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's mapped file behavior.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/mapped_file.h>

#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace {
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F action) {
    bool threw = false;
    try { action(); } catch (std::exception const &) { threw = true; }
    require(threw, "invalid mapping operation accepted");
  }
  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
      std::random_device random;
      for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto candidate = std::filesystem::temp_directory_path() /
                         ("everett-mapped-test-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
      }
      throw std::runtime_error("cannot allocate mapping test directory");
    }
    temporary_directory(temporary_directory const &) = delete;
    ~temporary_directory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  };

  void write(std::filesystem::path const & path, std::string const & value) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
  }

  std::string read(everett::mapped_slice const & slice) {
    auto bytes = slice.bytes();
    if (bytes.empty()) return {};
    return {reinterpret_cast<char const *>(bytes.data()), bytes.size()};
  }

  void test_mapping(std::filesystem::path const & directory) {
    auto path = directory / "records.bin";
    std::string content;
    for (unsigned i = 0; i < 20000; ++i) content.push_back(static_cast<char>(i % 251));
    write(path, content);
    everett::mapped_slice retained;
    {
      auto file = everett::mapped_file::open(path);
      require(file.size() == content.size(), "mapped length mismatch");
      auto all = file.slice(0, file.size());
      require(read(all) == content, "mapped bytes mismatch");
      auto slice = all.slice(4093, 4111);
      retained = slice.slice(7, 3000);
      require(read(retained) == content.substr(4100, 3000), "nested slice mismatch");
      auto end = file.slice(file.size(), 0);
      require(end.bytes().empty(), "empty endpoint slice");
      rejects([&] { file.slice(file.size() + 1, 0); });
      rejects([&] { file.slice(file.size(), 1); });
      rejects([&] { all.slice(10, std::numeric_limits<std::uint64_t>::max()); });
      rejects([&] { all.slice(std::numeric_limits<std::uint64_t>::max(), 1); });
      rejects([&] { slice.slice(slice.size(), 1); });
      auto copied = retained;
      auto moved = std::move(copied);
      require(copied.size() == 0 && copied.bytes().empty(), "moved slice retained invalid bounds");
      require(read(moved) == read(retained), "slice move lost ownership");
      copied = std::move(moved);
      require(moved.empty() && moved.bytes().empty(), "move assignment left invalid bounds");
      require(read(copied) == read(retained), "move assignment lost bytes");
    }
    require(read(retained) == content.substr(4100, 3000), "slice lost mapping with file owner");
#if !defined(_WIN32)
    require(std::filesystem::remove(path), "unlink own mapped file failed");
    require(read(retained) == content.substr(4100, 3000), "unlink invalidated retained mapping");
#endif
    auto temporary_owner_slice = everett::mapped_file::open(directory / "empty.bin").slice(0, 0);
    require(temporary_owner_slice.bytes().empty(), "temporary owner did not transfer pin");
  }

  void test_empty_and_failures(std::filesystem::path const & directory) {
    auto path = directory / "empty.bin";
    write(path, {});
    auto file = everett::mapped_file::open(path);
    require(file.size() == 0, "empty file length");
    auto empty = file.slice(0, 0);
    require(empty.bytes().empty(), "empty mapping bytes");
    rejects([&] { file.slice(0, 1); });
    rejects([&] { everett::mapped_file::open(directory / "missing"); });
    rejects([&] { everett::mapped_file::open(directory); });
    everett::mapped_file default_file;
    auto default_slice = default_file.slice(0, 0);
    require(default_slice.bytes().empty(), "default empty mapping");
#if !defined(_WIN32)
    auto fifo = directory / "not-a-regular-file";
    if (::mkfifo(fifo.c_str(), 0600) == -1)
      throw std::system_error(errno, std::generic_category(), "create own test FIFO");
    rejects([&] { everett::mapped_file::open(fifo); });
#endif
  }

  void test_repeated_lifetimes(std::filesystem::path const & directory) {
    auto path = directory / "repeated.bin";
    write(path, "persistent bytes");
    std::vector<everett::mapped_slice> slices;
    for (unsigned i = 0; i < 64; ++i) {
      auto file = everett::mapped_file::open(path);
      slices.push_back(file.slice(0, file.size()));
    }
    for (auto const & slice : slices) require(read(slice) == "persistent bytes", "retained repeated open");
    slices.clear();
    for (unsigned i = 0; i < 2048; ++i) {
      auto file = everett::mapped_file::open(path);
      auto slice = file.slice(0, file.size());
      require(read(slice) == "persistent bytes", "repeated release/reopen");
    }
  }
}

int main() {
  static_assert(std::is_same_v<decltype(std::declval<everett::mapped_slice const &>().bytes()),
                               std::span<std::byte const>>);
  try {
    temporary_directory directory;
    test_empty_and_failures(directory.path);
    test_mapping(directory.path);
    test_repeated_lifetimes(directory.path);
    std::cout << "Read-only mappings, bounded retained slices, and native lifetime checks passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
