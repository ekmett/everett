/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks paid typed admissions and resolved scans over redundant COLA frontiers.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/redundant_runtime.h>
#include <everett/typed_scan.h>

#include <cassert>
#include <map>

namespace {
  template <class P> void exercise() {
    using engine_type = everett::typed_engine<P, everett::wrapping_fingerprint_algebra, 256, everett::redundant_runtime_family<P>>;
    engine_type engine("typed-redundant/string/1");
    auto original = engine.snapshot();
    std::map<std::string, std::string> expected;
    std::uint64_t random = 0x24756bd;
    for (unsigned i = 0; i != 1025; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      auto key = "prefix/" + std::to_string(random % 81);
      if (!(random & 3) && expected.contains(key)) {
        engine.contribute(engine_type::erase(key)); expected.erase(key);
      } else {
        auto value = std::to_string(i);
        engine.contribute(engine_type::put(key, value)); expected[key] = value;
      }
      assert(engine.admission_ready());
      auto snapshot = engine.snapshot();
      assert(snapshot.live_count() == expected.size() && snapshot.runtime().admissions() == i + 1);
      assert(snapshot.runtime().frontier().service_due == 0);
      if (i % 37 == 0) {
        auto rows = everett::scan(snapshot);
        auto oracle = expected.begin();
        while (auto row = rows.next()) {
          assert(oracle != expected.end() && row->key == oracle->first && row->value == oracle->second);
          assert(snapshot.get(row->key) == row->value); ++oracle;
        }
        assert(oracle == expected.end());
      }
    }
    assert(original.live_count() == 0 && !original.get("prefix/0"));
    auto work = engine.work();
    assert(work.merges && work.native_inputs && work.index_occurrences && work.charged <= work.granted);
    auto restarted = engine_type::from_snapshot(engine.snapshot());
    auto next = restarted.contribute(engine_type::put("reopened", "works"));
    assert(next.get("reopened") == "works" && !engine.snapshot().get("reopened"));
    auto base = engine.snapshot();
    auto a = engine_type::from_snapshot(base), b = engine_type::from_snapshot(base);
    auto left = base.put("partition/a", "a"), right = base.put("partition/b", "b");
    a.contribute(left); a.contribute(right); b.contribute(right); b.contribute(left);
    assert(a.snapshot().signature() == b.snapshot().signature());
    auto before = engine.snapshot();
    try { engine.contribute(engine_type::erase("missing")); assert(false); }
    catch (std::invalid_argument const &) {}
    assert(!engine.failed() && engine.snapshot().signature() == before.signature());
  }
}
int main() {
  exercise<everett::string_policy>();
  using P = everett::storage_policy<everett::string_registry, 3, everett::golomb<3>, 5>;
  exercise<P>();
}
