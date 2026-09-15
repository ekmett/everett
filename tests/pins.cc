/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Diet's storage pins behavior.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include "diet/pins.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
  using namespace diet;
  using owner = pin_set<int>;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  template <class F> void rejects(F && operation, char const * message) {
    try { operation(); }
    catch (std::invalid_argument const &) { return; }
    throw std::runtime_error(message);
  }

  void contributions() {
    auto first = std::make_shared<int const>(1);
    auto update = std::make_shared<int const>(2);
    auto negative = wrapping_fingerprint_algebra::subtract(0, 7);
    auto base = owner{}.add({"base", first, 20, 20});
    auto changed = base.add({"delete", update, negative, 0});
    require(changed.signature() == 13, "deletion's negative contribution enters the global sum");
    require(changed.find("delete")->native_signature == 0,
      "zero native tombstone hash remains separate from additive contribution");
    require(changed.recompute_signature() == changed.signature(), "cached total equals entry sum");
    require(base.size() == 1 && base.signature() == 20, "adding a pin preserves old owner snapshot");
    auto removed = changed.remove("delete");
    require(removed.signature() == 20 && removed.find("base")->object == first,
      "removal subtracts only that object's contribution");
    require(changed.size() == 2, "removal preserves earlier owner snapshot");

    auto equal = changed.add({"another-base", std::make_shared<int const>(3), 20, 20});
    require(equal.size() == 3 && equal.signature() == 33,
      "distinct object identities are retained even when signatures match");
    require(equal.find("base")->object != equal.find("another-base")->object,
      "equal contributions do not identify physical objects");
    rejects([&] { changed.add({"base", update, 999, 0}); }, "duplicate exact object identity rejected");
    rejects([&] { changed.add({"null", {}, 0, {}}); }, "null pins rejected");
    rejects([&] { changed.add({"", update, 0, {}}); }, "empty identity rejected");
    rejects([&] { changed.remove("missing"); }, "missing removal rejected");
    require(changed.signature() == 13 && changed.size() == 2, "failed mutations preserve owner");
  }

  void merging_and_lifetime() {
    std::weak_ptr<int const> old_object;
    auto saved = owner{};
    auto merged = owner{};
    {
      auto object = std::make_shared<int const>(1);
      old_object = object;
      auto before = owner{}.add({"old", object, 12, 12})
        .add({"delta", std::make_shared<int const>(2), 8, 27})
        .add({"unrelated", std::make_shared<int const>(3), 5, 5});
      saved = before;
      std::array<std::string, 2> inputs{"old", "delta"};
      merged = before.replace(inputs, {{"merged", std::make_shared<int const>(4), 20, 20}});
      require(merged.signature() == before.signature() && merged.recompute_signature() == 25,
        "replacement preserves the sum of replaced contributions");
      require(merged.size() == 2 && merged.find("unrelated")->object == before.find("unrelated")->object,
        "replacement preserves unrelated exact pins");
      require(!merged.find("old") && !merged.find("delta") && merged.find("merged"),
        "all merge inputs switched together to the output");
      rejects([&] { before.replace(inputs, {{"wrong", std::make_shared<int const>(5), 19, 19}}); },
        "compaction cannot silently change contribution total");
      std::array<std::string, 2> duplicate{"old", "old"};
      rejects([&] { before.replace(duplicate, {{"wrong", object, 24, 24}}); },
        "expected inputs cannot contain duplicates");
      std::array<std::string, 1> missing{"not-here"};
      rejects([&] { before.replace(missing, {}); }, "stale missing merge inputs rejected");
      rejects([&] { before.replace(inputs, {{"unrelated", object, 20, 20}}); },
        "replacement cannot take an existing identity");
      rejects([&] { before.replace(inputs, {{"old", object, 20, 20}}); },
        "replacement cannot overwrite an input identity");
      rejects([&] { before.replace(inputs, {{"new", object, 10, 10}, {"new", object, 10, 10}}); },
        "replacement identities must be distinct");
      require(before.size() == 3 && before.signature() == 25, "failed replacements are atomic");
    }
    require(!old_object.expired(), "saved owner keeps old physical objects alive after merge");
    saved = owner{};
    require(old_object.expired(), "releasing last old owner reclaims old physical object");
    require(merged.signature() == 25, "independent merged owner survives");
  }
}

int main() {
  try {
    contributions();
    merging_and_lifetime();
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
