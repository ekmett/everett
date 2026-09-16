/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks that snapshot copies retain shared heads without copying metadata or dependencies.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/connection.h>
#include <diet/replacement_rebuild.h>

#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
  thread_local bool forbid_allocation = false;
  void * allocate(std::size_t size) {
    if (forbid_allocation) throw std::bad_alloc();
    if (auto value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
  }
  struct without_allocation {
    without_allocation() { forbid_allocation = true; }
    ~without_allocation() { forbid_allocation = false; }
  };
}
void * operator new(std::size_t n) { return allocate(n); }
void * operator new[](std::size_t n) { return allocate(n); }
void operator delete(void * p) noexcept { std::free(p); }
void operator delete[](void * p) noexcept { std::free(p); }
void operator delete(void * p, std::size_t) noexcept { std::free(p); }
void operator delete[](void * p, std::size_t) noexcept { std::free(p); }

namespace {
  template <class Core> void snapshots() {
    using namespace diet;
    using core = Core;
    using Family = typename core::runtime_family;
    using cola = typename core::cola_type;
    using saved_cola = stored_cola<cola>;
    static_assert(std::is_nothrow_copy_constructible_v<cola>);
    static_assert(std::is_nothrow_copy_assignable_v<cola>);
    static_assert(std::is_nothrow_copy_constructible_v<saved_cola>);
    std::weak_ptr<typename Family::node_type const> head;
    std::optional<saved_cola> saved;
    {
      core table(std::string(4096, 's'));
      for (unsigned i = 0; i != 37; ++i)
        table.contribute(core::put("key-" + std::to_string(i), "value-" + std::to_string(i)));
      auto source = table.snapshot();
      head = source.runtime().query_root().head();
      object_id id("0123456789abcdef0123456789abcdef");
      catalog_tap_head catalog{{std::string(4096, 'n'), 1, {id, id}, std::string(4096, 'o')},
        std::vector<std::byte>(1024 * 1024, std::byte{42}), {{}, {}}};
      catalog.auxiliary.pairs.assign(128, {id, id});
      catalog.auxiliary.natives.assign(128, id);
      saved.emplace(source, std::move(catalog));
      std::array<std::optional<saved_cola>, 1024> copies;
      {
        without_allocation guard;
        auto another = table.snapshot();
        assert(&another.metadata() == &source.metadata());
        assert(&another.runtime() == &source.runtime());
        for (auto & copy : copies) {
          copy.emplace(*saved);
          assert(&copy->head() == &saved->head());
          assert(&copy->metadata() == &saved->metadata());
          assert(copy->runtime().query_root().head() == source.runtime().query_root().head());
        }
        copies.back() = copies.front();
      }
      table.contribute(core::put("key-0", "changed"));
      assert(saved->get("key-0") == "value-0" && table.snapshot().get("key-0") == "changed");
    }
    assert(!head.expired() && saved->get("key-36") == "value-36");
    saved.reset();
    assert(head.expired());
  }
}
int main() {
  try {
    snapshots<diet::typed_engine<>>();
    snapshots<diet::typed_engine<diet::string_policy, diet::wrapping_fingerprint_algebra, 256,
      diet::sort_runtime_family<diet::string_policy>>>();
    snapshots<diet::replacement_rebuild_engine<>>();
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
