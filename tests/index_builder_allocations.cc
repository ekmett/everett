/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests index input replacement rollback under allocation failure.
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/index_builder.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  // Only append itself is faulted. Fixture/oracle construction and assertions
  // run with allocation enabled, including while inspecting rejected appends.
  thread_local std::ptrdiff_t fail_after = -1;
  void * allocate(std::size_t size) {
    if (fail_after == 0) throw std::bad_alloc();
    if (fail_after > 0) --fail_after;
    if (auto result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
  }
}

void * operator new(std::size_t size) { return allocate(size); }
void * operator new[](std::size_t size) { return allocate(size); }
void operator delete(void * pointer) noexcept { std::free(pointer); }
void operator delete[](void * pointer) noexcept { std::free(pointer); }
void operator delete(void * pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void * pointer, std::size_t) noexcept { std::free(pointer); }


namespace {
  using namespace diet;
  void require(bool value, char const * message) {
    if (!value) throw std::runtime_error(message);
  }
  void equal(bit_view a, bit_view b) {
    require(a.size() == b.size(), "key size after rejected replacement");
    for (std::uint64_t i = 0; i < a.size(); ++i)
      require(a.at(i) == b.at(i), "key bits after rejected replacement");
  }
  template <class P> void run(bool coded) {
    auto first = bit_string::from_bytes(std::string(4096, 'a'));
    auto next = bit_string::from_bytes(std::string(8192, 'b'));
    if constexpr (P::unit == profile_unit::bit) {
      first.bytes.push_back(std::byte{0xa0}); first.bit_size += 3;
      next.bytes.push_back(std::byte{0x40}); next.bit_size += 2;
    }
    profile_sample_encoder<P> encoder;
    auto a = encoder.encode(first.view(), 0), b = encoder.encode(next.view(), P::group_size);
    auto empty = std::make_shared<profile_array<P> const>(profile_array<P>::build({}));
    index_builder<P> stage(empty);
    if (coded) stage.push(a); else stage.push(first.view(), 0);
    stage.step(1);
    require(stage.has_output() && stage.needs_input(), "first sample not queued");
    fail_after = 0;
    bool failed = false;
    try {
      if (coded) stage.push(b); else stage.push(next.view(), P::group_size);
    } catch (std::bad_alloc const &) { failed = true; }
    catch (...) { fail_after = -1; throw; }
    fail_after = -1;
    require(failed && !stage.failed() && stage.needs_input() && stage.received_samples() == 1 && stage.has_output(),
      "decoder allocation failure changed accepted state");
    auto sample = stage.take_output(); equal(sample.key.view(), first.view());
    if (coded) stage.push(b); else stage.push(next.view(), P::group_size);
    // A successful push owns its replacement before its caller's frame expires.
    a.suffix = {}; b.suffix = {};
    stage.close_input();
    while (!stage.done()) { stage.step(1); if (stage.has_output()) stage.take_coded_output(); }
    auto artifact = stage.finish_index(P::group_size + 1);
    auto cursor = artifact.borrowed().view().cursor();
    equal(cursor.peek().key.prefix, first.view()); cursor.advance();
    equal(cursor.peek().key.prefix, next.view()); cursor.advance();
    require(cursor.done(), "rejected decoder replacement changed occurrence count");
  }
}
int main() try {
  for (bool coded : {false, true}) {
    run<storage_policy<profile_unit::byte>>(coded);
    run<storage_policy<profile_unit::bit>>(coded);
  }
  std::cout << "Index decoder allocation rollback checks passed\n";
  return 0;
} catch (std::exception const & error) { fail_after = -1; std::cerr << error.what() << '\n'; return 1; }
