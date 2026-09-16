/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Registers one exact pair without reopening its registered main suffix.
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
      auto pattern = (std::filesystem::temp_directory_path() / "diet-pair-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  struct hidden_file {
    std::filesystem::path path, away;
    explicit hidden_file(std::filesystem::path value) : path(std::move(value)), away(path.string() + ".away") {
      std::filesystem::rename(path, away);
    }
    ~hidden_file() { std::filesystem::rename(away, path); }
  };
  template<class F> void rejects(F && action) {
    bool caught = false; try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  template<class Storage> auto native(unsigned count) {
    auto result = Storage::empty();
    for (unsigned i = 0; i != count; ++i) {
      char key[16]; std::snprintf(key, sizeof key, "key-%04u", i);
      auto command = core::put(std::string(key), std::string("value"));
      typename Storage::template merge_type<replace_native_value> merge(result, Storage::singleton(command.records()[0]));
      while (!merge.done()) merge.step(1024);
      result = Storage::native_type::from_owned(merge.finish());
    }
    return result;
  }
  template<class Storage> void run() {
    using mapped = typename Storage::mapped_pair_type;
    using node = redundant_node<P, Storage>;
    using index = typename node::built_type;
    temporary dir;
    auto catalog = sqlite_catalog<P>::create_taps(dir.root, id(1));
    unsigned serial = 100;
    auto seal = [&](auto const & encoded, file_kind kind) {
      auto object = id(serial++);
      object_attempt_id attempt(id(serial++).hex());
      std::array reservations{catalog_object_reservation{object, kind}};
      catalog.reserve("reserve-" + object.hex(), attempt, "builder-" + object.hex(), {}, reservations);
      auto receipt = encoded.seal(dir.root, object, attempt);
      catalog.record_sealed("seal-" + object.hex(), receipt);
      return object;
    };
    auto seal_native = [&](auto const & value) {
      return seal(Storage::encode_native(*value->owned()), file_kind::native_blob);
    };
    auto seal_pair = [&](auto const & pair, object_id const & own, std::optional<blob_identity> main = {},
                         std::optional<object_id> secondary = {}) {
      return blob_identity{own, seal(encode_cola_sections(*pair->built(), own, main, secondary), file_kind::fractional_index)};
    };
    auto main_native = native<Storage>(4), own = native<Storage>(3), large = native<Storage>(13);
    auto main = node::from_built(index::adopt_native(main_native));
    auto main_id = seal_pair(main, seal_native(main_native));
    auto own_id = seal_native(own);
    auto secondary_id = seal_native(main_native);
    auto child = node::from_built(index::adopt_native(own, main, main_native));
    auto child_id = seal_pair(child, own_id, main_id, secondary_id);

    // Presence on disk is insufficient: a main target must have an immutable row.
    rejects([&] { catalog.template register_pair<mapped>("child", child_id); });
    assert(!catalog.lookup_operation("child") && !catalog.poisoned());
    catalog.template register_pair<mapped>("main", main_id);
    {
      hidden_file hidden(dir.root / object_path(main_id.index, file_kind::fractional_index));
      catalog.template register_pair<mapped>("child", child_id);
      auto first = catalog.lookup_operation("child"); assert(first);
      catalog.template register_pair<mapped>("child", child_id);
      auto replay = catalog.lookup_operation("child");
      assert(replay && replay->request == first->request && replay->outcome == first->outcome);
      catalog.template register_pair<mapped>("equivalent", child_id);
    }
    // The same rows interoperate with complete-graph registration and recovery.
    mapped_cola_resolver<P, mapped> resolver(dir.root);
    auto loaded = resolver.pair(child_id);
    catalog.register_graph("complete", loaded, catalog_admission::scan);
    catalog.template register_pair<mapped>("after-complete", child_id);
    rejects([&] { catalog.template register_pair<mapped>("child", main_id); });

    auto missing_id = blob_identity{main_id.native, id(9999)};
    auto missing = seal_pair(child, own_id, missing_id, secondary_id);
    rejects([&] { catalog.template register_pair<mapped>("absent-main", missing); });
    auto wrong_native = blob_identity{own_id, main_id.index};
    auto wrong = seal_pair(child, own_id, wrong_native, secondary_id);
    rejects([&] { catalog.template register_pair<mapped>("wrong-main", wrong); });
    auto large_main = node::from_built(index::adopt_native(large));
    auto wrong_samples = node::from_built(index::adopt_native(own, large_main));
    auto wrong_samples_id = seal_pair(wrong_samples, own_id, main_id);
    rejects([&] { catalog.template register_pair<mapped>("main-samples", wrong_samples_id); });
    auto wrong_secondary = node::from_built(index::adopt_native(own, main, large));
    auto wrong_secondary_id = seal_pair(wrong_secondary, own_id, main_id, secondary_id);
    rejects([&] { catalog.template register_pair<mapped>("secondary-samples", wrong_secondary_id); });
    assert(!catalog.poisoned());
    for (auto op : {"absent-main", "wrong-main", "main-samples", "secondary-samples"})
      assert(!catalog.lookup_operation(op));
    catalog.template register_pair<mapped>("still-healthy", child_id);
    // The combined path checks the same exact two-target layout, without
    // reopening the already registered main suffix, and retains its mapping.
    auto fresh = id(serial++); object_attempt_id attempt(id(serial++).hex());
    std::array outputs{catalog_object_reservation{fresh, file_kind::fractional_index}};
    catalog.reserve("atomic-reserve", attempt, "atomic-owner", {}, outputs);
    auto receipt = encode_cola_sections(*child->built(), own_id, main_id, secondary_id).seal(dir.root, fresh, attempt);
    blob_identity atomic{own_id, fresh};
    {
      hidden_file hidden(dir.root / object_path(main_id.index, file_kind::fractional_index));
      auto admitted = catalog.template seal_pair<mapped>("atomic", atomic, receipt);
      assert(admitted->main_id() == main_id && admitted->secondary_id() == secondary_id);
      admitted->scan();
      catalog.template seal_pair<mapped>("atomic", atomic, receipt)->scan();
    }
    mapped_cola_resolver<P, mapped> atomic_resolver(dir.root);
    auto loaded_atomic = atomic_resolver.pair(atomic); loaded_atomic->scan();
    catalog.register_graph("atomic-complete", loaded_atomic, catalog_admission::scan);
  }
}
int main() {
  try { run<profile_runtime_storage<P>>(); run<sort_runtime_storage<P>>(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
