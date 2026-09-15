/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures encoded blob construction, index pipelines and known-window searches.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/index_pipeline.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace {
  using clock_type = std::chrono::steady_clock;
  void require(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  std::uint64_t random_word(std::uint64_t & state) {
    state += 0x9e3779b97f4a7c15ull;
    auto value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
  }
  struct digest {
    std::uint64_t value = 0xcbf29ce484222325ull;
    std::uint64_t bytes = 0;
    void byte(unsigned v) { value = (value ^ v) * 0x100000001b3ull; ++bytes; }
    void integer(std::uint64_t v) {
      for (unsigned i = 0; i != 8; ++i) byte(unsigned((v >> (i * 8)) & 255));
    }
    void data(std::span<std::byte const> values) {
      integer(values.size());
      for (auto v : values) byte(std::to_integer<unsigned>(v));
    }
    template <class V> void words(V const & values) {
      integer(values.size());
      for (auto v : values) integer(v);
    }
  };
  template <class P> diet::bit_string key_for(std::uint64_t id, unsigned prefix) {
    std::string text(prefix, 'p');
    for (unsigned i = 8; i; --i) text.push_back(char((id >> ((i - 1) * 8)) & 255));
    auto result = diet::bit_string::from_bytes(text);
    if constexpr (P::unit == diet::profile_unit::bit) {
      // Build the oracle input directly, independently of the bit-copy helpers.
      result.bytes.push_back((id & 1) ? std::byte{128} : std::byte{0});
      ++result.bit_size;
    }
    return result;
  }
  template <class P> diet::bit_string value_for(std::uint64_t id) {
    auto units = P::value_width.value_or(8 + id % 17);
    auto bits = units * P::bits_per_unit;
    diet::bit_string value;
    value.bit_size = bits;
    value.bytes.resize(static_cast<std::size_t>(bits / 8 + (bits % 8 != 0)));
    for (std::size_t i = 0; i != value.bytes.size(); ++i)
      value.bytes[i] = std::byte((id * 37 + i * 91) & 255);
    if (bits % 8) value.bytes.back() &= std::byte(255u << (8 - bits % 8));
    return value;
  }
  template <class P> struct fixture {
    using blob = diet::profile_blob<P>;
    using pair = std::shared_ptr<blob const>;
    struct entry { std::uint64_t id; bool borrowed; };
    struct lookup {
      diet::bit_string key;
      diet::bit_string boundary;
      std::uint64_t group;
      std::optional<std::uint64_t> native;
      std::optional<std::uint64_t> borrowed;
      diet::bit_string value;
    };
    std::array<std::vector<std::uint64_t>, 4> ids;
    std::array<std::vector<diet::profile_record>, 4> records;
    std::array<std::vector<std::uint64_t>, 4> borrowed_ids;
    std::array<pair, 4> sources;
    std::vector<lookup> queries;

    fixture(unsigned count, unsigned prefix, unsigned query_count) {
      std::uint64_t stride = 4;
      for (unsigned level = 0; level != 4; ++level) {
        for (std::uint64_t i = 0; i != count; ++i) {
          auto id = i * stride + (i % 7 ? level : 0);
          ids[level].push_back(id);
          records[level].push_back({key_for<P>(id, prefix), value_for<P>(id)});
        }
        sources[level] = std::make_shared<blob const>(blob::build(records[level]));
        count = std::max(1u, count / 4);
        stride *= 4;
      }
      for (unsigned level = 1; level != 4; ++level) {
        auto catalog = catalog_for(level - 1);
        for (std::size_t i = 0; i < catalog.size(); i += P::group_size)
          borrowed_ids[level].push_back(catalog[i].id);
      }
      auto catalog = catalog_for(3);
      std::uint64_t seed = 0x1926081742ull;
      for (unsigned i = 0; i != query_count; ++i) {
        auto random = random_word(seed);
        auto id = i % 2 ? random % (ids[0].back() + 5) : ids[3][random % ids[3].size()];
        auto upper = std::upper_bound(catalog.begin(), catalog.end(), id,
          [](auto value, entry const & e) { return value < e.id; });
        auto at = upper == catalog.begin() ? 0 : std::uint64_t(upper - catalog.begin() - 1);
        lookup q{key_for<P>(id, prefix), key_for<P>(catalog[at / P::group_size * P::group_size].id, prefix),
          at / P::group_size, {}, {}, {}};
        auto native = std::lower_bound(ids[3].begin(), ids[3].end(), id);
        if (native != ids[3].end() && *native == id) {
          q.native = std::uint64_t(native - ids[3].begin());
          q.value = value_for<P>(id);
        }
        auto borrowed = std::upper_bound(borrowed_ids[3].begin(), borrowed_ids[3].end(), id);
        if (borrowed != borrowed_ids[3].begin())
          q.borrowed = std::uint64_t(borrowed - borrowed_ids[3].begin() - 1);
        queries.push_back(std::move(q));
      }
    }
    std::vector<entry> catalog_for(unsigned level) const {
      std::vector<entry> result;
      for (auto id : ids[level]) result.push_back({id, false});
      for (auto id : borrowed_ids[level]) result.push_back({id, true});
      std::stable_sort(result.begin(), result.end(), [](entry const & a, entry const & b) {
        return a.id != b.id ? a.id < b.id : a.borrowed < b.borrowed;
      });
      return result;
    }
    pair build_chain() const {
      diet::index_pipeline<P> pipeline(sources[0], {sources[1], sources[2], sources[3]});
      while (!pipeline.done()) pipeline.step(256);
      return pipeline.finish();
    }
    auto search(blob const & head, lookup const & q) const {
      return head.search_window(q.key.view(), q.group, diet::profile_anchor<P>::complete(q.boundary.view()));
    }
    void verify(pair head) const {
      auto current = head;
      for (unsigned level = 3; level; --level) {
        require(std::addressof(current->native()) == std::addressof(sources[level]->native()), "native allocation changed");
        require(current->borrowed().size() == borrowed_ids[level].size(), "borrowed count differs from integer oracle");
        std::size_t at = 0;
        current->borrowed().view().visit_all([&](auto const & item) {
          auto expected = key_for<P>(borrowed_ids[level][at], unsigned((records[level][0].key.bit_size / 8) - 8));
          require(item.key.full_units == expected.bit_size / P::bits_per_unit, "borrowed key length");
          // A bitwise oracle does not call the optimized comparison/copy helpers.
          require(item.key.prefix.size() == expected.bit_size, "borrowed key prefix length");
          for (std::uint64_t bit = 0; bit != expected.bit_size; ++bit) {
            auto source = item.key.prefix.storage();
            auto offset = item.key.prefix.offset() + bit;
            auto actual = (std::to_integer<unsigned>(source[offset / 8]) >> (7 - offset % 8)) & 1;
            auto wanted = (std::to_integer<unsigned>(expected.bytes[bit / 8]) >> (7 - bit % 8)) & 1;
            require(actual == wanted, "borrowed key bits");
          }
          ++at;
          return true;
        });
        require(at == borrowed_ids[level].size(), "borrowed stream traversal");
        current = current->target();
      }
      require(current == sources[0], "target pin changed");
      for (auto const & q : queries) {
        auto result = search(*head, q);
        require(bool(result.native) == bool(q.native), "window native presence");
        if (q.native) {
          require(result.native->ordinal == *q.native, "window native ordinal");
          require(result.native->value == q.value, "window native value");
        }
        require(bool(result.borrowed_predecessor) == bool(q.borrowed), "window borrowed presence");
        if (q.borrowed) {
          require(result.borrowed_predecessor->ordinal == *q.borrowed, "window borrowed ordinal");
          require(result.borrowed_predecessor->target_ordinal == *q.borrowed * P::group_size, "window target ordinal");
        }
      }
    }
  };
  template <class A> void hash_array(digest & out, A const & value) {
    out.data(value.bytes());
    auto const & metadata = value.metadata();
    out.integer(metadata.record_count);
    out.integer(metadata.extent);
    out.integer(metadata.common_value_width.value_or(~std::uint64_t{0}));
    auto const & offsets = value.group_offsets();
    out.words(offsets.low); out.words(offsets.high); out.words(offsets.sparse);
    out.integer(metadata.record_count); out.integer(offsets.universe); out.integer(offsets.low_width);
    for (auto const & sample : offsets.samples) { out.integer(sample.first); out.integer(sample.sparse); }
  }
  template <class B> void hash_blob(digest & result, B const & value) {
    hash_array(result, value.native()); hash_array(result, value.borrowed());
    result.words(value.interleave().classes);
    result.words(value.interleave().checkpoints);
    result.integer(value.borrowed().size());
    result.integer(value.virtual_size());
    result.data(value.false_borrow_bits());
  }
  template <class B> digest hash_chain(std::shared_ptr<B const> head) {
    digest result;
    for (auto current = head; current; current = current->target()) hash_blob(result, *current);
    return result;
  }
  double elapsed(clock_type::time_point first) {
    return std::chrono::duration<double, std::nano>(clock_type::now() - first).count();
  }
  template <class P> void run(char const * label, unsigned count, unsigned prefix, unsigned query_count, unsigned rounds) {
    using blob = typename fixture<P>::blob;
    fixture<P> input(count, prefix, query_count);
    auto head = input.build_chain();
    input.verify(head);
    auto signature = hash_chain(head);
    digest base_signature;
    hash_blob(base_signature, *input.sources[0]);
    std::array<double, 3> times{};
    std::uint64_t checksum = 0;
    for (unsigned round = 0; round != rounds; ++round) {
      for (unsigned operation = 0; operation != 3; ++operation) {
        auto selected = (operation + round) % 3;
        if (selected == 0) {
          auto begin = clock_type::now();
          auto result = blob::build(input.records[0]);
          times[0] += elapsed(begin);
          digest check;
          hash_blob(check, result);
          require(!result.target() && check.value == base_signature.value && check.bytes == base_signature.bytes,
                  "base encoding changed between rounds");
          checksum += check.value;
        } else if (selected == 1) {
          auto begin = clock_type::now();
          auto result = input.build_chain();
          times[1] += elapsed(begin);
          auto check = hash_chain(result);
          require(check.value == signature.value && check.bytes == signature.bytes, "pipeline encoding changed between rounds");
          checksum += check.value;
        } else {
          auto begin = clock_type::now();
          std::uint64_t found = 0;
          for (auto const & q : input.queries) {
            auto result = input.search(*head, q);
            if (result.native) found += result.native->ordinal + result.native->value.bit_size + 1;
            if (result.borrowed_predecessor) found += result.borrowed_predecessor->ordinal + 1;
          }
          times[2] += elapsed(begin);
          checksum += found;
        }
      }
    }
    constexpr char const * operations[] = {"build_base", "build_pipeline", "search_window"};
    for (unsigned i = 0; i != 3; ++i) {
      auto operations_count = i == 2 ? query_count : 1u;
      std::cout << label << ',' << P::group_size << ',' << count << ',' << prefix << ',' << query_count << ','
        << rounds << ',' << operations[i] << ',' << std::fixed << std::setprecision(3)
        << times[i] / rounds / operations_count << ',' << signature.bytes << ',' << signature.value << ',' << checksum << '\n';
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
  auto rounds = argc > 4 ? unsigned(std::stoul(argv[4])) : 3u;
  require(count >= 64 && count <= 1048576 && prefix <= 4096 && queries && rounds, "invalid benchmark dimensions");
  std::cout << "profile,group_size,base_records,prefix_bytes,queries,rounds,operation,ns_per_operation,digest_bytes,encoding_digest,checksum\n";
  run<diet::storage_policy<diet::profile_unit::byte>>("byte_variable", count, prefix, queries, rounds);
  run<diet::storage_policy<diet::profile_unit::bit>>("bit_variable", count, prefix, queries, rounds);
  run<diet::storage_policy<diet::profile_unit::byte, diet::fixed_values<8>, 7>>("byte_fixed", count, prefix, queries, rounds);
  run<diet::storage_policy<diet::profile_unit::bit, diet::fixed_values<13>, 31>>("bit_fixed", count, prefix, queries, rounds);
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}
