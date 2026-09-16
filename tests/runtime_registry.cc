/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks incremental owner retention, exact identities and allocation rollback.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/runtime_registry.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
  thread_local std::ptrdiff_t fail_after = -1;
  void * allocate(std::size_t n) {
    if (!fail_after) throw std::bad_alloc();
    if (fail_after > 0) --fail_after;
    if (auto result = std::malloc(n ? n : 1)) return result;
    throw std::bad_alloc();
  }
}
void * operator new(std::size_t n) { return allocate(n); }
void * operator new[](std::size_t n) { return allocate(n); }
void operator delete(void * p) noexcept { std::free(p); }
void operator delete[](void * p) noexcept { std::free(p); }
void operator delete(void * p, std::size_t) noexcept { std::free(p); }
void operator delete[](void * p, std::size_t) noexcept { std::free(p); }

namespace {
  using namespace everett;
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  struct index {
    std::optional<object_id> secondary;
    auto const & secondary_id() const noexcept { return secondary; }
  };
  struct physical {
    blob_identity id;
    index navigation;
    auto const & identity() const noexcept { return id; }
    auto index_object() const noexcept { return &navigation; }
  };
  struct node {
    using pair_type = std::shared_ptr<node const>;
    using native_pointer = std::shared_ptr<int const>;
    physical file;
    native_pointer own, side;
    pair_type main;
    auto mapped() const noexcept { return &file; }
    auto const & native_owner() const noexcept { return own; }
    auto const & main_target() const noexcept { return main; }
    auto const & secondary_target() const noexcept { return side; }
  };
  node::pair_type make(unsigned n, node::pair_type main = {}) {
    return std::make_shared<node const>(node{{{id(2 * n), id(2 * n + 1)}, {}}, std::make_shared<int const>(n), {}, std::move(main)});
  }
  struct counts { unsigned pairs_in = 0, pairs_out = 0, natives_in = 0, natives_out = 0; };
  struct visitor {
    counts * value;
    void operator()(bool pair, bool acquire) const noexcept {
      if (pair) ++(acquire ? value->pairs_in : value->pairs_out);
      else ++(acquire ? value->natives_in : value->natives_out);
    }
  };
  using registry = runtime_store_detail::runtime_registry<node, visitor>;
  template <class F> void rejects(F && action) {
    bool caught = false; try { action(); } catch (std::invalid_argument const &) { caught = true; }
    assert(caught);
  }
  void sharing() {
    counts seen; registry kept(visitor{&seen});
    node::pair_type tail;
    for (unsigned n = 1; n != 513; ++n) tail = make(n, std::move(tail));
    assert(kept.replace({tail}, {}));
    assert(seen.pairs_in == 512 && seen.natives_in == 512);
    for (unsigned n = 0; n != 100; ++n) assert(kept.replace({tail}, {}));
    assert(seen.pairs_in == 512 && seen.pairs_out == 0 && seen.natives_in == 512);
    auto next = make(513, tail); std::weak_ptr<node const> expired = next;
    assert(kept.replace({next, tail}, {}));
    assert(seen.pairs_in == 513 && seen.natives_in == 513);
    auto wrong = next->mapped()->identity(); wrong.native = id(9999);
    rejects([&] { (void)kept.pair(wrong); });
    assert(kept.replace({tail}, {})); next.reset(); assert(expired.expired());
    assert(seen.pairs_out == 1 && seen.natives_out == 1);
    assert(kept.pair_count() == 512 && kept.native_count() == 512);

    // A distinct facade for a live identity is found after many new edges.
    auto duplicate = std::make_shared<node const>(*tail);
    node::pair_type invalid = duplicate;
    for (unsigned n = 600; n != 632; ++n) invalid = make(n, std::move(invalid));
    assert(!kept.replace({invalid}, {}));
    assert(kept.pair_count() == 512 && kept.native_count() == 512);
    assert(kept.pair(tail->mapped()->identity()) == tail);
    assert(!kept.pair(invalid->mapped()->identity()));

    // The same native ID under distinct facade owners also conflicts.
    auto copy = *make(700, tail);
    copy.file.id.native = tail->mapped()->identity().native;
    assert(!kept.replace({std::make_shared<node const>(std::move(copy))}, {}));
    assert(kept.pair_count() == 512 && kept.native_count() == 512);
    auto a = std::make_shared<int const>(1), b = std::make_shared<int const>(2);
    assert(!kept.replace({make(801, tail)}, {{id(9999), a}, {id(9999), b}}));
    assert(kept.pair_count() == 512 && kept.native_count() == 512 && !kept.native(id(9999)));
    kept.clear(); assert(!kept.pair_count() && !kept.native_count());
    // Registry retirement does not invalidate the caller's immutable graph.
    assert(tail->main_target() && *tail->native_owner() == 512);

    auto side = *make(800, tail);
    side.side = tail->native_owner();
    side.file.navigation.secondary = tail->mapped()->identity().native;
    auto branch = std::make_shared<node const>(std::move(side));
    auto detached = std::make_shared<int const>(99);
    assert(kept.replace({branch}, {{id(9999), detached}}));
    assert(kept.pair_count() == 513 && kept.native_count() == 514);
    // The main edge and terminal secondary edge share one native entry. Its
    // references are independent of this unrelated explicit native root.
    assert(kept.replace({}, {{id(9999), detached}}));
    assert(!kept.pair_count() && kept.native_count() == 1);
    assert(!kept.native(tail->mapped()->identity().native));
    assert(kept.native(id(9999)) == detached);
    kept.clear(); assert(!kept.native_count());
  }
  void allocations() {
    auto old = make(1); node::pair_type next = old;
    for (unsigned n = 2; n != 18; ++n) next = make(n, std::move(next));
    unsigned failures = 0;
    for (std::ptrdiff_t point = 0; point != 300; ++point) {
      counts seen; registry kept(visitor{&seen}); assert(kept.replace({old}, {}));
      std::vector<node::pair_type> roots{next};
      bool success = false;
      fail_after = point;
      try { success = kept.replace(std::move(roots), {}); }
      catch (std::bad_alloc const &) { ++failures; }
      catch (...) { fail_after = -1; throw; }
      fail_after = -1;
      if (success) {
        assert(kept.pair_count() == 17 && kept.native_count() == 17); break;
      }
      assert(kept.pair_count() == 1 && kept.native_count() == 1);
      assert(kept.pair(old->mapped()->identity()) == old && !kept.pair(next->mapped()->identity()));
    }
    assert(failures > 30 && failures < 300);
  }
}
int main() {
  try { sharing(); allocations(); }
  catch (std::exception const & error) { fail_after = -1; std::cerr << error.what() << '\n'; return 1; }
}
