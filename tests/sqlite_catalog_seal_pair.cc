/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks atomic index sealing, pair admission, replay and uncertain commits.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sqlite_catalog.h>
#include <diet/sort_runtime.h>

#include <cassert>
#include <iostream>

namespace {
  using namespace diet;
  using P = storage_policy<string_registry, 3>;
  using core = typed_engine<P, wrapping_fingerprint_algebra, 256, sort_runtime_family<P>>;
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-atomic-pair-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template<class F> void rejects(F && action) {
    bool caught = false; try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  struct fault_ops {
    bool after;
    int commit(sqlite3 * db) noexcept {
      if (!after) return SQLITE_IOERR_FSYNC;
      auto code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return code == SQLITE_OK ? SQLITE_IOERR_FSYNC : code;
    }
  };
  void sql(std::filesystem::path const & root, char const * command) {
    sqlite3 * db = nullptr;
    assert(sqlite3_open((root / "catalog.sqlite3").c_str(), &db) == SQLITE_OK);
    auto result = sqlite3_exec(db, command, nullptr, nullptr, nullptr);
    assert(sqlite3_close(db) == SQLITE_OK); assert(result == SQLITE_OK);
  }
  template<class Storage> void run() {
    using mapped = typename Storage::mapped_pair_type;
    using node = redundant_node<P, Storage>;
    for (unsigned mode = 0; mode != 3; ++mode) {
      temporary dir; auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
      auto input = core::put("key", "value");
      auto native = Storage::singleton(input.records()[0]);
      auto pair = node::from_built(node::built_type::adopt_native(native));
      auto reserve = [&](unsigned object, file_kind kind) {
        std::array outputs{catalog_object_reservation{id(object), kind}};
        object_attempt_id attempt(id(object + 1).hex());
        catalog.reserve("reserve-" + std::to_string(object), attempt, "owner-" + std::to_string(object), {}, outputs);
        return attempt;
      };
      auto native_receipt = Storage::encode_native(*native->owned()).seal(dir.root, id(2), reserve(2, file_kind::native_blob));
      catalog.record_sealed("native", native_receipt);
      blob_identity identity{id(2), id(4)};
      auto receipt = encode_cola_sections(*pair->built(), id(2)).seal(dir.root, id(4), reserve(4, file_kind::fractional_index));
      std::shared_ptr<typename mapped::index_type const> published;
      if (!mode) {
        auto bad = receipt; bad.attempt = object_attempt_id(id(99).hex());
        rejects([&] { catalog.template seal_pair<mapped>("bad-attempt", identity, bad); });
        bad = receipt; ++bad.bytes;
        rejects([&] { catalog.template seal_pair<mapped>("bad-size", identity, bad); });
        bad = receipt; bad.body_crc32c ^= 1;
        rejects([&] { catalog.template seal_pair<mapped>("bad-crc", identity, bad); });
        bad = receipt; bad.barrier = static_cast<object_sync_barrier>(2);
        rejects([&] { catalog.template seal_pair<mapped>("bad-barrier", identity, bad); });
        bad = receipt; bad.path = native_receipt.path;
        rejects([&] { catalog.template seal_pair<mapped>("bad-path", identity, bad); });
        bad = receipt; bad.object = id(99);
        rejects([&] { catalog.template seal_pair<mapped>("bad-id", identity, bad); });
        for (auto name : {"bad-attempt", "bad-size", "bad-crc", "bad-barrier", "bad-path", "bad-id"})
          assert(!catalog.lookup_operation(name));
        rejects([&] { catalog.sealed_receipt(id(4), file_kind::fractional_index); });
        assert(!catalog.poisoned());

        // Fail after the seal UPDATE, at the pair INSERT. Both rows must roll
        // back together, and the same healthy connection can try again.
        sql(dir.root, "CREATE TRIGGER reject_pair BEFORE INSERT ON pairs BEGIN SELECT RAISE(ABORT,'test pair rejection'); END");
        rejects([&] { catalog.template seal_pair<mapped>("rollback", identity, receipt); });
        assert(!catalog.poisoned() && !catalog.lookup_operation("rollback"));
        rejects([&] { catalog.sealed_receipt(id(4), file_kind::fractional_index); });
        rejects([&] { catalog.save("not-registered", "invalid", identity); });
        sql(dir.root, "DROP TRIGGER reject_pair");
        published = catalog.template seal_pair<mapped>("atomic", identity, receipt);
      } else {
        auto faulty = sqlite_catalog<P, fault_ops>::open(dir.root, {}, fault_ops{mode == 2});
        try { published = faulty.template seal_pair<mapped>("atomic", identity, receipt); assert(false); }
        catch (catalog_error const & error) { assert(error.outcome_unknown && error.operation == "atomic"); }
        assert(faulty.poisoned() && !published);
        auto reopened = sqlite_catalog<P>::open(dir.root);
        assert(bool(reopened.lookup_operation("atomic")) == (mode == 2));
        if (mode == 1) {
          rejects([&] { reopened.sealed_receipt(id(4), file_kind::fractional_index); });
          rejects([&] { reopened.save("unregistered", "invalid", identity); });
        } else {
          assert(reopened.sealed_receipt(id(4), file_kind::fractional_index).bytes == receipt.bytes);
          reopened.save("registered", "committed", identity);
        }
        // Before COMMIT this applies; after COMMIT it is exact operation replay.
        published = reopened.template seal_pair<mapped>("atomic", identity, receipt);
      }
      assert(published && published->native_id() == id(2)); published->scan();
      auto first = catalog.lookup_operation("atomic"); assert(first && first->kind == "seal_cola_pair");
      auto replay = catalog.template seal_pair<mapped>("atomic", identity, receipt);
      assert(replay && replay != published && replay->native_id() == id(2));
      assert(catalog.lookup_operation("atomic")->request == first->request);
      catalog.template register_pair<mapped>("standalone", identity);
      rejects([&] { catalog.record_sealed("already-sealed", receipt); });
      auto bad = receipt; bad.attempt = object_attempt_id(id(98).hex());
      rejects([&] { catalog.template seal_pair<mapped>("atomic", identity, bad); });
      rejects([&] { catalog.template seal_pair<mapped>("atomic", {id(99), id(4)}, receipt); });

      // Even exact replay rechecks the retained native envelope against its
      // sealed catalog CRC, without scanning body bytes or repeating mutations.
      auto mapping = mapped_file::open(native_receipt.path);
      auto slice = mapping.slice(0, mapping.size());
      auto raw = slice.bytes();
      std::array<std::byte, file_detail::header_bytes> original;
      std::copy_n(raw.begin(), original.size(), original.begin());
      auto header = decode_file_header<P>(original);
      auto altered = encode_file_header(header, native_receipt.body_crc32c ^ 1);
      assert(::chmod(native_receipt.path.c_str(), 0600) == 0);
      int fd = ::open(native_receipt.path.c_str(), O_WRONLY | O_CLOEXEC); assert(fd >= 0);
      assert(::pwrite(fd, altered.data(), altered.size(), 0) == static_cast<ssize_t>(altered.size()));
      rejects([&] { catalog.template seal_pair<mapped>("atomic", identity, receipt); });
      assert(::pwrite(fd, original.data(), original.size(), 0) == static_cast<ssize_t>(original.size()));
      assert(::close(fd) == 0); assert(::chmod(native_receipt.path.c_str(), 0400) == 0);
      catalog.template seal_pair<mapped>("atomic", identity, receipt)->scan();
    }
  }
}
int main() {
  try { run<profile_runtime_storage<P>>(); run<sort_runtime_storage<P>>(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
