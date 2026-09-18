/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks imported native storage through a durable connection.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <concepts>
#include <optional>
#include <string>

import everett;
import everett.sqlite;
#if defined(EVERETT_PACKAGE_NEON)
import everett.neon;
using policy = everett::neon_policy<>;
constexpr auto directory = "native-neon-table";
#elif defined(EVERETT_PACKAGE_AVX2)
import everett.avx2;
using policy = everett::avx2_policy<>;
constexpr auto directory = "native-avx2-table";
#elif defined(EVERETT_PACKAGE_AVX512)
import everett.avx512;
using policy = everett::avx512_policy<>;
constexpr auto directory = "native-avx512-table";
#endif

int native_identity(everett::rank15_view const &) {
#if defined(__APPLE__) || defined(__linux__)
  auto storage = everett::multiverse<policy>::create(directory);
  auto db = storage.connect("earth-616");
  static_assert(std::same_as<decltype(db), everett::connection<everett::active_engine<policy>>>);
  for (unsigned i = 0; i < 64; ++i) {
    auto suffix = std::to_string(i);
    db.put("key/" + std::string(4 - suffix.size(), '0') + suffix, "value");
  }
  auto saved = db.snapshot();
  db.save("original", saved);
  db.put("key/0042", "new");
  db.erase("key/0043");
  db.erase_range("key/0008", "key/0016");
  if (saved.live_count() != 64 || saved.get("key/0042") != "value" ||
      saved.get("key/0043") != "value" || db.get("key/0042") != "new" ||
      db.get("key/0043") || db.snapshot().live_count() != 55) return 1;
  unsigned count = 0;
  for (auto row : db.range("key/0040", "key/0045")) {
    if (!row.value || row.key == "key/0043") return 2;
    ++count;
  }
  if (count != 4) return 3;
  auto branch = db.fork("earth-617", saved);
  branch.put("key/0042", "branch");
  if (branch.get("key/0042") != "branch" || db.get("key/0042") != "new") return 4;
  auto loaded = db.load("original");
  if (!loaded || loaded->live_count() != 64 || loaded->get("key/0042") != "value") return 5;
#endif
  return 0;
}
