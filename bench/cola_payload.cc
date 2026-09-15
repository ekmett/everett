// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <diet/cola_sections.h>
#include <diet/cola_query.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>
namespace allocation_probe {
  struct totals { std::uint64_t requested = 0, peak = 0, live = 0, calls = 0; };
#if defined(DIET_BENCH_ALLOCATIONS)
  thread_local totals counts;
  thread_local std::uint64_t epoch = 0;
  thread_local bool active = false;
  struct header { void * raw; std::size_t size; std::uint64_t epoch; };
  void begin() { ++epoch; counts = {}; active = true; }
  totals end() { active = false; return counts; }
  void * allocate(std::size_t size, std::size_t alignment) {
    alignment = std::max(alignment, alignof(header));
    auto overhead = sizeof(header) + alignment - 1;
    auto extent = std::max(size, std::size_t{1});
    if (extent > std::numeric_limits<std::size_t>::max() - overhead) throw std::bad_alloc();
    auto raw = std::malloc(extent + overhead);
    if (!raw) throw std::bad_alloc();
    auto address = (reinterpret_cast<std::uintptr_t>(raw) + overhead) & ~(alignment - 1);
    ::new (reinterpret_cast<void *>(address - sizeof(header))) header{raw, size, active ? epoch : 0};
    if (active) {
      counts.requested += size; counts.live += size; ++counts.calls;
      counts.peak = std::max(counts.peak, counts.live);
    }
    return reinterpret_cast<void *>(address);
  }
  void release(void * pointer) noexcept {
    if (!pointer) return;
    auto stored = reinterpret_cast<header *>(reinterpret_cast<std::uintptr_t>(pointer) - sizeof(header));
    if (stored->epoch && stored->epoch == epoch) counts.live -= stored->size;
    std::free(stored->raw);
  }
#else
  void begin() {}
  totals end() { return {}; }
#endif
}
#if defined(DIET_BENCH_ALLOCATIONS)
void * operator new(std::size_t size) { return allocation_probe::allocate(size, alignof(std::max_align_t)); }
void * operator new[](std::size_t size) { return allocation_probe::allocate(size, alignof(std::max_align_t)); }
void * operator new(std::size_t size, std::align_val_t align) { return allocation_probe::allocate(size, std::size_t(align)); }
void * operator new[](std::size_t size, std::align_val_t align) { return allocation_probe::allocate(size, std::size_t(align)); }
void operator delete(void * p) noexcept { allocation_probe::release(p); }
void operator delete[](void * p) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::size_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::size_t) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete(void * p, std::size_t, std::align_val_t) noexcept { allocation_probe::release(p); }
void operator delete[](void * p, std::size_t, std::align_val_t) noexcept { allocation_probe::release(p); }
#endif

using namespace diet;
using clock_type = std::chrono::steady_clock;
void require(bool ok, char const * message) { if (!ok) throw std::runtime_error(message); }
volatile std::uint64_t observed = 0;
template <class P> bit_string key_for(unsigned prefix, unsigned n) {
  std::string text(prefix, 'x');
  for (unsigned shift : {24u,16u,8u,0u}) text.push_back(char(n >> shift));
  auto key = bit_string::from_bytes(text);
  if constexpr (P::unit == profile_unit::bit) { key.bytes.push_back(std::byte((n & 7) << 5)); key.bit_size += 3; }
  return key;
}
template <class P> void run(unsigned prefix, unsigned rounds, std::filesystem::path const & dump, bool default_only) {
  using node = cola_index<P>;
  auto records = [prefix](unsigned count, unsigned stride, unsigned offset) {
    std::vector<profile_record> result;
    for (unsigned i = 0; i < count; ++i)
      result.push_back({key_for<P>(prefix,i*stride+offset),bit_string::from_bytes("value")});
    return result;
  };
  auto leaf = std::make_shared<node const>(node::build(records(4096,5,0)));
  auto main = std::make_shared<node const>(node::build(records(4096,3,1),leaf,leaf->native_owner()));
  auto secondary = std::make_shared<profile_array<P> const>(profile_array<P>::build(records(4096,3,0)));
  auto native = std::make_shared<profile_array<P> const>(profile_array<P>::build(records(2048,6,2)));
  auto old_native = std::make_shared<profile_array<P> const>(profile_array<P>::build(records(2048,6,3)));
  auto different_main = std::make_shared<node const>(node::build(records(4096,3,2),leaf,leaf->native_owner()));
  auto different_secondary = std::make_shared<profile_array<P> const>(profile_array<P>::build(records(4096,3,1)));
  std::vector<typename node::pair_type> sources{{},
    std::make_shared<node const>(node::adopt_native(old_native,main,different_secondary)),
    std::make_shared<node const>(node::adopt_native(old_native,different_main,secondary)),
    std::make_shared<node const>(node::adopt_native(old_native,main,secondary))};
  std::vector<bit_string> queries;
  std::vector<unsigned> wanted;
  std::uint64_t random = 0x123456789abcdef;
  for (unsigned i=0;i<4096;++i) {
    random ^= random << 13; random ^= random >> 7; random ^= random << 17;
    unsigned n = unsigned(random % 24576);
    queries.push_back(key_for<P>(prefix,n));
    wanted.push_back(unsigned(n>=2 && (n-2)%6==0 && (n-2)/6<2048) +
      unsigned(n>=1 && (n-1)%3==0 && (n-1)/3<4096) +
      2*unsigned(n%5==0 && n/5<4096) + unsigned(n%3==0 && n/3<4096));
  }
  [[maybe_unused]] auto consume = [&](auto const & root, bool check) {
    std::uint64_t sum = 0;
    for (std::size_t i=0;i<queries.size();++i) {
      auto cursor = root.cursor(queries[i].view()); unsigned count=0;
      while (!cursor.done()) {
        cursor.step(1);
        while(cursor.has_match()) {
          auto match=cursor.take_match(); ++count;
          if(check) require(match.value==bit_string::from_bytes("value"),"query value oracle");
          for(auto byte:match.value.bytes) sum=sum*131+std::to_integer<unsigned>(byte);
          sum+=match.ordinal;
        }
      }
      if(check) require(count==wanted[i],"independent integer query multiplicity oracle");
      sum+=count;
    }
    return sum;
  };
  object_id native_id("00000000000000000000000000000001"),side_id("00000000000000000000000000000002");
  blob_identity main_id{object_id("00000000000000000000000000000003"),object_id("00000000000000000000000000000004")};
  std::vector<std::byte> expected;
#if defined(DIET_REUSE)
  unsigned modes=default_only?1:4;
#else
  (void)default_only;
  unsigned modes=1;
#endif
  for (unsigned round=0;round<=rounds;++round) for(unsigned turn=0;turn<modes;++turn) {
    unsigned mode=(turn+round)%modes;
    allocation_probe::begin();
    auto start=clock_type::now();
    auto result=[&] {
#if defined(DIET_REUSE)
      cola_index_builder<P> builder(native,main,secondary,sources[mode]);
#else
      cola_index_builder<P> builder(native,main,secondary);
#endif
      while(!builder.done()) builder.step(4096);
      return builder.finish();
    }();
    auto stop=clock_type::now();
    auto allocations=allocation_probe::end();
    auto encoded=encode_cola_sections(result,native_id,main_id,side_id).materialize();
    if(expected.empty()) expected=encoded;
    require(expected==encoded,"complete IX03 bytes differ between modes/rounds");
#if defined(DIET_REUSE)
    if(mode) for(unsigned route=0;route<2;++route) {
      bool reused=mode==3 || mode==route+1;
      require((&result.borrowed(route)==&sources[mode]->borrowed(route))==reused,"payload sharing identity");
    }
#endif
    auto name=std::string(P::unit==profile_unit::byte?"byte":"bit")+"-"+std::to_string(P::group_size)+"-"+std::to_string(prefix);
    if(!round && !mode) {
      std::ofstream out(dump/(name+".index"),std::ios::binary);out.exceptions(std::ios::badbit|std::ios::failbit);
      out.write(reinterpret_cast<char const *>(encoded.data()),std::streamsize(encoded.size()));
    }
    if(round) {
      auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start).count();
      std::cout<<name<<",build,"<<mode<<','<<round<<','<<result.virtual_size()<<','<<ns<<','
        <<allocations.calls<<','<<allocations.requested<<','<<allocations.peak<<','<<allocations.live<<','<<crc32c(encoded)<<'\n';
    }
#ifndef DIET_BENCH_ALLOCATIONS
    if(!mode) {
      auto shared=std::make_shared<node const>(std::move(result));
      auto root=cola_query_root<P>::build(shared);
      auto oracle=consume(root,true);
      start=clock_type::now(); auto sum=consume(root,false); stop=clock_type::now();
      require(sum==oracle,"timed query changed answers"); observed=sum;
      if(round) std::cout<<name<<",query,0,"<<round<<','<<queries.size()<<','
        <<std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start).count()<<",0,0,0,0,"<<sum<<'\n';
    }
#endif
  }
}
int main(int argc,char **argv) {
  unsigned rounds=argc>1?unsigned(std::stoul(argv[1])):3;
  std::filesystem::path dump=argc>2?argv[2]:"build-payload-wire";std::filesystem::create_directories(dump);
  bool default_only=argc>3 && std::string(argv[3])=="1";
  for(unsigned prefix:{0u,4096u}) {
    run<storage_policy<profile_unit::byte,variable_values,3,exponential_golomb<0>,16>>(prefix,rounds,dump,default_only);
    run<storage_policy<profile_unit::byte,variable_values,15,exponential_golomb<0>,16>>(prefix,rounds,dump,default_only);
    run<storage_policy<profile_unit::bit,variable_values,3,exponential_golomb<0>,16>>(prefix,rounds,dump,default_only);
    run<storage_policy<profile_unit::bit,variable_values,15,exponential_golomb<0>,16>>(prefix,rounds,dump,default_only);
  }
}
