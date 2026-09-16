/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Compares pinned Elias-Fano query and construction paths.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
// Use select_compare.py to supply the pinned baseline and optional prototype.
#include <everett/select_groups.h>
#include <everett/select15.h>
#define everett select_baseline
#include SELECT_BASELINE_GROUPS
#include SELECT_BASELINE_FIXED
#undef everett
#ifdef SELECT_PROTOTYPE
#define everett select_prototype
#include SELECT_PROTOTYPE
#undef everett
#endif
#include <chrono>
#include <iostream>
#include <iomanip>
#include <string>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace {
  std::uint64_t observed = 0;
  std::uint64_t random_word(std::uint64_t & state) {
    state += 0x9e3779b97f4a7c15ull;
    auto v = state;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ull;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebull;
    return v ^ (v >> 31);
  }
  std::uint64_t reduce(std::uint64_t v, std::uint64_t n) {
    return std::uint64_t((static_cast<unsigned __int128>(v) * n) >> 64);
  }
  using query_function = std::uint64_t (*)(void const *, std::uint64_t);
  template<class V> [[gnu::noinline]] std::uint64_t query(void const * p, std::uint64_t i) {
    return static_cast<V const *>(p)->residual(i);
  }
  struct variant { char const * name; void const * view; query_function run; };
  template<class V> variant make(char const * name, V const & v) { return {name, &v, query<V>}; }
  void row(char const * operation, std::string const & dataset, char const * variant_name,
           std::size_t entries, unsigned width, std::size_t bytes, unsigned alignment,
           std::uint64_t operations, std::vector<double> times, std::uint64_t sum) {
    std::sort(times.begin(), times.end());
    std::cout << operation << ',' << dataset << ',' << variant_name << ',' << entries << ',' << width << ','
      << bytes << ',' << alignment << ',' << operations << ',' << times.size() << ',' << std::fixed << std::setprecision(3)
      << times[times.size()/2] << ',' << times.front() << ',' << times.back() << ',' << sum << '\n';
  }
  template<class I> I built;
  template<class I> [[gnu::noinline]] std::uint64_t build(std::span<std::uint64_t const> source) {
    // Retain the complete owner beyond the timed call so payload writes remain
    // observable. Replacing the previous result includes its destruction.
    built<I> = I::build(source, (source.size() - 1) * 15);
    return built<I>.universe;
  }
  template<class I> std::uint64_t build_checksum() {
    auto const & index = built<I>;
    std::uint64_t sum = index.universe;
    for (auto words : {std::span<std::uint64_t const>(index.low), std::span<std::uint64_t const>(index.high), std::span<std::uint64_t const>(index.sparse)})
      for (auto word : words) sum = (sum ^ word) * 0x9e3779b97f4a7c15ull;
    for (auto sample : index.samples) sum = (sum ^ sample.first ^ sample.sparse) * 0x9e3779b97f4a7c15ull;
    return sum;
  }
  using build_function = std::uint64_t (*)(std::span<std::uint64_t const>);
  using pack_function = void (*)(std::span<std::uint64_t const>, std::span<std::uint64_t>, unsigned);
  [[gnu::noinline]] bool monotone_scalar(std::span<std::uint64_t const> source) {
    for (std::size_t i=1;i<source.size();++i) if(source[i]<source[i-1]) return false;
    return true;
  }
  [[gnu::noinline]] bool monotone_current(std::span<std::uint64_t const> source) {
    return everett::select_groups_detail::monotone(source);
  }
  void exercise(std::size_t entries, unsigned trials, std::uint64_t count, unsigned width, std::string pattern) {
    std::uint64_t seed = 0x123456789abcdefull;
    std::vector<std::uint64_t> source(entries);
    auto step = std::uint64_t{1} << width;
    for (std::size_t i = 0; i + 1 < entries; ++i)
      source[i] = pattern == "zero" ? 0 : pattern == "sparse" ? (i < 17 ? 0 : entries * step) :
        (i / 2) * (step * 2) + (i % 2 ? random_word(seed) % (step * 2) : 0);
    source.back() = pattern == "zero" ? 0 : entries * step;
    auto index = everett::select_groups<15>::build(source, (entries - 1) * 15);
    auto old = select_baseline::select_groups<15>::build(source, (entries - 1) * 15);
    auto fixed = everett::select15_index::build(source, (entries - 1) * 15);
    auto old_fixed = select_baseline::select15_index::build(source, (entries - 1) * 15);
    if (old.low != index.low || old.high != index.high || old.sparse != index.sparse ||
        fixed.low != index.low || fixed.high != index.high || old_fixed.low != index.low || old_fixed.high != index.high)
      throw std::runtime_error("encoded array mismatch");
    auto bytes = index.low.size()*8 + index.high.size()*8 + index.samples.size()*16 + index.sparse.size()*8;
    // Retain exactly aligned and +8-byte-shifted copies of identical high words.
    std::vector<std::uint64_t> high_storage(index.high.size() + 16);
    auto start = high_storage.data();
    while (reinterpret_cast<std::uintptr_t>(start) % 64) ++start;
    for (unsigned misalignment : {0, 8}) {
      auto pointer = start + misalignment/8;
      std::copy(index.high.begin(), index.high.end(), pointer);
      std::span<std::uint64_t const> high(pointer, index.high.size());
      everett::select_groups_view<15> current(index.low, high, index.samples, index.sparse, (entries-1)*15, index.universe, index.low_width);
      select_baseline::select_groups_view<15> baseline(index.low, high, old.samples, index.sparse, (entries-1)*15, index.universe, index.low_width);
      everett::select15_view current_fixed(index.low, high, fixed.samples, index.sparse, (entries-1)*15, index.universe, index.low_width);
      select_baseline::select15_view baseline_fixed(index.low, high, old_fixed.samples, index.sparse, (entries-1)*15, index.universe, index.low_width);
      std::vector variants{make("old_groups", baseline), make("current_groups", current),
        make("old_select15", baseline_fixed), make("current_select15", current_fixed)};
#ifdef SELECT_PROTOTYPE
      auto proto = select_prototype::select_groups<15>::build(source, (entries-1)*15);
      if (proto.low != index.low || proto.high != index.high || proto.sparse != index.sparse) throw std::runtime_error("prototype encoding mismatch");
      select_prototype::select_groups_view<15> prototype(index.low, high, proto.samples, index.sparse, (entries-1)*15, index.universe, index.low_width);
      variants.push_back(make("simd_prototype", prototype));
#endif
      auto query_count = std::bit_floor(std::min<std::uint64_t>(count, 65536));
      std::vector<std::uint64_t> queries(query_count);
      std::uint64_t query_seed = 0x5e1ec7;
      for (auto & q : queries) q = reduce(random_word(query_seed), entries);
      for (auto const & v : variants) for (auto q : queries)
        if (v.run(v.view,q) != source[q]) throw std::runtime_error("independent select oracle");
      std::vector<std::vector<double>> times(variants.size());
      std::vector<std::uint64_t> sums(variants.size());
      auto measure = [&](std::size_t j, std::uint64_t n) {
        auto const & v = variants[j]; std::uint64_t sum = 0;
        auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t i=0; i<n; ++i) sum += v.run(v.view, queries[i & (query_count-1)]);
        auto end = std::chrono::steady_clock::now(); observed ^= sum; sums[j] = sum;
        return std::chrono::duration<double,std::nano>(end-begin).count()/double(n);
      };
      for (std::size_t j=0;j<variants.size();++j) (void)measure(j,std::min<std::uint64_t>(count,32768));
      for (unsigned t=0;t<trials;++t) {
        for (std::size_t j=0;j<variants.size();++j) { auto v=(j+t)%variants.size();times[v].push_back(measure(v,count)); }
        for (auto sum:sums) if (sum!=sums.front()) throw std::runtime_error("timed checksum mismatch");
      }
      auto name = pattern + (entries < 65536 ? "_hot" : "_larger");
      for(std::size_t j=0;j<variants.size();++j) row("query", name, variants[j].name,entries,index.low_width,bytes,misalignment,count,times[j],sums[j]);
    }
    std::vector<std::pair<char const *,build_function>> builders{{"old_groups",build<select_baseline::select_groups<15>>},
      {"current_groups",build<everett::select_groups<15>>}, {"old_select15",build<select_baseline::select15_index>},
      {"current_select15",build<everett::select15_index>}};
#ifdef SELECT_PROTOTYPE
    builders.push_back({"simd_prototype",build<select_prototype::select_groups<15>>});
#endif
    std::vector<std::uint64_t (*)()> checks{build_checksum<select_baseline::select_groups<15>>,
      build_checksum<everett::select_groups<15>>,build_checksum<select_baseline::select15_index>,build_checksum<everett::select15_index>};
#ifdef SELECT_PROTOTYPE
    checks.push_back(build_checksum<select_prototype::select_groups<15>>);
#endif
    std::vector<std::vector<double>> times(builders.size());std::vector<std::uint64_t>sums(builders.size());
    auto repetitions = std::max<std::size_t>(1, 1048576/entries);
    for(unsigned t=0;t<trials;++t) for(std::size_t j=0;j<builders.size();++j) {
      auto v=(j+t)%builders.size();auto begin=std::chrono::steady_clock::now();std::uint64_t sum=0;
      for(std::size_t n=0;n<repetitions;++n)sum+=builders[v].second(source);
      auto end=std::chrono::steady_clock::now(); observed^=sum; sums[v]=checks[v]();
      times[v].push_back(std::chrono::duration<double,std::nano>(end-begin).count()/double(repetitions*entries));
    }
    for(auto sum:sums)if(sum!=sums.front())throw std::runtime_error("constructed checksum mismatch");
    for(std::size_t j=0;j<builders.size();++j) row("build_per_entry",pattern,builders[j].first,entries,index.low_width,bytes,0,repetitions,times[j],sums[j]);
    for (auto validator : {std::pair{"old_scalar",monotone_scalar}, std::pair{"current",monotone_current}}) {
      std::vector<double> samples;
      bool (*volatile run)(std::span<std::uint64_t const>) = validator.second;
      for(unsigned t=0;t<trials;++t) {
        auto begin=std::chrono::steady_clock::now();std::uint64_t sum=0;
        for(std::size_t n=0;n<repetitions;++n)sum+=run(source);
        auto end=std::chrono::steady_clock::now();observed^=sum;
        if(sum!=repetitions)throw std::runtime_error("validation oracle mismatch");
        samples.push_back(std::chrono::duration<double,std::nano>(end-begin).count()/double(repetitions*entries));
      }
      row("validate_per_entry",pattern,validator.first,entries,index.low_width,entries*8,0,repetitions,samples,1);
    }
    if(pattern!="dense")return;
    for(unsigned w:{1,7,8,16,32,63}) {
      std::vector<std::uint64_t> packed((entries*w+63)/64);
      std::vector<std::pair<char const *,pack_function>> packers{{"old_groups",select_baseline::select_groups_detail::pack_low},
        {"current_groups",everett::select_groups_detail::pack_low}};
#ifdef SELECT_PROTOTYPE
      packers.push_back({"simd_prototype",select_prototype::select_groups_detail::pack_low});
#endif
      times.assign(packers.size(),{});
      for(unsigned t=0;t<trials;++t)for(std::size_t j=0;j<packers.size();++j) {
        auto v=(j+t)%packers.size();auto begin=std::chrono::steady_clock::now();
        for(std::size_t n=0;n<repetitions;++n)packers[v].second(source,packed,w);
        auto end=std::chrono::steady_clock::now();observed^=packed.back();
        times[v].push_back(std::chrono::duration<double,std::nano>(end-begin).count()/double(repetitions*entries));
      }
      for(std::size_t j=0;j<packers.size();++j)row("pack_per_entry",pattern,packers[j].first,entries,w,packed.size()*8,0,repetitions,times[j],packed.back());
    }
  }
}
int main(int argc,char**argv)try {
#if defined(__APPLE__)
  if(pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0))throw std::runtime_error("QoS request failed");
#endif
  auto entries=argc>1?std::stoull(argv[1]):4096ull;
  auto trials=argc>2?unsigned(std::stoul(argv[2])):5u;
  auto queries=argc>3?std::stoull(argv[3]):1048576ull;
  auto pattern=argc>4?std::string(argv[4]):"dense";
  if(entries<2||!trials||!queries)throw std::invalid_argument("positive dimensions required");
  std::cout<<"operation,case,variant,entries,width,encoded_bytes,high_mod64,operations,trials,median_ns,min_ns,max_ns,checksum\n";
  exercise(entries,trials,queries,8,pattern);
  std::cerr<<"observed="<<observed<<'\n';
}catch(std::exception const&e){std::cerr<<e.what()<<'\n';return 1;}
