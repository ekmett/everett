/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures mapped complete replacement searches with two EF selectors.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include <everett/sort_profile_file.h>
#include <everett/typed_world.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
  using namespace everett;
  using u64 = std::uint64_t;
  using clock_type = std::chrono::steady_clock;
  using strings = unsorted<std::optional<std::string>>;
  using bit_policy = storage_policy<bin<tip<strings>, sort_undefined>>;
  using byte_policy = storage_policy<>;
  void require(bool condition, char const * message) { if (!condition) throw std::runtime_error(message); }
  double elapsed(clock_type::time_point start) {
    return std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
  }
  u64 mix(u64 x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }
  std::string key(u64 ordinal, unsigned width, bool random) {
    std::string result(width, 'p');
    if (random) for (unsigned i = 0; i < width; i += 8) {
      auto word = mix(ordinal + u64(i) * 0xa0761d6478bd642full);
      for (unsigned j = 0; j < 8 && i + j < width; ++j) result[i + j] = char(word >> (j * 8));
    }
    // The suffix guarantees uniqueness even if the random prefix collides.
    for (unsigned j = 0; j < 8; ++j) result[width - 8 + j] = char(ordinal >> ((7 - j) * 8));
    return result;
  }
  std::string value(u64 ordinal) {
    std::string result(32, 'v');
    for (unsigned j = 0; j < 8; ++j) result[j] = char(ordinal >> (j * 8));
    return result;
  }
  u64 digest(std::optional<std::string> const & value) {
    if (!value) return 0x5be0cd19137e2179ull;
    u64 result = 0;
    for (unsigned j = 0; j < 8; ++j) result |= u64(static_cast<unsigned char>((*value)[j])) << (j * 8);
    return mix(result);
  }
  struct temporary {
    std::filesystem::path path;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-search-XXXXXX").string();
      auto p = ::mkdtemp(pattern.data()); if (!p) throw std::runtime_error("mkdtemp failed"); path = p;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  void write(std::filesystem::path const & path, std::span<std::byte const> data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<char const *>(data.data()), std::streamsize(data.size()));
    require(bool(out), "fixture write failed");
  }
  struct space {
    u64 files = 0, payload = 0, ef_payload = 0, ef_aux = 0, rank = 0, cuts = 0, flags = 0;
    u64 payload_hash = 14695981039346656037ull;
    void offsets(elias_fano_view ef) {
      ef_payload += (ef.low_words().size() + ef.high_words().size()) * 8;
      ef_aux += ef.samples().size() * 16 + ef.sparse_words().size() * 8;
    }
    template <class V> void stream(V const & view) {
      auto meta = view.metadata();
      payload += meta.offset_unit == profile_unit::bit ? (meta.extent + 7) / 8 : meta.extent;
      auto bytes = [&] {
        if constexpr (requires { view.bytes(); }) return view.bytes();
        else return view.data().storage();
      }();
      for (auto byte : bytes) payload_hash = (payload_hash ^ std::to_integer<unsigned char>(byte)) * 1099511628211ull;
      payload_hash = (payload_hash ^ meta.extent) * 1099511628211ull;
      offsets(view.group_offsets());
    }
  };
  template <bool Bit> struct fixture {
    using P = std::conditional_t<Bit, bit_policy, byte_policy>;
    using array = std::conditional_t<Bit, sort_profile_array<P>, profile_array<P>>;
    using native = std::conditional_t<Bit, mapped_sort_profile<P>, mapped_native<P>>;
    using blob = std::conditional_t<Bit, mapped_sort_cola<P>, mapped_cola_blob<P>>;
    using native_ptr = std::shared_ptr<native const>;
    using pair_ptr = typename blob::pair_type;
    temporary directory;
    unsigned serial = 0;
    space footprint;
    double native_ns = 0, index_ns = 0;
    pair_ptr head;
    std::vector<std::pair<std::string, u64>> records;
    struct query { std::string key; std::optional<std::string> expected; };
    std::vector<query> hits, misses, mixed;

    object_id identity() {
      char text[33]; std::snprintf(text, sizeof(text), "%032x", ++serial); return object_id(text);
    }
    std::pair<native_ptr, object_id> save(unsigned stride, bool empty = false) {
      auto start = clock_type::now();
      auto source = [&] {
        if constexpr (Bit) {
          sort_profile_writer<P> writer;
          if (!empty) for (auto const & [k, ordinal] : records)
            if (!(ordinal % stride)) writer.template append<strings>(k, value(ordinal));
          return writer.finish();
        } else {
          profile_native_writer<P> writer;
          if (!empty) for (auto const & [k, ordinal] : records)
            if (!(ordinal % stride)) writer.append(profile_record{typed_detail::key<P, strings>(k),
              typed_detail::value<P, strings>(std::optional(value(ordinal)))});
          return writer.finish();
        }
      }();
      footprint.stream(source.view());
      auto bytes = [&] {
        if constexpr (Bit) return encoded_sort_sections<P>::from(source).materialize();
        else return encode_native_sections(source).materialize();
      }();
      auto id = identity(); auto path = directory.path / (id.hex() + ".kv"); write(path, bytes);
      footprint.files += bytes.size();
      auto mapped = std::make_shared<native const>(native::open(path));
      native_ns += elapsed(start);
      return {std::move(mapped), id};
    }
    pair_ptr index(native_ptr source, object_id id, pair_ptr main = {}, native_ptr side = {},
                   std::optional<object_id> side_id = {}) {
      auto start = clock_type::now();
      auto built = cola_index<P, native, blob>::adopt_native(source, main, side);
      for (unsigned route = 0; route < 2; ++route) {
        footprint.stream(built.borrowed(route).view());
        auto rank = built.interleave(route).view();
        footprint.rank += (rank.class_words().size() + rank.checkpoint_words().size()) * 8;
        footprint.cuts += built.cut_lcps(route).size() * 8;
        footprint.flags += built.false_borrow_bits(route).size();
      }
      auto index_id = identity(); auto path = directory.path / (index_id.hex() + ".index");
      auto encoded = encode_cola_sections(built, id, main ? std::optional(main->identity()) : std::nullopt, side_id);
      auto bytes = encoded.materialize(); write(path, bytes); footprint.files += bytes.size();
      auto mapped = std::make_shared<mapped_cola_index<P> const>(mapped_cola_index<P>::open(path));
      auto result = blob::bind({id, index_id}, source, mapped, main, side, side_id);
      index_ns += elapsed(start); return result;
    }
    fixture(unsigned count, unsigned width, bool random, unsigned queries) {
      records.reserve(count);
      for (unsigned i = 0; i < count; ++i) records.emplace_back(key(2 * i, width, random), i);
      std::sort(records.begin(), records.end(), [](auto const & a, auto const & b) { return a.first < b.first; });
      for (unsigned stride : {1u, 4u, 16u, 64u}) {
        auto [source, id] = save(stride);
        if (stride == 16) {
          auto [side, side_id] = save(128);
          head = index(source, id, head, side, side_id);
        } else head = index(source, id, head);
      }
      auto [empty, empty_id] = save(1, true);
      while (head->virtual_size() > P::group_size) head = index(empty, empty_id, head);
      for (unsigned i = 0; i < queries; ++i) {
        auto ordinal = mix(i + 917) % count;
        hits.push_back({key(2 * ordinal, width, random), value(ordinal)});
        misses.push_back({key(2 * ordinal + 1, width, random), {}});
        mixed.push_back(i & 1 ? misses.back() : hits.back());
      }
    }
    [[gnu::noinline]] std::optional<std::string> get(auto const & root, std::string const & key) const {
      auto encoded = [&] {
        if constexpr (Bit) return sort_profile_query<P, strings>(key);
        else return typed_detail::key<P, strings>(key);
      }();
      auto result = cola_detail::first_value(root, std::move(encoded), [](bit_view v) {
        return typed_detail::value<P, strings>(v);
      });
      return result ? std::move(*result) : std::optional<std::string>{};
    }
  };

  template <bool Bit> void run(unsigned count, unsigned width, bool random, unsigned queries,
      unsigned trials, unsigned loops, unsigned profile_seconds) {
    fixture<Bit> data(count, width, random, queries);
    using P = typename fixture<Bit>::P; using B = typename fixture<Bit>::blob;
    auto root = cola_query_root<P, B>::adopt_prepared(data.head);
    for (auto const * group : {&data.hits, &data.misses, &data.mixed})
      for (auto const & query : *group) require(data.get(root, query.key) == query.expected, "query differs from independent oracle");
    auto execute = [&](auto const & set, bool dependent, unsigned repetitions, bool oracle) {
      u64 checksum = 0, previous = 0;
      for (unsigned r = 0; r < repetitions; ++r) for (unsigned j = 0; j < queries; ++j) {
        auto at = dependent ? (j + previous) % queries : j;
        auto const & q = set[at];
        auto result = oracle ? q.expected : data.get(root, q.key);
        previous = digest(result);
        checksum += previous;
      }
      return checksum;
    };
    if (profile_seconds) {
      // A launch profiler also sees setup. This marker and a long repeated
      // query phase allow selecting only the steady-state interval.
      std::cerr << "PROFILE_QUERY_BEGIN\n" << std::flush;
      auto until = clock_type::now() + std::chrono::seconds(profile_seconds);
      u64 checksum = 0, batches = 0;
      do { checksum += execute(data.mixed, true, 1, false); ++batches; } while (clock_type::now() < until);
      std::cerr << "PROFILE_QUERY_END " << checksum << ' ' << batches << '\n';
      return;
    }
    auto const & s = data.footprint;
    std::cerr << "SPACE," << s.files << ',' << s.payload << ',' << s.ef_payload << ',' << s.ef_aux << ','
      << s.rank << ',' << s.cuts << ',' << s.flags << ',' << data.native_ns << ',' << data.index_ns << ',' << s.payload_hash << '\n';
    for (unsigned trial = 0; trial < trials; ++trial) for (unsigned kind = 0; kind < 3; ++kind)
      for (unsigned order = 0; order < 2; ++order) {
        auto const & set = kind == 0 ? data.hits : kind == 1 ? data.misses : data.mixed;
        auto expected = execute(set, order, loops, true);
#ifdef EVERETT_SEARCH_AUDIT
        search_audit::calls = {}; search_audit::enabled = true;
#endif
        auto start = clock_type::now(); auto actual = execute(set, order, loops, false); auto ns = elapsed(start);
#ifdef EVERETT_SEARCH_AUDIT
        search_audit::enabled = false;
#endif
        require(actual == expected, "timed query checksum differs");
        std::cout << (Bit ? "typed-bit" : "typed-byte") << ',' << (random ? "hash" : "structured") << ','
          << count << ',' << width << ',' << (kind == 0 ? "hit" : kind == 1 ? "miss" : "mixed") << ','
          << (order ? "dependent" : "independent") << ',' << trial << ',' << queries * u64(loops) << ','
          << std::fixed << std::setprecision(3) << ns / (queries * u64(loops)) << ',' << actual;
#ifdef EVERETT_SEARCH_AUDIT
        for (auto calls : search_audit::calls) std::cout << ',' << calls;
#endif
        std::cout << '\n';
      }
  }
}

int main(int argc, char ** argv) {
  try {
    if (argc != 10) throw std::runtime_error("usage: bench byte|bit records width structured|hash queries trials loops profile_seconds label");
    unsigned count = std::stoul(argv[2]), width = std::stoul(argv[3]), queries = std::stoul(argv[5]);
    require(count && width >= 8 && queries, "invalid fixture dimensions");
    std::cout << "profile,distribution,records,key_bytes,query_kind,access,trial,queries,ns_per_query,checksum";
#ifdef EVERETT_SEARCH_AUDIT
    std::cout << ",ef_select_calls,rank_calls,project_calls,byte_frame_calls,bit_frame_calls,compare_calls,materialize_calls";
#endif
    std::cout << '\n';
    if (std::string(argv[1]) == "bit") run<true>(count, width, std::string(argv[4]) == "hash", queries,
      std::stoul(argv[6]), std::stoul(argv[7]), std::stoul(argv[8]));
    else run<false>(count, width, std::string(argv[4]) == "hash", queries,
      std::stoul(argv[6]), std::stoul(argv[7]), std::stoul(argv[8]));
  } catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
  return 0;
}
