/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Bitmap rank with packed nine-bit prefixes in 512-bit blocks.
 *
 * \license
 * SPDX-FileCopyrightText: 2015, 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma once
// The rank/select design follows my ekmett/vr shaders/poppy.glsl, using
// 512-bit blocks and packed nine-bit ranks in place of its 2K/512 hierarchy.
#ifdef __METAL_VERSION__
#include <metal_stdlib>
#define everett_bench_rank_device device
#else
#include <bit>
#include <cstdint>
#define everett_bench_rank_device
#endif
namespace everett_bench {
#ifdef __METAL_VERSION__
  using rank_wide = unsigned long;
  inline unsigned rank_popcount(unsigned x){return metal::popcount(x);}
  inline unsigned rank_first(unsigned x){return 31-metal::clz(x&(~x+1));}
#else
  using rank_wide = std::uint64_t;
  inline unsigned rank_popcount(unsigned x){return std::popcount(x);}
  inline unsigned rank_first(unsigned x){return std::countr_zero(x);}
#endif
  struct rank_block {unsigned base,low,high;};
  template <unsigned N> struct rank_index {
    enum : unsigned {bits=N,words=(N+31)/32,blocks=(N+511)/512};
    unsigned raw[words];
    rank_block directory[blocks];
    unsigned total;
  };
  template <unsigned N> struct rank_view {
    everett_bench_rank_device rank_index<N> const * data;
    unsigned count() const {return data->total;}
    bool contains(unsigned i) const {return i<N&&(data->raw[i/32]&(1u<<(i%32)));}
    // Exclusive rank: the compact index of a populated grid location.
    unsigned rank(unsigned i) const {
      if(i>=N)return count();
      auto block=data->directory[i/512];unsigned pair=(i/64)%8;
      rank_wide packed=rank_wide(block.low)|(rank_wide(block.high)<<32);
      unsigned prefix=block.base+(pair?unsigned((packed>>((pair-1)*9))&511):0);
      if(i&32)prefix+=rank_popcount(data->raw[(i/64)*2]);
      return prefix+rank_popcount(data->raw[i/32]&((1u<<(i%32))-1));
    }
    // Dedicated select: locate a block, then a word, then its kth set bit.
    // Invalid ordinals return N, including select(0) on an empty index.
    unsigned select(unsigned ordinal) const {
      if(ordinal>=count())return N;
      unsigned lo=0,hi=rank_index<N>::blocks;
      while(lo+1<hi){unsigned mid=(lo+hi)/2;if(data->directory[mid].base<=ordinal)lo=mid;else hi=mid;}
      unsigned k=ordinal-data->directory[lo].base;
      for(unsigned word=lo*16;word<(lo+1)*16&&word<rank_index<N>::words;++word){
        unsigned value=data->raw[word],n=rank_popcount(value);
        if(k<n){while(k>0){--k;value&=value-1;}return word*32+rank_first(value);}k-=n;
      }
      return N;
    }
  };
#ifndef __METAL_VERSION__
  template <unsigned N> void build_rank(rank_index<N> & data) {
    if constexpr(N%32)data.raw[data.words-1]&=(1u<<(N%32))-1;
    unsigned total=0;
    for(unsigned block=0;block<data.blocks;++block){unsigned local=0;rank_wide packed=0;
      for(unsigned pair=0;pair<8;++pair){if(pair)packed|=rank_wide(local)<<((pair-1)*9);
        for(unsigned j=0;j<2;++j){unsigned word=block*16+pair*2+j;if(word<data.words)local+=rank_popcount(data.raw[word]);}}
      data.directory[block]={total,unsigned(packed),unsigned(packed>>32)};total+=local;
    }
    data.total=total;
  }
#endif
}
#undef everett_bench_rank_device

