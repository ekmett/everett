/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks bounded byte prefix comparison for scalar and explicit SIMD profiles.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/key_detail.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

#ifndef EVERETT_TEST_ARCH
#define EVERETT_TEST_ARCH simd::scalar
#endif

namespace {
  void check(void const *a,void const *b,std::size_t n,std::size_t expected) {
    if(everett::key_detail::common_bytes<EVERETT_TEST_ARCH>(a,b,n)!=expected)
      throw std::runtime_error("bounded common byte prefix mismatch");
  }
}
int main() {
  std::array<unsigned char,512> a{},b{};
  for(std::size_t i=0;i<a.size();++i) a[i]=b[i]=static_cast<unsigned char>(i*39);
  for(std::size_t n=0;n<=256;++n)
    for(std::size_t offset=0;offset<64;++offset) {
      check(a.data()+offset,b.data()+offset,n,n);
      for(std::size_t i=0;i<n;++i) {
        b[offset+i]^=1;check(a.data()+offset,b.data()+offset,n,i);b[offset+i]^=1;
      }
    }
  auto page=std::size_t(sysconf(_SC_PAGESIZE));
  auto p=static_cast<unsigned char *>(mmap(nullptr,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0));
  if(p==MAP_FAILED) throw std::runtime_error("mmap");
  if(mprotect(p+page,page,PROT_READ|PROT_WRITE)) {munmap(p,page*3);throw std::runtime_error("mprotect");}
  try {
    check(nullptr,nullptr,0,0);check(p,p,0,0);
    for(std::size_t n=0;n<=256;++n) {
      auto last=p+page*2-n;
      if(n) std::memcpy(last,a.data(),n);
      check(last,a.data(),n,n);
      for(std::size_t i=0;i<n;++i) {
        last[i]^=1;check(last,a.data(),n,i);last[i]^=1;
      }
    }
  } catch(...) {munmap(p,page*3);throw;}
  munmap(p,page*3);
}
