/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures offset selection against independently checked integer sequences.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include "export.h"
#include <chrono>
#include <iomanip>
#include <numeric>
#include <random>
#include "candidates.h"

namespace {
  using namespace select_compare;
  using clock_type = std::chrono::steady_clock;
  volatile u64 sink = 0;
  double ns(clock_type::time_point start) { return std::chrono::duration<double, std::nano>(clock_type::now() - start).count(); }
  template <class R> void validate(R &rep, std::span<u64 const> values) {
    for (u64 i = 0; i < values.size(); ++i) require(rep.select(i) == values[i], "select result differs");
  }
  inline void escape(void const *pointer) { asm volatile("" : : "g"(pointer) : "memory"); }
  template <class F> void candidates(F &&fn, unsigned rotation = 0) {
    for (unsigned i = 0; i < 11; ++i) switch ((i + rotation) % 11) {
      case 0: fn.template operator()<current>("ef-current"); break;
      case 1: fn.template operator()<trusted_current>("ef-trusted-control"); break;
      case 2: fn.template operator()<direct<u64>>("direct64"); break;
      case 3: fn.template operator()<direct<std::uint32_t>>("direct32"); break;
      case 4: fn.template operator()<packed>("packed-absolute"); break;
      case 5: fn.template operator()<alternative<high_direct>>("ef-high-direct64"); break;
      case 6: fn.template operator()<alternative<sub32>>("ef-sub32"); break;
      case 7: fn.template operator()<alternative<simple<1>>>("ef-sux-simple1"); break;
      case 8: fn.template operator()<alternative<simple<2>>>("ef-sux-simple2"); break;
      case 9: fn.template operator()<alternative<simple<-1>>>("ef-sux-half"); break;
      case 10: fn.template operator()<alternative<simple<-2>>>("ef-sux-half-fixed"); break;
    }
  }
  template <class R> void bench(std::string const &sequence, char const *name, std::span<u64 const> values,
      unsigned trials, u64 queries) {
    if constexpr (std::same_as<R, direct<std::uint32_t>>) if (values.back() > UINT32_MAX) {
      std::cout << sequence << "," << name << ",invalid,0," << values.size() << "," << values.back()
        << ",0,0,0,0,0,0,0,0,0,0,0,ineligible-width\n";
      return;
    }
    auto rep = std::make_unique<R>(values); validate(*rep, values);
    auto memory = rep->sizes(); u64 n = values.size(), universe = values.back();
    auto quotient = universe / n; auto width = quotient ? unsigned(std::bit_width(quotient) - 1) : 0;
    auto high_bits = (universe >> width) + n;
    std::mt19937_64 generator(0x4f464653455453ULL);
    std::vector<u64> random(queries), clustered(queries), salts(queries), probes(queries);
    for (u64 i = 0, center = 0; i < queries; ++i) {
      random[i] = generator() % n;
      if (!(i & 63)) center = generator() % n;
      clustered[i] = (center + (generator() & 31)) % n;
      salts[i] = generator(); probes[i] = universe == UINT64_MAX ? generator() : generator() % (universe + 1);
    }
    auto independent = [&](auto ordinal) {
      std::array<u64, 8> sums{};
      for (u64 i = 0; i < queries; i += 8)
        for (unsigned lane = 0; lane < 8; ++lane) sums[lane] += rep->select(ordinal(i + lane));
      return std::accumulate(sums.begin(), sums.end(), u64{});
    };
    auto output = [&](int trial, char const *access, double duration, u64 calls, double build, double index_build, u64 checksum) {
      sink = checksum;
      std::cout << sequence << ',' << name << ',' << access << ',' << trial << ',' << n << ',' << universe << ',' << width << ',' << high_bits << ','
        << memory.payload << ',' << memory.auxiliary << ',' << memory.allocated << ',' << memory.object << ',' << calls << ','
        << duration << ',' << build << ',' << index_build << ',' << checksum << ",ok\n";
    };
    for (unsigned iteration = 0; iteration <= trials; ++iteration) {
      int trial = int(iteration) - 1; // Retain trial -1 as an excluded warmup.
      auto repeats = std::clamp<u64>((u64{1} << 16) / n, 1, 256);
      std::vector<std::unique_ptr<R>> constructed; constructed.reserve(repeats);
      escape(values.data());
      auto start = clock_type::now();
      for (u64 i = 0; i < repeats; ++i) constructed.push_back(std::make_unique<R>(values));
      escape(constructed.data());
      auto build = ns(start) / repeats;
      // Keep every allocation observable until after the stop clock.
      for (auto const &item : constructed) validate(*item, values);
      constructed.clear();
      double index_build = -1;
      if constexpr (requires { typename R::index_type; }) {
        std::vector<std::unique_ptr<typename R::index_type>> indexes; indexes.reserve(repeats);
        escape(&rep->data); start = clock_type::now();
        for (u64 i = 0; i < repeats; ++i) indexes.push_back(std::make_unique<typename R::index_type>(rep->data));
        escape(indexes.data()); index_build = ns(start) / repeats;
        for (auto const &index : indexes) for (u64 i = 0; i < n; ++i)
          require(rep->data.decode(i, index->select(i)) == values[i], "constructed index result");
      }
      for (unsigned mode = 0; mode < 3; ++mode) {
        auto ordinal = [&](u64 i) { return mode == 0 ? random[i] : mode == 1 ? clustered[i] : i % n; };
        auto expected = u64{}; for (u64 i = 0; i < queries; ++i) expected += values[ordinal(i)];
        escape(rep.get()); start = clock_type::now(); auto sum = independent(ordinal); escape(&sum); auto elapsed = ns(start);
        require(sum == expected, "independent checksum");
        output(trial, mode == 0 ? "random-throughput" : mode == 1 ? "clustered-throughput" : "sequential-throughput",
          elapsed, queries, build, index_build, sum);
      }
      u64 ordinal = 0, sum = 0;
      escape(rep.get()); start = clock_type::now();
      for (u64 i = 0; i < queries; ++i) { auto value = rep->select(ordinal); sum += value; ordinal = (value ^ salts[i]) % n; }
      escape(&sum); auto elapsed = ns(start);
      u64 expected = 0, at = 0;
      for (u64 i = 0; i < queries; ++i) { auto value = values[at]; expected += value; at = (value ^ salts[i]) % n; }
      require(expected == sum && at == ordinal, "dependent checksum");
      output(trial, "dependent-latency", elapsed, queries, build, index_build, sum);
      u64 calls = 0; sum = 0;
      auto searches = std::max<u64>(8, queries >> 4);
      escape(rep.get()); start = clock_type::now();
      for (u64 i = 0; i < searches; ++i) {
        u64 lo = 0, hi = n;
        while (lo < hi) {
          auto mid = lo + ((hi - lo) >> 1); auto value = rep->select(mid); ++calls;
          if (value < probes[i]) lo = mid + 1; else hi = mid;
        }
        sum += lo;
      }
      escape(&sum); elapsed = ns(start); expected = 0;
      for (u64 i = 0; i < searches; ++i) expected += std::lower_bound(values.begin(), values.end(), probes[i]) - values.begin();
      require(sum == expected, "binary-search checksum");
      output(trial, "lower-bound", elapsed, calls, build, index_build, sum);
      if constexpr (std::same_as<R, current>) {
        auto scans = std::max<u64>(1, queries / n); sum = 0;
        escape(rep.get()); start = clock_type::now();
        for (u64 pass = 0; pass < scans; ++pass) { auto cursor = rep->view.cursor(); while (!cursor.done()) sum += cursor.next(); }
        escape(&sum); elapsed = ns(start);
        require(sum == std::accumulate(values.begin(), values.end(), u64{}) * scans, "forward checksum");
        output(trial, "forward-cursor", elapsed, n * scans, build, index_build, sum);
        ordinal = 0;
        start = clock_type::now();
        for (u64 i = 0; i < queries; ++i) ordinal = (ordinal ^ salts[i]) % n;
        escape(&ordinal);
        output(trial, "dependency-loop-control", ns(start), queries, build, index_build, ordinal);
      }
    }
  }
  void check() {
    std::vector<std::vector<u64>> cases{{}, {0}, {0,0,0}, {0,1,2,3}, {0,1,UINT64_MAX - 4096}};
    for (u64 size : {7,63,64,255,256,257,1023,1024,1025,4097}) {
      std::vector<u64> values(size); u64 at = 0;
      for (u64 i = 0; i < size; ++i) { at += (i * 17) % 1000; values[i] = at; }
      cases.push_back(values);
      for (u64 i = 0; i < size; ++i) values[i] = i < size / 2 ? i / 3 : u64{1} << 40;
      cases.push_back(values);
    }
    std::vector<u64> sparse(4097);
    for (u64 i = 2047; i < sparse.size(); ++i) sparse[i] = u64{1} << 40;
    require(!everett::elias_fano::build(sparse).sparse.empty(), "fixture missed sparse directory");
    cases.push_back(sparse);
    for (auto const &values : cases) candidates([&]<class R>(char const *name) {
      if constexpr (std::same_as<R, direct<std::uint32_t>>) if (!values.empty() && values.back() > UINT32_MAX) return;
      try { R rep(values); validate(rep, values); }
      catch (...) { std::cerr << "Candidate " << name << " failed for " << values.size() << " offsets\n"; throw; }
    });
    for (u64 span : {65535, 65536, 65537}) {
      std::vector<u64> values(40000, span - 1024);
      std::fill_n(values.begin(), 1024, 0);
      alternative<simple<-2>> fixed(values); validate(fixed, values);
      alternative<simple<1>> full(values); validate(full, values);
      alternative<simple<-1>> original(values);
      bool rejected = false;
      try { validate(original, values); } catch (std::runtime_error const &) { rejected = true; }
      require(rejected == (span == 65536), "unexpected upstream Half boundary behavior");
      if (rejected) std::cout << "Observed expected original Sux Half verification failure at span 65536; fixed candidate passed\n";
    }
    std::cout << "All select candidates match independent offsets across " << cases.size() << " fixtures\n";
  }
}
int main(int argc, char **argv) try {
  std::cout << std::setprecision(17);
  require(argc >= 2, "usage: select_compare check | export DIR CASE | replay IN OUT COUNT | bench SEQUENCE QUERIES TRIALS");
  std::string operation = argv[1];
  if (operation == "check") check();
  else if (operation == "export") { require(argc == 4, "export arguments"); select_fixture::export_case(argv[2], unsigned(std::stoul(argv[3]))); }
  else if (operation == "replay") {
    require(argc == 5, "replay arguments"); auto original = select_fixture::load(argv[2]);
    auto scaled = select_fixture::replay(original, std::stoull(argv[4])); select_fixture::save(argv[3], scaled);
  } else if (operation == "bench") {
    require(argc == 5 || argc == 6, "bench arguments"); auto values = select_fixture::load(argv[2]); require(!values.empty(), "empty benchmark sequence");
    auto queries = std::stoull(argv[3]); auto trials = unsigned(std::stoul(argv[4])); require(queries >= 16 && !(queries & 7) && trials, "benchmark extent");
    std::cout << "sequence,candidate,access,trial,count,universe,low_width,high_bits,payload_bytes,auxiliary_bytes,allocated_bytes,object_bytes,select_calls,time_ns,whole_build_ns,index_build_ns,checksum,status\n";
    candidates([&]<class R>(char const *name) {
      try { bench<R>(std::filesystem::path(argv[2]).stem().string(), name, values, trials, queries); }
      catch (std::runtime_error const &error) {
        if constexpr (std::same_as<R, alternative<simple<-1>>>) {
          std::cout << std::filesystem::path(argv[2]).stem().string() << ',' << name << ",invalid,0,"
            << values.size() << ',' << values.back() << ",0,0,0,0,0,0,0,0,0,0,0,failed-verification\n";
          std::cerr << name << ": " << error.what() << '\n';
        } else throw;
      }
    }, argc == 6 ? unsigned(std::stoul(argv[5])) : 0);
  } else throw std::invalid_argument("unknown operation");
} catch (std::exception const &error) { std::cerr << error.what() << '\n'; return 1; }
