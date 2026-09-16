/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks sorted scans over reopened mappings and retained named snapshots.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/connection.h>
#include <diet/typed_scan.h>

#include <cassert>
#include <map>

int main() {
  auto path = std::filesystem::temp_directory_path() / ("diet-scan-" + diet::random_object_ids{}().hex());
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } guard{path};
  auto fridge = diet::fridge<>::create(path);
  std::map<std::string, std::string> expected;
  {
    auto db = fridge.connect("scan");
    auto batch = decltype(db)::core_type::batch();
    for (unsigned i = 0; i != 65; ++i) {
      auto key = "key/" + std::to_string(i), value = "value/" + std::to_string(i);
      batch.put(key, value); expected[key] = value;
    }
    db.apply(std::move(batch).finish());
    auto removals = db.snapshot().batch();
    for (unsigned i = 0; i < 65; i += 3) { auto key = "key/" + std::to_string(i); removals.erase(key); expected.erase(key); }
    db.apply(std::move(removals).finish());
    db.save("frozen");
  }
  auto rows = [&] {
    auto db = fridge.connect("scan");
    auto saved = db.load("frozen");
    assert(saved && saved->live_count() == expected.size());
    auto rows = diet::scan(*saved);
    rows.step(3);
    db.put("key/0", "new incarnation");
    return rows;
  }();
  auto oracle = expected.begin();
  while (auto row = rows.next()) {
    assert(oracle != expected.end() && row->key == oracle->first && row->value == oracle->second);
    ++oracle;
  }
  assert(oracle == expected.end());
}
