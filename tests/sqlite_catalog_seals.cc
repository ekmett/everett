/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Verifies exact completed seal receipts without scanning object payloads.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sqlite_catalog.h>

#include <cassert>
#include <fstream>
#include <iostream>

namespace {
  using namespace everett;
  using P = storage_policy<tip<encoded_sort<bit_encoding<>>>>;
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-seals-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template<class F> void rejects(F && action) {
    bool caught = false; try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  template<class Catalog> auto produce(Catalog & catalog, unsigned n) {
    object_attempt_id attempt(id(n + 1).hex());
    std::array reservation{catalog_object_reservation{id(n), file_kind::native_blob}};
    catalog.reserve("reserve", attempt, "builder", {}, reservation);
    std::array records{profile_record{bit_string::from_bytes("key"), bit_string::from_bytes(std::string(32768, 'v'))}};
    auto array = profile_array<P>::build(records);
    return encode_native_sections(array).seal(catalog.root(), id(n), attempt);
  }
  void flip(std::filesystem::path const & path, std::uint64_t offset) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    {
      std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
      stream.seekg(static_cast<std::streamoff>(offset)); char value; stream.read(&value, 1); assert(stream);
      value ^= 1; stream.seekp(static_cast<std::streamoff>(offset)); stream.write(&value, 1); stream.flush(); assert(stream);
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);
  }
  void exact_receipts() {
    temporary dir, foreign;
    auto catalog = sqlite_catalog<P>::create_sessions(dir.root, id(1));
    assert(catalog.identity() == id(1));
    auto receipt = produce(catalog, 10);
    rejects([&] { catalog.verify_sealed(receipt, file_kind::native_blob); }); // File exists, catalog has no completed seal.
    rejects([&] { (void)catalog.sealed_receipt(receipt.object, file_kind::native_blob); });
    rejects([&] { (void)catalog.sealed_receipt(id(999), file_kind::native_blob); });
    catalog.record_sealed("sealed", receipt);
    catalog.verify_sealed(receipt, file_kind::native_blob);
    auto recovered = catalog.sealed_receipt(receipt.object, file_kind::native_blob);
    assert(recovered.object == receipt.object && recovered.attempt == receipt.attempt &&
      recovered.bytes == receipt.bytes && recovered.body_crc32c == receipt.body_crc32c &&
      recovered.barrier == receipt.barrier && recovered.path == receipt.path);
    rejects([&] { (void)catalog.sealed_receipt(receipt.object, file_kind::fractional_index); });
    for (unsigned field = 0; field != 6; ++field) {
      auto changed = receipt;
      switch (field) {
        case 0: changed.object = id(90); break;
        case 1: changed.attempt = object_attempt_id(id(91).hex()); break;
        case 2: ++changed.bytes; break;
        case 3: changed.body_crc32c ^= 1; break;
        case 4: changed.barrier = static_cast<object_sync_barrier>(1u - static_cast<unsigned>(changed.barrier)); break;
        default: changed.path = foreign.root / "alien.kv"; break;
      }
      rejects([&] { catalog.verify_sealed(changed, file_kind::native_blob); });
    }
    rejects([&] { catalog.verify_sealed(receipt, file_kind::fractional_index); });
    assert(!catalog.poisoned());
    auto other = sqlite_catalog<P>::create_sessions(foreign.root, id(2));
    assert(other.identity() != catalog.identity());
    rejects([&] { other.verify_sealed(receipt, file_kind::native_blob); });
    auto reopened = sqlite_catalog<P>::open(dir.root);
    auto moved = std::move(reopened); assert(moved.identity() == id(1));
    rejects([&] { (void)reopened.identity(); });
    moved.verify_sealed(receipt, file_kind::native_blob);
    flip(receipt.path, 64); // The object envelope must still match.
    rejects([&] { moved.verify_sealed(receipt, file_kind::native_blob); });
    rejects([&] { (void)moved.sealed_receipt(receipt.object, file_kind::native_blob); });
    flip(receipt.path, 64);
    moved.verify_sealed(receipt, file_kind::native_blob);
    flip(receipt.path, 8192); // Receipt verification deliberately does not checksum the body.
    moved.verify_sealed(receipt, file_kind::native_blob);
    (void)moved.sealed_receipt(receipt.object, file_kind::native_blob);
    rejects([&] { file<P>::open(receipt.path).scan(); });
  }
  struct failing_ops {
    std::shared_ptr<int> mode;
    int commit(sqlite3 * db) noexcept {
      auto failure = std::exchange(*mode, 0);
      if (failure == 1) return SQLITE_IOERR_FSYNC;
      auto result = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return result == SQLITE_OK && failure == 2 ? SQLITE_IOERR_FSYNC : result;
    }
  };
  void uncertain_seal() {
    for (int mode : {1, 2}) {
      temporary dir; auto state = std::make_shared<int>(0);
      auto catalog = sqlite_catalog<P, failing_ops>::create_sessions(dir.root, id(1), {}, {state});
      auto receipt = produce(catalog, 10); *state = mode;
      try { catalog.record_sealed("sealed", receipt); assert(false); }
      catch (catalog_error const & error) { assert(error.outcome_unknown && error.operation == "sealed"); }
      assert(catalog.poisoned());
      rejects([&] { catalog.verify_sealed(receipt, file_kind::native_blob); });
      auto reopened = sqlite_catalog<P>::open(dir.root);
      if (mode == 1) rejects([&] { reopened.verify_sealed(receipt, file_kind::native_blob); });
      else reopened.verify_sealed(receipt, file_kind::native_blob);
    }
  }
}
int main() {
  try { exact_receipts(); uncertain_seal(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
