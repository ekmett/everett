/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures complete fixed-key merges against an independent CPU oracle.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../host_backend.h"
#include <everett/elias_fano.h>
#include "fixtures.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using key = std::array<u32, 4>;
static_assert(std::endian::native == std::endian::little);
using fixed_fixture::record;
using fixed_fixture::fixture;
using fixed_fixture::make_fixture;
using fixed_fixture::value_shape;
using clock_type = std::chrono::steady_clock;
double elapsed(clock_type::time_point start) {
  return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
}
void require(bool ok, char const *message) { if (!ok) throw std::runtime_error(message); }
u32 narrow(u64 value) {
  require(value < (u64{1} << 30), "experiment extent exceeds 30-bit bound");
  return u32(value);
}
u32 align8(u32 value) { return (value + 7) & ~7u; }

// This envelope is an experiment format, not an Everett object-file ABI.
struct layout {
  std::array<u32, 64> h{};
  layout(bool variable, u32 n, u32 bytes, u32 sparse = 0, u32 identity = 0) {
    auto &a = h;
    a[0] = variable ? 0x31564645u : 0x31464645u; // EFV1 / EFF1
    a[1] = 1; a[2] = variable; a[3] = n; a[4] = 64;
    a[5] = narrow(256 + u64(n) * 16); a[6] = bytes; a[18] = identity;
    a[16] = narrow(u64(a[5]) + bytes);
    if (!variable) return;
    auto count = n + 1, q = bytes / count;
    a[11] = q ? std::bit_width(q) - 1 : 0;
    a[12] = narrow((u64(count) * a[11] + 63) >> 6);
    a[13] = narrow(((u64(bytes) >> a[11]) + count + 63) >> 6);
    a[14] = (count + 255) >> 8; a[15] = sparse;
    a[7] = align8(a[16]) >> 2;
    a[8] = a[7] + a[12] * 2;
    a[9] = a[8] + a[13] * 2;
    a[10] = a[9] + a[14] * 4;
    a[16] = narrow(u64(a[10]) * 4 + u64(sparse) * 8);
  }
  u32 capacity() const { return narrow(u64(h[16]) + (h[2] ? u64(h[3] + 1) * 8 : 0)); }
};

struct mapping {
  int fd = -1;
  std::size_t length = 0;
  void *data = MAP_FAILED;
  mapping(std::filesystem::path const &path, std::size_t bytes) {
    auto page = std::size_t(sysconf(_SC_PAGESIZE));
    length = (std::max(bytes, std::size_t(4)) + page - 1) / page * page;
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    require(fd >= 0, "open mapped fixture");
    require(ftruncate(fd, off_t(length)) == 0, "size mapped fixture");
    data = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    require(data != MAP_FAILED, "map fixture");
  }
  mapping(mapping const &) = delete;
  ~mapping() {
    if (data != MAP_FAILED) munmap(data, length);
    if (fd >= 0) close(fd);
  }
  u32 *words() { return static_cast<u32 *>(data); }
  std::byte *bytes() { return static_cast<std::byte *>(data); }
  void clip(std::size_t size) { require(ftruncate(fd, off_t(size)) == 0, "clip final file"); }
};

struct gpu {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue;
  id<MTLLibrary> library;
  std::map<std::string, id<MTLComputePipelineState>> pipelines;
  u32 value_tile_words = 4;
  explicit gpu(char const *path) {
    require(device != nil, "Metal unavailable");
    queue = [device newCommandQueue];
    NSError *error = nil;
    library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]] error:&error];
    if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
    for (auto name : {"keep_initialize", "cancel_due", "decode_values", "compact_a", "merge_order",
                     "merge_lengths", "emit_keys", "emit_values", "ef_sparse_count", "ef_low",
                     "ef_high", "ef_samples", "ef_sparse", "scan_blocks", "scan_add", "emit_values_word"}) {
      auto function = [library newFunctionWithName:[NSString stringWithUTF8String:name]];
      require(function != nil, "Metal function absent");
      auto state = [device newComputePipelineStateWithFunction:function error:&error];
      if (!state) throw std::runtime_error(error.localizedDescription.UTF8String);
      pipelines.emplace(name, state);
    }
  }
  id<MTLBuffer> buffer(std::size_t bytes) {
    auto result = [device newBufferWithLength:std::max(bytes, std::size_t(4)) options:MTLResourceStorageModeShared];
    require(result != nil, "Metal allocation");
    return result;
  }
  id<MTLBuffer> import(mapping &source) {
    auto result = [device newBufferWithBytesNoCopy:source.data length:source.length
        options:MTLResourceStorageModeShared deallocator:nil];
    require(result != nil, "Metal mmap import");
    return result;
  }
  using bindings = std::array<id<MTLBuffer>, 9>;
  using params = std::array<u32, 8>;
  void dispatch(id<MTLCommandBuffer> command, std::string const &name, u32 count,
                bindings const &buffers, params const &p) {
    if (!count) return;
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipelines.at(name)];
    for (unsigned i = 0; i < buffers.size(); ++i)
      if (i != 3 && buffers[i]) [encoder setBuffer:buffers[i] offset:0 atIndex:i];
    [encoder setBytes:p.data() length:sizeof(p) atIndex:3];
    auto group = name.starts_with("scan_") ? 256u : 128u;
    [encoder dispatchThreadgroups:MTLSizeMake((count + group - 1) / group, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
    [encoder endEncoding];
  }
  id<MTLBuffer> scan(id<MTLCommandBuffer> command, id<MTLBuffer> input, u32 n) {
    auto output = buffer(u64(n) * 4), totals = buffer(u64((n + 255) >> 8) * 4);
    dispatch(command, "scan_blocks", n, {input, output, totals}, {n});
    if (n > 256) {
      auto starts = scan(command, totals, (n + 255) >> 8);
      dispatch(command, "scan_add", n, {starts, output}, {n});
    }
    return output;
  }
  static double finish(id<MTLCommandBuffer> command) {
    [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
      throw std::runtime_error(command.error.localizedDescription.UTF8String);
    return (command.GPUEndTime - command.GPUStartTime) * 1000;
  }
};

struct input_view {
  u32 const *words;
  everett::elias_fano_view ef;
  explicit input_view(u32 const *source) : words(source) {
    if (!source[2]) return;
    auto u64s = [&](u32 offset, u32 size) {
      return std::span<u64 const>(reinterpret_cast<u64 const *>(source + offset), size);
    };
    ef = everett::elias_fano_view(u64s(source[7], source[12]), u64s(source[8], source[13]),
        std::span<everett::elias_fano_sample const>(
            reinterpret_cast<everett::elias_fano_sample const *>(source + source[9]), source[14]),
        u64s(source[10], source[15]), source[3] + 1, source[6], source[11]);
  }
  key const &key_at(u32 i) const { return reinterpret_cast<key const *>(words + words[4])[i]; }
  u32 offset(u32 i) const { return words[2] ? u32(ef.select<everett_experiment::architecture>(i)) : i * 16; }
  std::byte const *payload() const { return reinterpret_cast<std::byte const *>(words) + words[5]; }
};

// Forward-only EF decoding ignores the select directory. Consecutive value
// ranges reuse their shared boundary; cancellation gaps skip whole high words
// by popcount and need not decode the discarded low fields.
struct value_cursor {
  input_view const &source;
  u32 next_boundary = 0, high_word = 0, next_record = 0, previous_end = 0;
  u64 remaining = 0;
  bool have_previous = false;
  explicit value_cursor(input_view const &input) : source(input) {
    if (source.words[2]) remaining = source.ef.high_words()[0];
  }
  u32 boundary(u32 ordinal) {
    auto skip = ordinal - next_boundary;
    while (skip) {
      auto population = u32(std::popcount(remaining));
      if (skip >= population) {
        skip -= population;
        remaining = source.ef.high_words()[++high_word];
      } else {
        do { remaining &= remaining - 1; } while (--skip);
      }
    }
    while (!remaining) remaining = source.ef.high_words()[++high_word];
    auto position = u64(high_word) * 64 + std::countr_zero(remaining);
    remaining &= remaining - 1;
    auto width = source.words[11];
    u64 low = 0;
    if (width) {
      auto bit = u64(ordinal) * width, word = bit >> 6;
      auto shift = unsigned(bit & 63);
      low = source.ef.low_words()[word] >> shift;
      if (shift + width > 64) low |= source.ef.low_words()[word + 1] << (64 - shift);
      low &= (u64{1} << width) - 1;
    }
    next_boundary = ordinal + 1;
    return u32(((position - ordinal) << width) | low);
  }
  std::pair<u32, u32> range(u32 ordinal) {
    if (!source.words[2]) return {ordinal * 16, (ordinal + 1) * 16};
    auto start = have_previous && ordinal == next_record ? previous_end : boundary(ordinal);
    previous_end = boundary(ordinal + 1);
    next_record = ordinal + 1; have_previous = true;
    return {start, previous_end};
  }
};

void write_ef(std::byte *out, layout const &l, everett::elias_fano const &ef) {
  auto copy = [&](u32 word_offset, auto const &data) {
    if (!data.empty()) std::memcpy(out + word_offset * 4, data.data(), data.size() * sizeof(data[0]));
  };
  copy(l.h[7], ef.low); copy(l.h[8], ef.high); copy(l.h[9], ef.samples); copy(l.h[10], ef.sparse);
}

std::vector<std::byte> encode(std::vector<record> const &records, bool variable, u32 identity) {
  std::vector<u64> offsets{0};
  for (auto const &r : records) offsets.push_back(offsets.back() + r.value.size());
  auto ef = variable ? everett::elias_fano::build<everett_experiment::architecture>(offsets) : everett::elias_fano{};
  layout l(variable, narrow(records.size()), narrow(offsets.back()), narrow(ef.sparse.size()), identity);
  std::vector<std::byte> out(l.h[16]);
  std::memcpy(out.data(), l.h.data(), sizeof(l.h));
  for (u32 i = 0; i < records.size(); ++i) {
    std::memcpy(out.data() + 256 + i * 16, records[i].k.data(), 16);
    if (!records[i].value.empty())
      std::memcpy(out.data() + l.h[5] + offsets[i], records[i].value.data(), records[i].value.size());
  }
  if (variable) write_ef(out.data(), l, ef);
  return out;
}

template <bool Forward = false>
u32 cpu_merge(input_view const &a, input_view const &b, u32 const *due, std::byte *out) {
  auto na = a.words[3], nb = b.words[3], nc = due[1], n = na + nb - nc;
  auto bytes = a.words[6] + b.words[6] - due[4];
  layout l(a.words[2], n, bytes);
  auto keys = reinterpret_cast<key *>(out + 256);
  std::vector<u64> offsets;
  if (a.words[2]) offsets.reserve(n + 1);
  value_cursor ac(a), bc(b);
  u32 ai = 0, bi = 0, ci = 0, oi = 0, written = 0;
  while (ai < na || bi < nb) {
    if (ci < nc && ai == due[8 + ci]) { ++ci; ++ai; continue; }
    bool take_a = ai < na && (bi == nb || a.key_at(ai) < b.key_at(bi));
    auto const &source = take_a ? a : b;
    auto i = take_a ? ai++ : bi++;
    keys[oi++] = source.key_at(i);
    u32 start, end;
    if constexpr (Forward) {
      auto range = (take_a ? ac : bc).range(i);
      start = range.first; end = range.second;
    } else { start = source.offset(i); end = source.offset(i + 1); }
    if (a.words[2]) offsets.push_back(written);
    if (end > start) std::memcpy(out + l.h[5] + written, source.payload() + start, end - start);
    written += end - start;
  }
  require(oi == n && written == bytes && ci == nc, "CPU schedule count/extent mismatch");
  if (a.words[2]) {
    offsets.push_back(written);
    auto ef = everett::elias_fano::build<everett_experiment::architecture>(offsets);
    l = layout(true, n, bytes, narrow(ef.sparse.size()));
    std::memset(out + l.h[5] + bytes, 0, l.h[7] * 4 - l.h[5] - bytes);
    write_ef(out, l, ef);
  }
  std::memcpy(out, l.h.data(), sizeof(l.h));
  return l.h[16];
}

struct timing { double total_ms = 0, gpu_ms = 0; u32 bytes = 0, sparse = 0; };
timing gpu_merge_impl(gpu &g, id<MTLBuffer> a, id<MTLBuffer> b, id<MTLBuffer> due,
                 id<MTLBuffer> output, u32 const *ah, u32 const *bh, u32 const *dh) {
  auto start = clock_type::now();
  auto na = ah[3], nb = bh[3], nc = dh[1];
  require(dh[2] == na && dh[3] == ah[18] && nc <= na && dh[4] <= ah[6], "due target identity/extent");
  auto n = na + nb - nc, survivors = na - nc;
  auto bytes = ah[6] + bh[6] - dh[4];
  bool variable = ah[2];
  layout l(variable, n, bytes);
  auto refs = g.buffer(u64(na + nb) * 4);
  id<MTLBuffer> flags = nil, live = nil, rank = nil;
  auto status = g.buffer(4); *static_cast<u32 *>(status.contents) = 0;
  id<MTLBuffer> desc_a = nil, desc_b = nil, lengths = nil, offsets = nil, sparse = nil, sparse_prefix = nil;
  auto command = [g.queue commandBuffer];
  if (nc) {
    flags = g.buffer(u64(na) * 4); live = g.buffer(u64(na) * 4);
    g.dispatch(command, "keep_initialize", na, {nil, flags}, {na});
    g.dispatch(command, "cancel_due", nc, {due, flags, status}, {nc, na});
    rank = g.scan(command, flags, na);
    g.dispatch(command, "compact_a", na, {flags, live, status, nil, rank}, {na});
  }
  g.dispatch(command, "merge_order", (n + 31) >> 5, {a, refs, status, nil, b, live}, {n, na, nb, survivors, 0, 0, 0, u32(nc != 0)});
  if (variable) {
    desc_a = g.buffer(u64(na) * 8); desc_b = g.buffer(u64(nb) * 8);
    g.dispatch(command, "decode_values", na, {a, desc_a, status, nil, flags}, {na, u32(nc != 0)});
    g.dispatch(command, "decode_values", nb, {b, desc_b, status}, {nb});
    lengths = g.buffer(u64(n) * 4);
    g.dispatch(command, "merge_lengths", n, {refs, lengths, status, nil, desc_a, desc_b}, {n, na});
    offsets = g.scan(command, lengths, n);
    sparse = g.buffer(u64(l.h[14]) * 4);
    g.dispatch(command, "ef_sparse_count", l.h[14], {offsets, sparse}, {n + 1, 0, 0, 0, bytes, l.h[11]});
    sparse_prefix = g.scan(command, sparse, l.h[14]);
  }
  double gpu_ms = g.finish(command);
  require(*static_cast<u32 *>(status.contents) == 0, "GPU input/schedule error");
  if (nc) require(static_cast<u32 *>(rank.contents)[na - 1] + static_cast<u32 *>(flags.contents)[na - 1] == survivors,
                  "GPU cancellation count");
  if (variable) {
    u32 actual = n ? static_cast<u32 *>(offsets.contents)[n - 1] + static_cast<u32 *>(lengths.contents)[n - 1] : 0;
    require(actual == bytes, "GPU payload extent");
    auto groups = l.h[14];
    auto exceptions = static_cast<u32 *>(sparse.contents)[groups - 1] + static_cast<u32 *>(sparse_prefix.contents)[groups - 1];
    l = layout(true, n, bytes, exceptions);
  }
  std::memcpy(output.contents, l.h.data(), sizeof(l.h));
  if (variable) std::memset(static_cast<std::byte *>(output.contents) + l.h[5] + bytes, 0,
                            l.h[7] * 4 - l.h[5] - bytes);
  command = [g.queue commandBuffer];
  g.dispatch(command, "emit_keys", n, {a, output, status, nil, b, refs}, {n, na});
  auto tile_bytes = g.value_tile_words * 4;
  g.dispatch(command, variable && g.value_tile_words == 1 ? "emit_values_word" : "emit_values",
      variable ? (bytes + tile_bytes - 1) / tile_bytes : (bytes + 3) >> 2,
      {a, output, status, nil, b, refs, desc_a, desc_b, offsets}, {n, na, 0, 0, bytes, 0, l.h[5], u32(variable)});
  if (variable) {
    auto bindings = gpu::bindings{offsets, output, status, nil, sparse, sparse_prefix};
    auto emit = [&](char const *name, u32 count, u32 destination) {
      g.dispatch(command, name, count, bindings, {n + 1, 0, 0, 0, bytes, l.h[11], destination});
    };
    emit("ef_low", l.h[12] * 2, l.h[7]); emit("ef_high", l.h[13] * 2, l.h[8]);
    emit("ef_samples", l.h[14], l.h[9]); emit("ef_sparse", n + 1, l.h[10]);
  }
  gpu_ms += g.finish(command);
  require(*static_cast<u32 *>(status.contents) == 0, "GPU output error");
  return {elapsed(start), gpu_ms, l.h[16], l.h[15]};
}

timing gpu_merge(gpu &g, id<MTLBuffer> a, id<MTLBuffer> b, id<MTLBuffer> due,
                 id<MTLBuffer> output, u32 const *ah, u32 const *bh, u32 const *dh) {
  auto start = clock_type::now();
  timing result;
  @autoreleasepool { result = gpu_merge_impl(g, a, b, due, output, ah, bh, dh); }
  result.total_ms = elapsed(start); // Includes intermediate buffer reclamation.
  return result;
}


double median(std::vector<double> values) {
  std::sort(values.begin(), values.end()); return values[values.size() / 2];
}

void run_case(gpu &g, fixture const &f, std::filesystem::path const &directory, unsigned repeats) {
  auto fixture_start = clock_type::now();
  auto a_data = encode(f.a, f.variable, 1001), b_data = encode(f.b, f.variable, 1002);
  auto fixture_ms = elapsed(fixture_start);
  auto setup_start = clock_type::now();
  auto ah = reinterpret_cast<u32 const *>(a_data.data()), bh = reinterpret_cast<u32 const *>(b_data.data());
  auto n = ah[3] + bh[3] - f.due[1], bytes = ah[6] + bh[6] - f.due[4];
  layout output_layout(f.variable, n, bytes);
  mapping a_map(directory / "input-a.tmp", a_data.size()), b_map(directory / "input-b.tmp", b_data.size());
  mapping due_map(directory / "due.tmp", f.due.size() * 4);
  mapping cpu_map(directory / "cpu.tmp", output_layout.capacity()), gpu_map(directory / "gpu.tmp", output_layout.capacity());
  mapping forward_map(directory / "forward.tmp", output_layout.capacity());
  std::memcpy(a_map.data, a_data.data(), a_data.size()); std::memcpy(b_map.data, b_data.data(), b_data.size());
  std::memcpy(due_map.data, f.due.data(), f.due.size() * 4);
  auto a = g.import(a_map), b = g.import(b_map), due = g.import(due_map), output = g.import(gpu_map);
  auto setup_ms = elapsed(setup_start);
  input_view av(a_map.words()), bv(b_map.words());
  std::vector<double> cpu_times, forward_times, gpu_times, device_times;
  timing result;
  auto cpu = [&](bool forward) {
    auto start = clock_type::now();
    auto size = forward ? cpu_merge<true>(av, bv, due_map.words(), forward_map.bytes())
                        : cpu_merge<false>(av, bv, due_map.words(), cpu_map.bytes());
    auto ms = elapsed(start);
    return std::pair(size, ms);
  };
  auto gpu_run = [&] { return gpu_merge(g, a, b, due, output, a_map.words(), b_map.words(), due_map.words()); };
  auto compare = [&](u32 size) {
    require(size == result.bytes, "CPU/GPU output size mismatch");
    if (std::memcmp(cpu_map.data, gpu_map.data, size)) {
      for (u32 i = 0; i < size; ++i) if (cpu_map.bytes()[i] != gpu_map.bytes()[i])
        throw std::runtime_error(f.name + ": output mismatch at byte " + std::to_string(i));
    }
    require(std::memcmp(cpu_map.data, forward_map.data, size) == 0, "forward CPU output mismatch");
    if (f.variable) {
      input_view check(gpu_map.words());
      require(check.offset(n) == bytes, "GPU output EOF select");
    }
  };
  auto warm_cpu = cpu(false); auto warm_forward = cpu(true);
  require(warm_cpu.first == warm_forward.first, "forward CPU output extent");
  result = gpu_run(); compare(warm_cpu.first);
  // Rotate all three orders so no path always inherits the warmest inputs.
  for (unsigned i = 0; i < repeats; ++i) {
    std::pair<u32, double> c, fwd;
    if (i % 3 == 0) { c = cpu(false); fwd = cpu(true); result = gpu_run(); }
    else if (i % 3 == 1) { fwd = cpu(true); result = gpu_run(); c = cpu(false); }
    else { result = gpu_run(); c = cpu(false); fwd = cpu(true); }
    require(c.first == fwd.first, "forward CPU output extent");
    compare(c.first); cpu_times.push_back(c.second); forward_times.push_back(fwd.second);
    gpu_times.push_back(result.total_ms); device_times.push_back(result.gpu_ms);
  }
  auto clip_start = clock_type::now();
  cpu_map.clip(result.bytes); gpu_map.clip(result.bytes); forward_map.clip(result.bytes);
  auto clip_ms = elapsed(clip_start);
  std::cout << f.name << ',' << (f.variable ? "fv" : "ff") << ',' << (f.variable ? g.value_tile_words : 1) << ',' << ah[3] << ',' << bh[3] << ','
      << f.due[1] << ',' << n << ',' << bytes << ',' << result.bytes << ',' << result.sparse << ','
      << median(cpu_times) << ',' << median(gpu_times) << ',' << median(device_times) << ','
      << fixture_ms << ',' << setup_ms << ',' << clip_ms << ',' << repeats << ','
      << *std::min_element(cpu_times.begin(), cpu_times.end()) << ',' << *std::max_element(cpu_times.begin(), cpu_times.end()) << ','
      << *std::min_element(gpu_times.begin(), gpu_times.end()) << ',' << *std::max_element(gpu_times.begin(), gpu_times.end()) << ','
      << median(forward_times) << ',' << *std::min_element(forward_times.begin(), forward_times.end()) << ','
      << *std::max_element(forward_times.begin(), forward_times.end()) << '\n';
}

int main(int argc, char **argv) {
  @autoreleasepool {
    try {
      require(argc >= 3, "usage: prototype kernels.metallib scratch-directory [quick|bench]");
      std::filesystem::path directory(argv[2]); std::filesystem::create_directories(directory);
      auto compile_start = clock_type::now(); gpu g(argv[1]);
      std::cerr << "device=" << g.device.name.UTF8String << " pipeline_load_ms=" << elapsed(compile_start) << '\n';
      std::cout << "case,layout,value_tile_words,older,newer,canceled,output_records,payload_bytes,file_bytes,sparse_entries,cpu_ms,gpu_total_ms,gpu_device_ms,fixture_encode_ms,map_import_ms,clip_ms,repeats,cpu_min_ms,cpu_max_ms,gpu_min_ms,gpu_max_ms,cpu_forward_ms,cpu_forward_min_ms,cpu_forward_max_ms\n";
      for (bool variable : {false, true}) {
        for (auto counts : {std::array<u32, 3>{0, 0, 0}, {0, 33, 0}, {33, 0, 0}, {31, 0, 100},
                            {257, 263, 0}, {513, 257, 50}, {513, 19, 90}, {31, 1009, 50}})
          run_case(g, make_fixture("check-" + std::to_string(counts[0]) + "-" + std::to_string(counts[1]) + "-" + std::to_string(counts[2]),
              variable, counts[0], counts[1], counts[2]), directory, 1);
        run_case(g, make_fixture("scattered-half", variable, 4099, 2057, 50, false, true), directory, 1);
        run_case(g, make_fixture("scattered-heavy", variable, 4099, 127, 90, false, true), directory, 1);
      }
      run_case(g, make_fixture("sparse-exceptions", true, 16384, 16384, 0, true), directory, 1);
      run_case(g, make_fixture("zero-values", true, 0, 1025, 0, true), directory, 1);
      auto tiny = make_fixture("tiny-values", true, 259, 517, 50, false, true);
      for (u32 i = 0; i < tiny.a.size(); ++i) tiny.a[i].value.resize(i % 7);
      for (u32 i = 0; i < tiny.b.size(); ++i) tiny.b[i].value.resize(i % 5);
      tiny.due[4] = 0;
      for (u32 i = 0; i < tiny.due[1]; ++i) tiny.due[4] += u32(tiny.a[tiny.due[8 + i]].value.size());
      run_case(g, tiny, directory, 1);
      g.value_tile_words = 1;
      run_case(g, make_fixture("word-scattered-check", true, 1027, 1029, 50, false, true), directory, 1);
      g.value_tile_words = 4;
      if (argc >= 4 && std::string(argv[3]) == "bench") {
        for (bool variable : {false, true})
          for (u32 n : {4096u, 16384u, 32768u, 65536u, 131072u, 262144u})
            for (u32 percent : {0u, 50u, 90u})
              run_case(g, make_fixture("balanced-" + std::to_string(n) + "-" + std::to_string(percent), variable, n, n, percent), directory, 5);
        for (bool variable : {false, true})
          for (auto counts : {std::array<u32, 2>{262144, 4096}, {4096, 262144}})
            run_case(g, make_fixture("skew-" + std::to_string(counts[0]), variable, counts[0], counts[1], 50), directory, 5);
        for (bool variable : {false, true})
          run_case(g, make_fixture("scattered-262144", variable, 262144, 262144, 50, false, true), directory, 5);
        for (u32 n : {4096u, 16384u, 32768u, 65536u, 131072u, 262144u})
          for (u32 percent : {0u, 50u})
            run_case(g, make_fixture("tiny-" + std::to_string(n) + "-" + std::to_string(percent),
                true, n, n, percent, false, false, value_shape::tiny), directory, 5);
        for (u32 n : {4096u, 16384u, 32768u})
          for (u32 percent : {0u, 50u})
            run_case(g, make_fixture("large-" + std::to_string(n) + "-" + std::to_string(percent),
                true, n, n, percent, false, false, value_shape::large), directory, 5);
        g.value_tile_words = 1;
        for (u32 percent : {0u, 50u})
          run_case(g, make_fixture("word-balanced-262144-" + std::to_string(percent), true, 262144, 262144, percent), directory, 5);
      }
      std::cerr << "All complete outputs matched the CPU oracle.\n";
    } catch (std::exception const &e) { std::cerr << e.what() << '\n'; return 1; }
  }
}
