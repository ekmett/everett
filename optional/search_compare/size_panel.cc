/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks complete-search offset choices across growing working sets.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#define main search_smoke_main
#include "bench.cc"
#undef main
#include <unordered_set>

namespace {
  template <bool Bit> void panel(unsigned count, unsigned width, bool random, unsigned queries, unsigned trials) {
    fixture<Bit> data(count, width, random, queries);
    using P = typename fixture<Bit>::P; using B = typename fixture<Bit>::blob;
    auto root = cola_query_root<P, B>::adopt_prepared(data.head);
    std::unordered_set<std::string_view> distinct;
    u64 query_bytes = 0, expected = 0;
    for (auto const & q : data.hits) {
      require(data.get(root, q.key) == q.expected, "size panel oracle differs");
      distinct.insert(q.key); query_bytes += q.key.size() + q.expected->size(); expected += digest(q.expected);
    }
    std::vector<std::size_t> order(queries); std::iota(order.begin(), order.end(), 0);
    auto const & s = data.footprint;
    std::cerr << "SPACE," << s.files << ',' << s.payload << ',' << s.ef_payload << ',' << s.ef_aux << ','
      << s.rank << ',' << s.cuts << ',' << s.flags << ',' << data.native_ns << ',' << data.index_ns << ',' << s.payload_hash << '\n';
    std::cerr << "QUERIES," << queries << ',' << distinct.size() << ',' << query_bytes << ','
      << queries * sizeof(typename fixture<Bit>::query) << '\n';
    for (unsigned sorted = 0; sorted < 2; ++sorted) {
      if (sorted) std::sort(order.begin(), order.end(), [&](auto a, auto b) { return data.hits[a].key < data.hits[b].key; });
      // Warm each access order separately, and exclude this row from timing.
      for (unsigned trial = 0; trial <= trials; ++trial) {
        auto start = clock_type::now(); u64 checksum = 0;
        for (auto i : order) checksum += digest(data.get(root, data.hits[i].key));
        auto ns = elapsed(start);
        require(checksum == expected, "size panel timed checksum differs");
        if (trial) std::cout << (Bit ? "typed-bit" : "typed-byte") << ',' << (random ? "hash" : "structured") << ','
          << count << ',' << width << ",hit," << (sorted ? "sorted" : "random") << ',' << trial - 1 << ',' << queries << ','
          << std::fixed << std::setprecision(3) << ns / queries << ',' << checksum << '\n';
      }
    }
  }
}

int main(int argc, char ** argv) {
  try {
    if (argc != 7) throw std::runtime_error("usage: size_panel byte|bit records width structured|hash queries trials");
    std::cout << "profile,distribution,records,key_bytes,query_kind,access,trial,queries,ns_per_query,checksum\n";
    auto count = unsigned(std::stoul(argv[2])), width = unsigned(std::stoul(argv[3]));
    auto queries = unsigned(std::stoul(argv[5])), trials = unsigned(std::stoul(argv[6]));
    require(count && width >= 8 && queries, "invalid panel dimensions");
    if (std::string(argv[1]) == "bit") panel<true>(count, width, std::string(argv[4]) == "hash", queries, trials);
    else panel<false>(count, width, std::string(argv[4]) == "hash", queries, trials);
  } catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
}
