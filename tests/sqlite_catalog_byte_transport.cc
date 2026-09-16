/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks byte string sessions, transaction branches, saved mappings and durable reopen.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>
#include <everett/typed_scan.h>

#include <cassert>
#include <iostream>

namespace {
  using namespace everett;
  using policy = storage_policy<>;
  using core = active_engine<policy>;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-byte-transport-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action) {
    bool caught = false;
    try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  void durable() {
    temporary directory;
    auto storage = multiverse<policy>::create(directory.root);
    auto db = storage.connect("main");
    auto binary_key = std::string("a\0b", 3), binary_value = std::string("v\0\xff", 3);
    db.put("", ""); db.put("a", "prefix"); db.put(binary_key, binary_value);
    auto first = db.snapshot();
    assert(first.get("") == "" && first.get(binary_key) == binary_value);
    assert(first.metadata().schema_id == "everett.optional-string/tagless/byte-profile-v2");
    db.save("first", first);
    auto tx = db.begin();
    tx.erase(""); tx.put(binary_key, "pending");
    auto frozen = tx.snapshot();
    auto staged = tx.flush();
    assert(!staged.get("") && staged.get(binary_key) == "pending");
    assert(db.get("") == "" && db.get(binary_key) == binary_value);
    tx.put("", "returned"); tx.erase(binary_key); tx.put(binary_key, "latest");
    auto second = tx.commit();
    assert(second.get("") == "returned" && second.get(binary_key) == "latest");
    assert(first.get("") == "" && first.get(binary_key) == binary_value);
    assert(!frozen.get("") && frozen.get(binary_key) == "pending");
    auto stale_branch = frozen.branch();
    stale_branch.put("stale", "no");
    rejects([&] { (void)stale_branch.commit(); });
    assert(!db.failure() && !db.get("stale"));
    auto branch = db.fork("branch", first);
    branch.erase("a"); branch.put(binary_key, "branch");
    assert(branch.get(binary_key) == "branch" && db.get(binary_key) == "latest");
    // Repeated writes force multiple native merge blocks and replacement
    // rebuilding while the earlier mapped world remains retained.
    for (unsigned i = 0; i != 42; ++i) {
      std::string key = "keys/"; key.push_back(char(i));
      db.put(key, std::string(i % 9, char(i)));
    }
    for (unsigned i = 0; i != 32; ++i) {
      std::string key = "keys/"; key.push_back(char(i)); db.erase(key);
    }
    auto final = db.snapshot();
    auto signature = final.signature();
    db.shutdown(); branch.shutdown();
    auto reopened = storage.connect("main", {.create_if_missing = false});
    assert(reopened.get("") == "returned" && reopened.get(binary_key) == "latest");
    assert(reopened.snapshot().signature() == signature && reopened.snapshot().live_count() == 13);
    auto saved = reopened.load("first");
    assert(saved && saved->get("") == "" && saved->get(binary_key) == binary_value);
    auto reopened_branch = storage.connect("branch", {.create_if_missing = false});
    assert(!reopened_branch.get("a") && reopened_branch.get(binary_key) == "branch");
    auto old_schema = connection_options{};
    old_schema.create_if_missing = false;
    old_schema.schema_id = "everett.optional-string/tagless/v1";
    rejects([&] { (void)persistent_engine<core>::connect(storage.root(), "main", old_schema); });
  }
}
int main() try {
  durable();
  std::cout << "durable byte transport: transactions, saved branches, merges, rebuild and reopen passed\n";
} catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
