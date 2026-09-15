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
  using clock_type = std::chrono::steady_clock;
  void require(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  double elapsed(clock_type::time_point first) {
    return std::chrono::duration<double, std::nano>(clock_type::now() - first).count();
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
  struct footprint {
    std::uint64_t native_bytes = 0, borrowed_bytes = 0, offset_bytes = 0;
    std::uint64_t rank_bytes = 0, cut_bytes = 0, false_borrow_bytes = 0;
    std::uint64_t total() const {
      return native_bytes + borrowed_bytes + offset_bytes + rank_bytes + cut_bytes + false_borrow_bytes;
    }
  };
  template <class C> std::uint64_t array_bytes(C const & values) {
    return values.size() * sizeof(typename C::value_type);
  }
  template <class EF> std::uint64_t directory_bytes(EF const & offsets) {
    return array_bytes(offsets.low) + array_bytes(offsets.high) +
      array_bytes(offsets.samples) + array_bytes(offsets.sparse);
  }
  template <class P> footprint measure(typename fixture<P>::pair current) {
    footprint result;
    std::unordered_set<void const *> native_seen;
    for (; current; current = current->target()) {
      if (native_seen.insert(&current->native()).second) {
        result.native_bytes += current->native().bytes().size();
        result.offset_bytes += directory_bytes(current->native().group_offsets());
      }
      result.borrowed_bytes += current->borrowed().bytes().size();
      result.offset_bytes += directory_bytes(current->borrowed().group_offsets());
      result.rank_bytes += array_bytes(current->interleave().classes) + array_bytes(current->interleave().checkpoints);
      result.false_borrow_bytes += current->false_borrow_bits().size();
      if constexpr (requires { current->cut_lcps(); }) result.cut_bytes += current->cut_lcps().size() * sizeof(std::uint64_t);
    }
    return result;
  }
  template <class P> constexpr std::uint64_t block_width() {
    if constexpr (requires { P::codec_block_size; }) return P::codec_block_size;
    else return P::group_size;
  }
  template <class P> void run(char const * label, unsigned count, unsigned prefix,
                              unsigned query_count, unsigned trials) {
    fixture<P> input(count, prefix, query_count);
    auto warm = everett::query_root<P>::build(input.head);
    input.verify(warm);
    std::uint64_t expected_checksum = 0, expected_matches = 0;
    for (auto const & q : input.queries) for (auto const & match : q.expected) {
      expected_checksum += match_checksum(match);
      ++expected_matches;
    }
    for (unsigned trial = 0; trial != trials; ++trial) {
      auto begin = clock_type::now();
      auto root = everett::query_root<P>::build(input.head);
      auto preparation = elapsed(begin);
      unsigned added = 0;
      for (auto current = root.head(); current != input.head; current = current->target()) {
        require(bool(current) && !current->native().size(), "invalid routing prefix");
        ++added;
      }
      std::uint64_t checksum = 0, matches = 0, visited = 0;
      begin = clock_type::now();
      for (auto const & q : input.queries) {
        auto cursor = root.cursor(q.key.view());
        while (!cursor.done()) {
          visited += cursor.step(1);
          if (!cursor.has_match()) continue;
          auto match = cursor.take_match();
          checksum += match_checksum(match);
          ++matches;
        }
      }
      auto query_time = elapsed(begin) / query_count;
      require(checksum == expected_checksum && matches == expected_matches, "timed query output differs");
      input.verify(root);
      auto footprint = measure<P>(root.head());
      std::cout << label << ',' << P::group_size << ',' << count << ',' << prefix << ','
        << input.head->virtual_size() << ',' << added << ',' << query_count << ',' << trial << ','
        << std::fixed << std::setprecision(3) << preparation << ',' << query_time << ','
        << visited << ',' << matches << ',' << checksum << ',' << block_width<P>() << ','
        << footprint.native_bytes << ',' << footprint.borrowed_bytes << ',' << footprint.offset_bytes << ','
        << footprint.rank_bytes << ',' << footprint.cut_bytes << ',' << footprint.false_borrow_bytes << ','
        << footprint.total() << '\n';
    }
  }
}

int main(int argc, char ** argv) try {
#if defined(__APPLE__)
  require(pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0) == 0, "benchmark QoS request failed");
#endif
  auto count = argc > 1 ? unsigned(std::stoul(argv[1])) : 4096u;
  auto prefix = argc > 2 ? unsigned(std::stoul(argv[2])) : 64u;
  auto queries = argc > 3 ? unsigned(std::stoul(argv[3])) : 4096u;
  auto trials = argc > 4 ? unsigned(std::stoul(argv[4])) : 5u;
  require(count >= 64 && count <= 1048576 && prefix <= 4096 && queries && trials, "invalid benchmark dimensions");
  std::cout << "profile,group_size,base_records,prefix_bytes,head_entries,prefix_catalogs,queries,trial,prepare_ns,query_ns,visited_catalogs,matches,checksum,codec_block_size,native_payload_bytes,borrowed_payload_bytes,offset_array_bytes,rank_array_bytes,cut_lcp_array_bytes,false_borrow_bytes,total_array_bytes\n";
#if defined(EVERETT_QUERY_COMPARE_W16)
  using byte_policy = everett::storage_policy<everett::profile_unit::byte, everett::variable_values, 15, everett::exponential_golomb<0>, 16>;
  using bit_policy = everett::storage_policy<everett::profile_unit::bit, everett::variable_values, 15, everett::exponential_golomb<0>, 16>;
#else
  using byte_policy = everett::storage_policy<everett::profile_unit::byte>;
  using bit_policy = everett::storage_policy<everett::profile_unit::bit>;
#endif
  run<byte_policy>("byte", count, prefix, queries, trials);
  run<bit_policy>("bit", count, prefix, queries, trials);
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares complete queries and backing arrays with an independent integer oracle.
 */
