/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks canonical multi-root registration with one shared graph closure.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/runtime_store.h>

#include <cassert>
#include <iostream>
#include <set>

int main() {
  using namespace everett;
  using P = storage_policy<tip<encoded_sort<bit_encoding<>>>, 3>;
  auto pattern = (std::filesystem::temp_directory_path() / "everett-graphs-XXXXXX").string();
  if (!::mkdtemp(pattern.data())) return 1;
  struct cleanup {
    std::filesystem::path root;
    ~cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  } directory{pattern};
  try {
    auto storage = runtime_store<P>::create(directory.root);
    cola_runtime<P> runtime;
    for (unsigned i = 0; i != 31; ++i) {
      while (runtime.pending()) runtime.advance(1024);
      (void)runtime.contribute({bit_string::from_bytes(std::to_string(i)), bit_string::from_bytes("value")}, 0);
    }
    auto saved = storage.create_session("many-roots", runtime.snapshot());
    using mapped = mapped_cola_blob<P>;
    std::vector<mapped::pair_type> roots;
    for (auto pair = saved.snapshot.query_root().head()->mapped(); pair; pair = pair->main_target()) roots.push_back(pair);
    assert(roots.size() >= 4);
    auto count = roots.size();
    auto duplicate = roots;
    roots.insert(roots.end(), duplicate.begin(), duplicate.end());
    std::reverse(roots.begin(), roots.end());
    auto catalog = sqlite_catalog<P>::open(directory.root);
    catalog.register_graphs<mapped>("closure", roots, catalog_admission::scan);
    auto recorded = catalog.lookup_operation("closure"); assert(recorded);
    catalog_detail::outcome_reader request{recorded->request};
    assert(request.number() == static_cast<unsigned>(catalog_admission::scan));
    assert(request.number() == count);
    for (std::size_t i = 0; i != count; ++i) { (void)request.field(); (void)request.field(); }
    std::set<std::string> nodes;
    while (!request.data.empty()) {
      (void)request.field(); assert(nodes.insert(request.field()).second);
      if (request.number()) { (void)request.field(); (void)request.field(); }
      if (request.number()) (void)request.field();
      for (unsigned i = 0; i != 5; ++i) (void)request.number();
    }
    assert(nodes.size() == count); // No repeated suffix in the transaction body.
    roots.resize(count); std::reverse(roots.begin(), roots.end());
    catalog.register_graphs<mapped>("closure", roots, catalog_admission::scan);
    auto replayed = catalog.lookup_operation("closure");
    assert(replayed && replayed->request == recorded->request && replayed->outcome == recorded->outcome);
    assert(catalog.find_session("many-roots") == saved.head);
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
