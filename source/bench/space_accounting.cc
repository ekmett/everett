/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures occupied arrays in a reproducible four-run chain.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include <diet/query.h>
#ifdef DIET_DUAL
#include <diet/cola_index.h>
#endif
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
namespace {
  template<class C> std::uint64_t bytes(C const & c) { return c.size()*sizeof(typename C::value_type); }
  template<class E> std::uint64_t ef(E const & e) { return bytes(e.low)+bytes(e.high)+bytes(e.samples)+bytes(e.sparse); }
  std::uint64_t mix(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }
  template<class P> void run(unsigned count, unsigned prefix, unsigned value_bytes, bool random) {
    using blob = diet::profile_blob<P>;
    using pair = std::shared_ptr<blob const>;
    #ifdef DIET_DUAL
    using native_array = diet::profile_array<P>;
    using index = diet::cola_index<P>;
    std::array<std::shared_ptr<native_array const>,8> sources;
    constexpr unsigned source_count=8;
#else
    std::array<pair,4> sources;
    constexpr unsigned source_count=4;
#endif
    std::uint64_t records=0, raw_keys=0, raw_values=0;
    for (unsigned level=0; level<source_count; ++level) {
      std::vector<diet::profile_record> data;
      for (unsigned j=0; j<count/(1u<<(2*(level/(source_count/4)))); ++j) {
        auto id=std::uint64_t(j)*source_count+level;
        if (random) id=mix(id);
        std::string key(prefix,'p');
        for (unsigned b=8;b;--b) key.push_back(char(id>>(8*(b-1))));
        std::string value(value_bytes,'v');
        data.push_back({diet::bit_string::from_bytes(key),diet::bit_string::from_bytes(value)});
        ++records; raw_keys+=key.size(); raw_values+=value.size();
      }
      std::sort(data.begin(),data.end(),[](auto const&a,auto const&b){return diet::compare_bits(a.key.view(),b.key.view())<0;});
      #ifdef DIET_DUAL
      sources[level]=std::make_shared<native_array const>(native_array::build(data));
#else
      sources[level]=std::make_shared<blob const>(blob::build(data));
#endif
    }
    std::uint64_t native=0,borrowed=0,offsets=0,rank=0,lcp=0,flags=0,bcount=0,catalogs=0;
#ifdef DIET_DUAL
    typename index::pair_type head;
    typename index::native_pointer secondary;
    for(unsigned level=0;level<4;++level) {
      head=std::make_shared<index const>(index::adopt_native(sources[2*level],head,secondary));
      secondary=sources[2*level+1];
    }
    head=index::prepare_root(head,secondary);
    for(auto p=head;p;p=p->main_target()) {
      native+=p->native().bytes().size(); offsets+=ef(p->native().group_offsets());
      if(auto s=p->secondary_target()) {native+=s->bytes().size(); offsets+=ef(s->group_offsets());}
      for(unsigned route=0;route<2;++route) {
        borrowed+=p->borrowed(route).bytes().size(); offsets+=ef(p->borrowed(route).group_offsets());
        rank+=bytes(p->interleave(route).classes)+bytes(p->interleave(route).checkpoints);
        lcp+=bytes(p->cut_lcps(route));flags+=p->false_borrow_bits(route).size();bcount+=p->borrowed(route).size();
      }
      ++catalogs;
    }
    std::cout<<"dual,";
#else
    diet::index_pipeline<P> pipeline(sources[0],{sources[1],sources[2],sources[3]});
    while(!pipeline.done()) pipeline.step(4096);
    auto root=diet::query_root<P>::build(pipeline.finish());
    for(auto p=root.head();p;p=p->target()) {
      native+=p->native().bytes().size(); borrowed+=p->borrowed().bytes().size();
      offsets+=ef(p->native().group_offsets())+ef(p->borrowed().group_offsets());
      rank+=bytes(p->interleave().classes)+bytes(p->interleave().checkpoints);
      lcp+=bytes(p->cut_lcps());flags+=p->false_borrow_bits().size();bcount+=p->borrowed().size();++catalogs;
    }
    std::cout<<"single,";
#endif
    std::cout<<(P::unit==diet::profile_unit::byte?"byte":"bit")<<','<<P::fixed_width<<','<<count<<','<<prefix<<','<<value_bytes<<','<<random<<','<<records<<','<<raw_keys<<','<<raw_values<<','<<catalogs<<','<<bcount<<','<<native<<','<<borrowed<<','<<offsets<<','<<rank<<','<<lcp<<','<<flags<<','<<(native+borrowed+offsets+rank+lcp+flags)<<'\n';
  }
}
int main() {
  std::cout<<"topology,unit,fixed,base_count,prefix_bytes,value_bytes,random,native_count,raw_key_bytes,raw_value_bytes,catalogs,borrowed_count,native_bytes,borrowed_bytes,ef_bytes,rank_bytes,lcp_bytes,false_borrow_bytes,total_array_bytes\n";
  for(auto n:{4096u,65536u}) for(auto prefix:{0u,64u}) for(bool random:{false,true}) {
    run<diet::storage_policy<diet::profile_unit::byte,diet::fixed_values<8>>>(n,prefix,8,random);
    run<diet::storage_policy<diet::profile_unit::bit,diet::fixed_values<64>>>(n,prefix,8,random);
    run<diet::storage_policy<diet::profile_unit::byte>>(n,prefix,128,random);
  }
}
