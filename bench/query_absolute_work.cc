/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/query.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_set>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace {
  void require(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  std::uint64_t random_word(std::uint64_t & state) {
    state += 0x9e3779b97f4a7c15ull;
    auto x = state;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }
  template <class P> std::uint64_t match_checksum(everett::query_match<P> const & match) {
    auto value = match.ordinal + match.value.bit_size + 1;
    for (auto byte : match.value.bytes) value = value * 37 + std::to_integer<unsigned>(byte);
    return value;
  }
  template <class P> everett::bit_string key_for(std::uint64_t id, unsigned prefix) {
    std::string key(prefix, 'p');
    for (unsigned i = 8; i; --i) key.push_back(char((id >> (8 * (i - 1))) & 255));
    auto result = everett::bit_string::from_bytes(key);
    if constexpr (P::unit == everett::profile_unit::bit) {
      result.bytes.push_back((id & 1) ? std::byte{128} : std::byte{0});
      ++result.bit_size;
    }
    return result;
  }
  template <class P> struct fixture {
    using blob = everett::profile_blob<P>;
    using pair = std::shared_ptr<blob const>;
    struct lookup {
      everett::bit_string key;
      std::vector<everett::query_match<P>> expected;
    };
    pair head;
    std::vector<lookup> queries;

    fixture(unsigned count, unsigned prefix, unsigned query_count) {
      std::array<std::vector<std::uint64_t>, 4> ids;
      std::array<std::vector<everett::profile_record>, 4> records;
      std::array<pair, 4> source;
      for (unsigned level = 0; level != 4; ++level) {
        auto stride = std::uint64_t{1} << (2 * level);
        for (unsigned i = 0; i != count / stride; ++i) {
          auto id = i * stride * 4 + (i % 7 ? level : 0);
          ids[level].push_back(id);
          auto value = everett::bit_string::from_bytes(std::to_string(level) + ":" + std::to_string(id));
          records[level].push_back({key_for<P>(id, prefix), std::move(value)});
        }
        source[level] = std::make_shared<blob const>(blob::build(records[level]));
      }
      everett::index_pipeline<P> pipeline(source[0], {source[1], source[2], source[3]});
      while (!pipeline.done()) pipeline.step(4096);
      head = pipeline.finish();
      auto current = head;
      for (unsigned i = 4; i; --i) { source[i - 1] = current; current = current->target(); }
      require(!current, "unexpected source depth");
      std::uint64_t random = 0xb908fcef32571ad9ull;
      for (unsigned i = 0; i != query_count; ++i) {
        auto id = random_word(random) % (std::uint64_t{count} * 4);
        if (!(i & 1)) id = ids[0][random_word(random) % ids[0].size()];
        lookup q{key_for<P>(id, prefix), {}};
        // The oracle searches integer identifiers, independently of encoded
        // key comparison, sampling, rank and offset navigation.
        for (unsigned n = 4; n; --n) {
          auto level = n - 1;
          auto at = std::lower_bound(ids[level].begin(), ids[level].end(), id);
          if (at != ids[level].end() && *at == id) {
            auto ordinal = std::uint64_t(at - ids[level].begin());
            q.expected.push_back({source[level], ordinal, records[level][ordinal].value});
          }
        }
        queries.push_back(std::move(q));
      }
    }
    void verify(everett::query_root<P> const & root) const {
      for (auto const & q : queries) {
        auto cursor = root.cursor(q.key.view());
        std::size_t at = 0;
        while (!cursor.done()) {
          require(cursor.step(1) <= 1, "query work budget");
          if (!cursor.has_match()) continue;
          auto actual = cursor.take_match();
          require(at < q.expected.size(), "unexpected query match");
          auto const & expected = q.expected[at++];
          require(actual.source == expected.source && actual.ordinal == expected.ordinal &&
                  actual.value == expected.value, "query result differs from integer oracle");
        }
        require(at == q.expected.size(), "missing query match");
      }
    }
  };

  template<class P> void audit(char const* label, unsigned count, unsigned prefix, unsigned queries) {
    fixture<P> input(count, prefix, queries);
    auto root = everett::query_root<P>::build(input.head);
    input.verify(root);
    everett::profile_comparison_work native, borrowed;
    std::uint64_t catalogs = 0, matches = 0, checksum = 0, unknown = 0, extra_values = 0;
    for (auto const& query : input.queries) {
      auto current = root.head();
      auto context = everett::profile_query_context<P>(query.key.view());
      std::uint64_t group = 0;
      std::size_t expected_at = 0;
      while (current) {
        auto window = current->project(group);
        auto result = current->search_window(group, context, &native, &borrowed);
        ++catalogs;
        if (result.native) {
          require(expected_at < query.expected.size(), "work audit extra match");
          everett::query_match<P> actual{current, result.native->ordinal, std::move(result.native->value)};
          auto const& expected = query.expected[expected_at++];
          require(actual.source == expected.source && actual.ordinal == expected.ordinal &&
                  actual.value == expected.value, "work audit differs from full integer oracle");
          extra_values += actual.ordinal < window.native_first;
          ++matches; checksum += match_checksum<P>(actual);
        }
        if (!result.borrowed_predecessor) break;
        auto const& next = *result.borrowed_predecessor;
        if constexpr(requires { next.comparison.full_units().has_value(); })
          unknown += !next.comparison.full_units().has_value();
        group = next.target_ordinal / P::group_size;
        context = std::move(result.borrowed_predecessor->comparison);
        current = current->target();
        require(bool(current), "work audit unbound target");
      }
      require(expected_at == query.expected.size(), "work audit missing match");
    }
    std::cout << label << ',' << count << ',' << prefix << ',' << queries << ',' << P::group_size << ','
      << P::codec_block_size << ',' << catalogs << ',' << matches << ',' << checksum << ','
      << native.skipped_headers << ',' << native.visited_headers << ',' << native.compared_bits << ','
      << borrowed.skipped_headers << ',' << borrowed.visited_headers << ',' << borrowed.compared_bits << ','
      << unknown << ',' << extra_values << '\n';
  }
}
int main(int argc, char** argv) try {
  auto count = argc > 1 ? unsigned(std::stoul(argv[1])) : 4096;
  auto prefix = argc > 2 ? unsigned(std::stoul(argv[2])) : 64;
  auto queries = argc > 3 ? unsigned(std::stoul(argv[3])) : 4096;
  require(count >= 64 && count <= 1048576 && prefix <= 4096 && queries, "work dimensions");
  std::cout << "profile,base_records,prefix_bytes,queries,K,W,catalogs,matches,checksum,native_skipped,native_visited,native_compared_bits,borrowed_skipped,borrowed_visited,borrowed_compared_bits,unknown_routes,extra_value_probes\n";
#if defined(EVERETT_QUERY_COMPARE_W16)
  using B = everett::storage_policy<everett::profile_unit::byte, everett::variable_values, 15, everett::exponential_golomb<0>, 16>;
  using I = everett::storage_policy<everett::profile_unit::bit, everett::variable_values, 15, everett::exponential_golomb<0>, 16>;
#else
  using B = everett::storage_policy<everett::profile_unit::byte>;
  using I = everett::storage_policy<everett::profile_unit::bit>;
#endif
  audit<B>("byte", count, prefix, queries); audit<I>("bit", count, prefix, queries);
  return 0;
} catch(std::exception const& e) { std::cerr << e.what() << '\n'; return 1; }
