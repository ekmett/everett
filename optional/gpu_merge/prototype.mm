/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises optional Metal construction kernels against Everett CPU
 * formats.
 *
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../host_backend.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <everett/rank.h>
#include <everett/rank15.h>
#include <everett/sort_profile_file.h>
#include <everett/sort_profile_merge.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using clock_type = std::chrono::steady_clock;
double elapsed(clock_type::time_point t) {
  return std::chrono::duration<double, std::milli>(clock_type::now() - t).count();
}
void require(bool value, char const *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct gpu {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue;
  id<MTLLibrary> library;
  std::map<std::string, id<MTLComputePipelineState>> pipelines;
  explicit gpu(char const *path) {
    require(device != nil, "Metal device absent");
    queue = [device newCommandQueue];
    NSError *error = nil;
    library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
                                  error:&error];
    if (!library)
      throw std::runtime_error(error.localizedDescription.UTF8String);
  }
  id<MTLBuffer> buffer(std::size_t bytes, void const *data = nullptr) {
    auto result = [device newBufferWithLength:std::max<std::size_t>(bytes, 4)
                                      options:MTLResourceStorageModeShared];
    require(result != nil, "Metal buffer allocation");
    if (data && bytes)
      std::memcpy(result.contents, data, bytes);
    return result;
  }
  id<MTLComputePipelineState> pipeline(std::string const &name) {
    auto found = pipelines.find(name);
    if (found != pipelines.end())
      return found->second;
    NSError *error = nil;
    auto function = [library newFunctionWithName:[NSString stringWithUTF8String:name.c_str()]];
    require(function != nil, "Metal function absent");
    auto state = [device newComputePipelineStateWithFunction:function error:&error];
    if (!state)
      throw std::runtime_error(error.localizedDescription.UTF8String);
    pipelines.emplace(name, state);
    return state;
  }
  void dispatch(id<MTLCommandBuffer> command, std::string const &name, std::uint32_t threads,
                id<MTLBuffer> input, id<MTLBuffer> output, id<MTLBuffer> totals, std::uint32_t n,
                id<MTLBuffer> extra = nil, id<MTLBuffer> refs = nil, id<MTLBuffer> frames = nil,
                std::uint32_t p1 = 0, std::uint32_t p2 = 0, std::uint32_t p3 = 0,
                std::uint32_t p4 = 0, std::uint32_t p5 = 0, std::uint32_t p6 = 0,
                id<MTLBuffer> other = nil, id<MTLBuffer> prefix = nil, id<MTLBuffer> tree = nil,
                id<MTLBuffer> tombstones = nil) {
    if (!threads)
      return;
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline(name)];
    if (input)
      [encoder setBuffer:input offset:0 atIndex:0];
    if (output)
      [encoder setBuffer:output offset:0 atIndex:1];
    if (totals)
      [encoder setBuffer:totals offset:0 atIndex:2];
    std::uint32_t params[]{n, p1, p2, p3, p4, p5, p6};
    [encoder setBytes:params length:sizeof(params) atIndex:3];
    if (extra)
      [encoder setBuffer:extra offset:0 atIndex:4];
    if (refs)
      [encoder setBuffer:refs offset:0 atIndex:5];
    if (frames)
      [encoder setBuffer:frames offset:0 atIndex:6];
    if (other)
      [encoder setBuffer:other offset:0 atIndex:7];
    if (prefix)
      [encoder setBuffer:prefix offset:0 atIndex:8];
    if (tree)
      [encoder setBuffer:tree offset:0 atIndex:9];
    if (tombstones)
      [encoder setBuffer:tombstones offset:0 atIndex:10];
    auto group = name == "compressed_prefix"                   ? 1u
                 : name == "scan_blocks" || name == "scan_add" ? 256u
                                                               : 128u;
    [encoder dispatchThreadgroups:MTLSizeMake((threads + group - 1) / group, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
    [encoder endEncoding];
  }
  void build_prefix_tree(id<MTLCommandBuffer> command, id<MTLBuffer> tree,
                         id<MTLBuffer> status, id<MTLBuffer> descriptors,
                         std::uint32_t count, std::uint32_t older, bool tiled) {
    auto base = std::bit_ceil(count);
    if (tiled) {
      dispatch(command, "prefix_leaf_tiles", ((base + 255) / 256) * 128,
               nil, tree, status, count, descriptors, nil, nil, older, count - older, 0, 0, 0, base);
      for (auto level = base / 256; level > 1; level /= 256)
        dispatch(command, "prefix_reduce_tiles", ((level + 255) / 256) * 128,
                 tree, tree, nil, level);
    } else {
      dispatch(command, "prefix_leaf", base, nil, tree, status, count,
               descriptors, nil, nil, older, count - older, 0, 0, 0, base);
      for (auto first = base / 2; first; first /= 2)
        dispatch(command, "prefix_reduce", first, tree, tree, nil, first, nil, nil, nil, first);
    }
  }
  id<MTLBuffer> scan(id<MTLCommandBuffer> command, id<MTLBuffer> input, std::uint32_t n) {
    auto output = buffer(n * 4), totals = buffer(((n + 255) / 256) * 4);
    dispatch(command, "scan_blocks", n, input, output, totals, n);
    if (n > 256) {
      auto starts = scan(command, totals, (n + 255) / 256);
      dispatch(command, "scan_add", n, starts, output, nil, n);
    }
    return output;
  }
  static void finish(id<MTLCommandBuffer> command) {
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
      throw std::runtime_error(command.error.localizedDescription.UTF8String);
  }
};
#include "collision_rank_test.h"
#include "prefix_tree_test.h"
#include "output_plan_test.h"
#include "ef_input_test.h"
#include "ef_output_test.h"
#include "emit_word_test.h"
struct mapping {
  int fd = -1;
  std::size_t length = 0;
  void *data = MAP_FAILED;
  explicit mapping(std::filesystem::path const &path, std::size_t bytes) {
    auto page = std::size_t(sysconf(_SC_PAGESIZE));
    length = (bytes + page - 1) / page * page;
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    require(fd >= 0, "open scratch");
    require(ftruncate(fd, off_t(length)) == 0, "truncate scratch");
    data = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    require(data != MAP_FAILED, "map scratch");
  }
  ~mapping() {
    if (data != MAP_FAILED)
      munmap(data, length);
    if (fd >= 0)
      close(fd);
  }
};
void probe(gpu &context, std::filesystem::path const &directory) {
  mapping source(directory / "probe-input.bin", 65536),
      target(directory / "probe-output.bin", 65536);
  auto words = source.length / 4;
  auto p = static_cast<std::uint32_t *>(source.data);
  for (std::size_t i = 0; i < words; i++)
    p[i] = std::uint32_t(i * 713 + 19);
  auto input = [context.device newBufferWithBytesNoCopy:source.data
                                                 length:source.length
                                                options:MTLResourceStorageModeShared
                                            deallocator:nil];
  auto output = [context.device newBufferWithBytesNoCopy:target.data
                                                  length:target.length
                                                 options:MTLResourceStorageModeShared
                                             deallocator:nil];
  require(input && output, "file mmap no-copy buffer rejected");
  auto command = [context.queue commandBuffer];
  context.dispatch(command, "probe_copy", std::uint32_t(words), input, output, nil,
                   std::uint32_t(words));
  gpu::finish(command);
  for (std::size_t i = 0; i < words; i++)
    require(static_cast<std::uint32_t *>(target.data)[i] == (p[i] ^ 0x5a5a5a5a),
            "mmap GPU output mismatch");
  require(msync(target.data, target.length, MS_SYNC) == 0, "probe msync");
  std::cout << "device," << context.device.name.UTF8String << ",unified,"
            << context.device.hasUnifiedMemory << ",max_buffer," << context.device.maxBufferLength
            << ",page," << sysconf(_SC_PAGESIZE) << ",mmap_no_copy,passed\n";
}
std::vector<std::uint8_t> classes(std::span<std::uint64_t const> words, std::uint32_t bits) {
  std::vector<std::uint8_t> result((bits + 14) / 15);
  for (std::size_t i = 0; i < result.size(); i++) {
    auto first = i * 15, wi = first / 64, shift = first % 64;
    auto x = words[wi] >> shift;
    if (shift > 49 && wi + 1 < words.size())
      x |= words[wi + 1] << (64 - shift);
    auto count = std::min<std::size_t>(15, bits - first);
    result[i] = std::uint8_t(std::popcount(x & ((std::uint64_t{1} << count) - 1)));
  }
  return result;
}
void rank_case(gpu &context, std::uint32_t bits, unsigned trials) {
  std::mt19937_64 random(bits + 13579);
  std::vector<std::uint64_t> source((std::uint64_t(bits) + 63) / 64);
  for (auto &word : source)
    word = random();
  if (bits % 64)
    source.back() &= (std::uint64_t{1} << (bits % 64)) - 1;
  auto cpu = everett::rank_index::build<everett_experiment::architecture>(source, bits);
  auto c = classes(source, bits);
  auto cpu15 = everett::rank15_index::build(c, bits);
  for (unsigned trial = 0; trial < trials; trial++) {
    auto begin = clock_type::now();
    auto cr = everett::rank_index::build<everett_experiment::architecture>(source, bits);
    double cpu_ms = elapsed(begin);
    begin = clock_type::now();
    auto cc = classes(source, bits);
    auto cr15 = everett::rank15_index::build(cc, bits);
    double cpu15_ms = elapsed(begin);
    for (unsigned mode = 0; mode < 2; mode++) {
      begin = clock_type::now();
      auto input = context.buffer(source.size() * 8, source.data());
      auto groups = mode ? (bits + 14) / 15 : (bits + 2047) / 2048;
      auto blocks = mode ? (groups + 127) / 128 : groups;
      auto output = context.buffer(mode ? ((groups + 15) / 16) * 8 : blocks * 8);
      auto checkpoints = context.buffer(blocks * 8);
      auto total = context.buffer(blocks * 4);
      auto command = [context.queue commandBuffer];
      context.dispatch(command, mode ? "rank15_count" : "rank_count", blocks, input, output, total,
                       bits);
      auto starts = context.scan(command, total, blocks);
      context.dispatch(command, mode ? "rank15_finish" : "rank_finish", blocks, starts,
                       mode ? checkpoints : output, nil, blocks);
      gpu::finish(command);
      double all_ms = elapsed(begin);
      double gpu_ms = (command.GPUEndTime - command.GPUStartTime) * 1000;
      if (mode) {
        require(std::memcmp(output.contents, cr15.classes.data(), cr15.classes.size() * 8) == 0,
                "rank15 class mismatch");
        require(std::memcmp(checkpoints.contents, cr15.checkpoints.data(),
                            cr15.checkpoints.size() * 8) == 0,
                "rank15 checkpoint mismatch");
      } else
        require(std::memcmp(output.contents, cr.blocks.data(), cr.blocks.size() * 8) == 0,
                "rank block mismatch");
      std::cout << "rank," << (mode ? "rank15" : "rank2048") << ',' << bits << ',' << trial << ','
                << (mode ? cpu15_ms : cpu_ms) << ',' << all_ms << ',' << gpu_ms << ','
                << cpu.view().count() << '\n';
    }
  }
}
using policy = everett_experiment::string_policy;
using sort_type = everett::unsorted<std::optional<std::string>>;
using native = everett::mapped_sort_profile<policy>;
#include "prepared_input.h"
using everett_gpu::compressed_blocks;
using everett_gpu::compressed_descriptor;
bool compressed_inputs = false;
bool use_prefix_cache = false;
bool use_prefix_tiles = false;
bool use_output_plan = false;
bool use_word_emitter = false;
bool gpu_output_ef = false;
bool verify_compressed_descriptors = false;
enum class collision_path { disabled, direct, tiled, compact };
collision_path cancellation_path = collision_path::disabled;
bool collision_2048 = false;
struct imported_payload {
  id<MTLBuffer> buffer;
  std::uint32_t delta = 0;
  std::uintptr_t base = 0;
  imported_payload(gpu &context, compressed_blocks const &input) {
    auto first = std::numeric_limits<std::uintptr_t>::max(), last = std::uintptr_t{0};
    auto include = [&](std::span<std::byte const> section) {
      if (section.empty())
        return;
      auto at = reinterpret_cast<std::uintptr_t>(section.data());
      first = std::min(first, at);
      last = std::max(last, at + section.size());
    };
    include(input.payload);
    for (auto part : input.ef_sections)
      include(part);
    require(last > first, "empty source mapping sections");
    auto page = std::size_t(sysconf(_SC_PAGESIZE));
    base = first / page * page;
    auto length = (last - base + page - 1) / page * page;
    auto payload = reinterpret_cast<std::uintptr_t>(input.payload.data());
    require(payload >= base && payload - base <= std::numeric_limits<std::uint32_t>::max(),
            "GPU payload delta");
    delta = std::uint32_t(payload - base);
    buffer = [context.device newBufferWithBytesNoCopy:reinterpret_cast<void *>(base)
                                               length:length
                                              options:MTLResourceStorageModeShared
                                          deallocator:nil];
    require(buffer != nil, "read-only input mmap import rejected");
  }
};
struct descriptor {
  std::uint32_t key_at, key_bytes, value_at, value_bits;
};
struct decoded {
  std::vector<std::byte> arena;
  std::vector<descriptor> records;
  void append(native const &input) {
    everett::sort_profile_cursor<policy, everett::registry_selector<typename policy::registry_type>>
        cursor(input.view());
    while (!cursor.done()) {
      auto item = cursor.peek();
      auto key = item.key.prefix.subview(1, item.key.prefix.size() - 1);
      require(key.size() % 8 == 0, "default string key width");
      require(key.size() < (1ull << 31) && item.value.size() < (1ull << 31),
              "prototype record bit bound");
      require(arena.size() + key.size() / 8 + (item.value.size() + 7) / 8 < (1ull << 30),
              "prototype arena byte bound");
      descriptor d{std::uint32_t(arena.size()), std::uint32_t(key.size() / 8), 0,
                   std::uint32_t(item.value.size())};
      arena.resize(arena.size() + d.key_bytes);
      everett::profile_detail::copy_bits(arena.data() + d.key_at, 0, key);
      d.value_at = std::uint32_t(arena.size());
      arena.resize(arena.size() + (d.value_bits + 7) / 8);
      everett::profile_detail::copy_bits(arena.data() + d.value_at, 0, item.value);
      records.push_back(d);
      cursor.advance();
    }
  }
};
void write_bytes(std::filesystem::path const &path, std::span<std::byte const> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
  require(bool(stream), "write fixture");
}
std::shared_ptr<native const> make_input(std::filesystem::path const &path, std::uint32_t n,
                                         unsigned side, unsigned prefix, unsigned duplicates) {
  everett::sort_profile_writer<policy> writer;
  for (std::uint32_t i = 0; i < n; i++) {
    // Both sources remain strictly ordered. Shared prefixes include binary
    // zeros.
    auto id = std::uint64_t(i) * 4 + (side && i % 100 >= duplicates ? 1 : 0);
    std::string key(prefix, 'p');
    if (prefix > 4)
      key[prefix / 2] = '\0';
    for (int byte = 7; byte >= 0; --byte)
      key.push_back(char(id >> (byte * 8)));
    std::optional<std::string> value;
    if ((i + side) % 11)
      value = std::string(8 + (i % 13), char('a' + side));
    writer.append<sort_type>(key, value);
  }
  auto array = writer.finish();
  auto wire = everett::encoded_sort_sections<policy>::from(array).materialize();
  write_bytes(path, wire);
  auto result = std::make_shared<native const>(native::open(everett::file<policy>::open(path)));
  result->scan();
  return result;
}
struct merge_result {
  std::filesystem::path path;
  double decode = 0, prepare = 0, order = 0, size = 0, emit = 0, assembly = 0, total = 0,
         gpu_ms = 0;
  std::uint32_t count = 0, bits = 0;
  std::size_t bytes = 0;
};
// This is caller-supplied semantic authority. Two arbitrary runs cannot prove
// the absence of older history. No durable-runtime dispatcher currently calls
// this optional prototype.
enum class merge_coverage { preserve_tombstones, complete_older_history };
bool conservative_tombstones = false;
merge_result gpu_merge(gpu &context, native const &a, native const &b,
                       std::filesystem::path const &path,
                       merge_coverage coverage = merge_coverage::preserve_tombstones) {
  require(coverage == merge_coverage::preserve_tombstones || conservative_tombstones,
          "cleanup requires conservative tombstone mode and explicit complete older coverage");
  require(!conservative_tombstones || (compressed_inputs && use_word_emitter &&
          cancellation_path == collision_path::disabled), "tombstone pipeline configuration");
  auto start = clock_type::now(), phase = start;
  merge_result result;
  result.path = path;
  id<MTLBuffer> arena = nil, desc = nil, other = nil, prefix = nil, tree = nil;
  std::uint32_t n = 0, shared_prefix = 0, delta_a = 0, delta_b = 0, tree_base = 0;
  unsigned descriptor_words = compressed_inputs ? 8 : 4;
  if (compressed_inputs) {
    compressed_blocks left(a), right(b);
    auto total = std::uint64_t(left.count) + right.count;
    require(total > 0 && total < (1ull << 24), "compressed merge record bound");
    n = std::uint32_t(total);
    tree_base = std::bit_ceil(n);
    imported_payload left_map(context, left), right_map(context, right);
    arena = left_map.buffer;
    other = right_map.buffer;
    delta_a = left_map.delta;
    delta_b = right_map.delta;
    desc = context.buffer(std::size_t(n) * sizeof(compressed_descriptor));
    auto status = context.buffer(8);
    std::memset(status.contents, 0, 8);
    auto left_packet = left.ef_parameters(left_map.base),
         right_packet = right.ef_parameters(right_map.base);
    auto left_offsets = context.buffer(sizeof(left_packet), left_packet.data());
    auto right_offsets = context.buffer(sizeof(right_packet), right_packet.data());
    tree = context.buffer(std::size_t(tree_base) * 8);
    prefix = context.buffer(4);
    auto parse = [context.queue commandBuffer];
    context.dispatch(parse, "parse_input", left.block_count, arena, desc, status, left.count, nil,
                     left_offsets, nil, 0, 0, left.block_count, delta_a, left.extent);
    context.dispatch(parse, "parse_input", right.block_count, other, desc, status, right.count, nil,
                     right_offsets, nil, left.count, 1, right.block_count, delta_b, right.extent);
    context.build_prefix_tree(parse, tree, status, desc, n, left.count, use_prefix_tiles);
    gpu::finish(parse);
    auto flags = static_cast<std::uint32_t const *>(status.contents);
    require(flags[0] == 0, "GPU input framing/retention validation failed");
    require(std::uint64_t(flags[1]) * n < (1ull << 31), "compressed merge worst-case output bound");
    auto records = static_cast<compressed_descriptor const *>(desc.contents);
    if (left.count)
      require(records[left.count - 1].key_bytes * 8 == left.terminal_key_bits,
              "left GPU terminal key");
    if (right.count)
      require(records[n - 1].key_bytes * 8 == right.terminal_key_bits, "right GPU terminal key");
    result.decode = elapsed(phase);
    result.gpu_ms += (parse.GPUEndTime - parse.GPUStartTime) * 1000;
    // The oracle is a correctness-mode check, explicitly absent in timings.
    if (verify_compressed_descriptors) {
      auto expected = everett_gpu::compressed_oracle(a.view(), 0);
      auto more = everett_gpu::compressed_oracle(b.view(), 1);
      expected.insert(expected.end(), more.begin(), more.end());
      require(std::equal(expected.begin(), expected.end(), records),
              "GPU frame descriptors differ from CPU parser");
      auto checks = context.buffer(std::size_t(n) * 4);
      auto query = [context.queue commandBuffer];
      context.dispatch(query, "compressed_probe", n, arena, checks, nil, n, desc, nil, nil,
                       left.count, right.count, 0, delta_a, delta_b, tree_base, other, nil, tree);
      gpu::finish(query);
      auto actual_hashes = static_cast<std::uint32_t const *>(checks.contents);
      std::size_t ordinal = 0;
      for (auto source : {&a, &b}) {
        auto cursor = source->view().cursor();
        while (!cursor.done()) {
          auto key = cursor.peek().key.prefix;
          std::uint32_t hash = 2166136261u;
          for (std::uint64_t bit = 1; bit < key.size(); bit += 8)
            hash = (hash ^ std::uint32_t(everett::profile_detail::load_bits(key, bit, 8))) * 16777619u;
          require(actual_hashes[ordinal++] == hash, "GPU inherited key reconstruction mismatch");
          cursor.advance();
        }
      }
    }
    phase = clock_type::now();
  } else {
    decoded data;
    data.append(a);
    data.append(b);
    result.decode = elapsed(phase);
    phase = clock_type::now();
    require(data.records.size() < (1ull << 24), "prototype record count bound");
    std::uint64_t worst_bits = 0;
    for (auto const &record : data.records) {
      worst_bits += std::uint64_t(record.key_bytes) * 8 + record.value_bits + 126;
      require(worst_bits < (1ull << 31), "prototype worst-case output exceeds2Gibit");
    }
    n = std::uint32_t(data.records.size());
    require(n > 0, "empty GPU merge handled separately");
    require(data.arena.size() < (1u << 30), "prototype arena exceeds1GiB");
    data.arena.resize((data.arena.size() + 3) / 4 * 4);
    arena = context.buffer(data.arena.size(), data.arena.data());
    desc = context.buffer(data.records.size() * sizeof(descriptor), data.records.data());
    shared_prefix = data.records.front().key_bytes;
    auto reduce_prefix = [&](descriptor const &other) {
      shared_prefix = std::min(shared_prefix, other.key_bytes);
      auto const &first = data.records.front();
      std::uint32_t i = 0;
      while (i < shared_prefix && data.arena[first.key_at + i] == data.arena[other.key_at + i])
        ++i;
      shared_prefix = i;
    };
    if (a.size())
      reduce_prefix(data.records[a.size() - 1]);
    if (b.size()) {
      reduce_prefix(data.records[a.size()]);
      reduce_prefix(data.records.back());
    }
  }
  auto kernel = [&](char const *name) {
    if (conservative_tombstones && (std::string_view(name) == "merge_keep" ||
                                   std::string_view(name) == "merge_sizes"))
      return "tombstone_" + std::string(name);
    if (use_prefix_cache && std::string_view(name) != "merge_emit")
      return "cached_" + std::string(name);
    return compressed_inputs ? "compressed_" + std::string(name) : std::string(name);
  };
  auto compact = context.buffer(std::size_t(n) * 4);
  id<MTLBuffer> tombstones = conservative_tombstones ? context.buffer(std::size_t(n) * 12) : nil;
  id<MTLBuffer> ordered = nil, keep = nil, count = nil;
  id<MTLBuffer> collision_map = nil, collision_directory = nil, collision_counts = nil,
                collision_positions = nil, surviving_a = nil;
  auto a_count = std::uint32_t(a.size()), b_count = std::uint32_t(b.size());
  auto collision_words = (a_count + 31) / 32;
  auto collision_blocks = (a_count + (collision_2048 ? 2047 : 511)) / (collision_2048 ? 2048 : 512);
  if (cancellation_path == collision_path::disabled) {
    ordered = context.buffer(std::size_t(n) * 4);
    keep = context.buffer(std::size_t(n) * 4);
    count = context.buffer(4);
  } else {
    auto storage_words = collision_words + (collision_2048 ? 0 : collision_blocks * 3 + 1);
    collision_map = context.buffer(std::size_t(storage_words) * 4);
    collision_directory =
        collision_2048 ? context.buffer(std::size_t(collision_blocks * 2 + 1) * 4) : collision_map;
    collision_counts = context.buffer(std::size_t(collision_blocks) * 4);
    collision_positions = context.buffer(std::size_t(b_count) * 4);
    if (cancellation_path == collision_path::compact)
      surviving_a = context.buffer(std::size_t(a_count) * 4);
  }
  result.prepare = elapsed(phase);
  phase = clock_type::now();
  auto command = [context.queue commandBuffer];
  if (compressed_inputs)
    context.dispatch(command, "compressed_prefix", 1, arena, prefix, nil, n, desc, nil, nil,
                     a_count, b_count, 0, delta_a, delta_b, tree_base, other, nil, tree);
  if (use_prefix_cache)
    context.dispatch(command, "compressed_cache", n, arena, desc, nil, n, desc, nil, nil, a_count,
                     b_count, 0, delta_a, delta_b, tree_base, other, prefix, tree);
  if (conservative_tombstones) {
    context.dispatch(command, "tombstone_initialize", n, arena, nil, nil, n, desc, nil,
                     tombstones, a_count, b_count, 0, delta_a, delta_b, tree_base, other, prefix, tree);
    context.dispatch(command, "tombstone_redirect", b_count, arena, nil, nil, n, desc, nil,
                     tombstones, a_count, b_count, 0, delta_a, delta_b, tree_base, other, prefix, tree);
  }
  if (cancellation_path == collision_path::disabled) {
    context.dispatch(command, kernel("merge_order"), (n + 7) / 8, arena, ordered, nil, n, desc, nil,
                     nil, a_count, b_count, shared_prefix, delta_a, delta_b, tree_base, other,
                     prefix, tree);
    context.dispatch(command, kernel("merge_keep"), n, arena, keep, nil, n, desc, ordered, nil, 0,
                     0, conservative_tombstones ? unsigned(coverage == merge_coverage::complete_older_history)
                                               : shared_prefix,
                     delta_a, delta_b, tree_base, other, prefix, tree, tombstones);
    auto positions = context.scan(command, keep, n);
    context.dispatch(command, "merge_compact", n, keep, compact, count, n, positions, ordered);
    gpu::finish(command);
    result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;
    result.count = *static_cast<std::uint32_t *>(count.contents);
  } else {
    auto storage_words = collision_words + (collision_2048 ? 0 : collision_blocks * 3 + 1);
    context.dispatch(command, collision_2048 ? "collision_rank2048_clear" : "collision_clear",
                     storage_words, nil, collision_map, nil, a_count);
    bool tiled = cancellation_path == collision_path::tiled;
    context.dispatch(command, tiled ? "collision_mark_tiled" : "collision_mark",
                     tiled ? (b_count + 31) / 32 : b_count, arena, collision_map, nil, n, desc, nil,
                     collision_positions, a_count, b_count, 0, delta_a, delta_b, tree_base, other,
                     prefix, tree);
    context.dispatch(command, collision_2048 ? "rank_count" : "collision_rank512_count",
                     collision_blocks, collision_map, collision_directory, collision_counts,
                     a_count);
    auto positions = context.scan(command, collision_counts, collision_blocks);
    context.dispatch(
        command, collision_2048 ? "collision_rank2048_finish" : "collision_rank512_finish",
        std::max(1u, collision_blocks), positions, collision_directory, collision_counts, a_count);
    gpu::finish(command);
    result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;
    auto total_at = collision_2048 ? collision_blocks * 2 : collision_words + collision_blocks * 3;
    auto cancelled = static_cast<std::uint32_t const *>(collision_directory.contents)[total_at];
    require(cancelled <= std::min(a_count, b_count), "invalid collision count");
    result.count = n - cancelled;
    command = [context.queue commandBuffer];
    auto collision_kernel = [&](char const *suffix) {
      return std::string(collision_2048 ? "collision_rank2048_" : "collision_") + suffix;
    };
    auto directory = collision_2048 ? collision_directory : nil;
    if (cancellation_path == collision_path::compact) {
      context.dispatch(command, collision_kernel("compact_a"), a_count, arena, surviving_a,
                       directory, n, desc, nil, collision_map, a_count, b_count, 0, delta_a,
                       delta_b, tree_base, other, prefix, tree);
      context.dispatch(command, "collision_merge_compact", (result.count + 7) / 8, arena, compact,
                       nil, n, desc, surviving_a, nil, a_count, b_count, a_count - cancelled,
                       delta_a, delta_b, tree_base, other, prefix, tree);
    } else {
      context.dispatch(command, collision_kernel(tiled ? "scatter_a_tiled" : "scatter_a"),
                       tiled ? (a_count + 31) / 32 : a_count, arena, compact, directory, n, desc,
                       collision_positions, collision_map, a_count, b_count, 0, delta_a, delta_b,
                       tree_base, other, prefix, tree);
      context.dispatch(command, collision_kernel("scatter_b"), b_count, arena, compact, directory,
                       n, desc, collision_positions, collision_map, a_count, b_count, 0, delta_a,
                       delta_b, tree_base, other, prefix, tree);
    }
    gpu::finish(command);
    result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;
  }
  result.order = elapsed(phase);
  auto survivors = result.count;
  require(survivors <= n, "bad survivor count");
  if (!survivors) {
    require(coverage == merge_coverage::complete_older_history, "unexpected empty merge");
    // Only constant-sized empty grammar/envelope construction occurs on CPU.
    // Every input record was parsed/ordered/filtered on GPU before this point.
    phase = clock_type::now();
    everett::sort_profile_writer<policy> empty;
    auto empty_array = empty.finish();
    auto encoded = everett::encoded_sort_sections<policy>::from(empty_array).materialize();
    mapping output(path, encoded.size());
    std::memcpy(output.data, encoded.data(), encoded.size());
    require(ftruncate(output.fd, off_t(encoded.size())) == 0, "empty output extent");
    result.bytes = encoded.size();
    result.assembly = elapsed(phase);
    result.total = elapsed(start);
    return result;
  }
  if (verify_compressed_descriptors && use_prefix_cache) {
    auto common_bytes = *static_cast<std::uint32_t const *>(prefix.contents);
    auto records = static_cast<compressed_descriptor const *>(desc.contents);
    std::size_t ordinal = 0;
    for (auto source : {&a, &b}) {
      auto cursor = source->view().cursor();
      while (!cursor.done()) {
        auto key = cursor.peek().key.prefix;
        std::array<std::uint32_t, 2> expected{};
        for (unsigned i = 0; i < 8; ++i) {
          auto bit = std::uint64_t(common_bytes + i) * 8 + 1;
          auto value =
              bit < key.size() ? std::uint32_t(everett::profile_detail::load_bits(key, bit, 8)) : 0;
          expected[i / 4] = (expected[i / 4] << 8) | value;
        }
        require(records[ordinal].parent_record == expected[0] &&
                    records[ordinal].reserved == expected[1],
                "GPU discriminating prefix cache mismatch");
        ++ordinal;
        cursor.advance();
      }
    }
  }
  phase = clock_type::now();
  auto status_bytes = use_output_plan ? 20u : 4u;
  auto lengths = context.buffer(survivors * 4), frames = context.buffer(survivors * 12),
       status = context.buffer(status_bytes);
  std::memset(status.contents, 0, status_bytes);
  std::uint32_t ef_entries = (survivors + 14) / 15 + 1, sample_count = (ef_entries + 255) / 256;
  id<MTLBuffer> sparse_counts = nil, sparse_starts = nil;
  command = [context.queue commandBuffer];
  context.dispatch(command, kernel("merge_sizes"), survivors, arena, lengths, status, survivors,
                   desc, compact, frames, 0, 0, shared_prefix, delta_a, delta_b, tree_base, other,
                   prefix, tree, tombstones);
  auto offsets = context.scan(command, lengths, survivors);
  if (use_output_plan) {
    sparse_counts = context.buffer(std::size_t(sample_count) * 4);
    context.dispatch(command, "ef_output_sparse_plan", sample_count, offsets, sparse_counts,
                     status, survivors, lengths, compact, status, descriptor_words, 0, 0, 0, 0, 0,
                     desc);
    sparse_starts = context.scan(command, sparse_counts, sample_count);
  }
  gpu::finish(command);
  auto starts = static_cast<std::uint32_t const *>(offsets.contents),
       lens = static_cast<std::uint32_t const *>(lengths.contents);
  result.bits = starts[survivors - 1] + lens[survivors - 1];
  require(result.bits < (1u << 31), "prototype bit extent exceeds2Gibit");
  result.size = elapsed(phase);
  result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;
  phase = clock_type::now();
  auto selected = static_cast<std::uint32_t const *>(compact.contents);
  std::optional<std::uint64_t> common;
  if (!*static_cast<std::uint32_t *>(status.contents))
    common = static_cast<std::uint32_t const *>(desc.contents)[selected[0] * descriptor_words + 3];
  std::array<std::vector<std::byte>, 8> parts;
  std::array<std::size_t, 8> part_sizes{};
  part_sizes[0] = (result.bits + 7) / 8;
  everett::elias_fano ef;
  if (gpu_output_ef) {
    auto fixed = std::uint64_t(survivors) * common.value_or(0);
    require(fixed <= result.bits, "GPU EF common stride extent");
    ef.universe = result.bits - fixed;
    auto quotient = ef.universe / ef_entries;
    ef.low_width = quotient ? unsigned(std::bit_width(quotient) - 1) : 0;
    if (use_output_plan) {
      auto plan = static_cast<std::uint32_t const *>(status.contents);
      require(plan[1] == result.bits && plan[2] == common.value_or(0) &&
                  plan[3] == ef.universe && plan[4] == ef.low_width,
              "GPU output plan disagrees with checked extent");
    } else {
      sparse_counts = context.buffer(std::size_t(sample_count) * 4);
      auto plan = [context.queue commandBuffer];
      context.dispatch(plan, "ef_output_sparse_count", sample_count, offsets, sparse_counts, nil,
                       survivors, nil, nil, nil, result.bits, std::uint32_t(common.value_or(0)),
                       ef.low_width);
      sparse_starts = context.scan(plan, sparse_counts, sample_count);
      gpu::finish(plan);
      result.gpu_ms += (plan.GPUEndTime - plan.GPUStartTime) * 1000;
    }
    auto last = sample_count - 1;
    auto sparse_count = static_cast<std::uint32_t const *>(sparse_counts.contents)[last] +
                        static_cast<std::uint32_t const *>(sparse_starts.contents)[last];
    auto low_words = (std::uint64_t(ef_entries) * ef.low_width + 63) / 64;
    auto high_words = ((ef.universe >> ef.low_width) + ef_entries + 63) / 64;
    part_sizes[1] = std::size_t(low_words) * 8;
    part_sizes[2] = std::size_t(high_words) * 8;
    part_sizes[3] = std::size_t(sample_count) * 16;
    part_sizes[4] = std::size_t(sparse_count) * 8;
  } else {
    std::vector<std::uint64_t> boundaries;
    for (std::uint32_t i = 0; i < survivors; i += 15)
      boundaries.push_back(starts[i] - std::uint64_t(i) * common.value_or(0));
    boundaries.push_back(result.bits - std::uint64_t(survivors) * common.value_or(0));
    ef = everett::elias_fano::build<everett_experiment::architecture>(boundaries);
    everett::sort_profile_file_detail::append_words(parts[1], ef.low);
    everett::sort_profile_file_detail::append_words(parts[2], ef.high);
    parts[3].resize(ef.samples.size() * 16);
    for (std::size_t i = 0; i < ef.samples.size(); i++) {
      everett::file_detail::put(parts[3], i * 16, 8, ef.samples[i].first);
      everett::file_detail::put(parts[3], i * 16 + 8, 8, ef.samples[i].sparse);
    }
    everett::sort_profile_file_detail::append_words(parts[4], ef.sparse);

    for (unsigned i = 1; i < 5; i++)
      part_sizes[i] = parts[i].size();
  }
  parts[5].resize(1);
  std::array<std::uint64_t, 2> dictionary_offsets{0, 1};
  everett::sort_profile_file_detail::append_words(parts[6], dictionary_offsets);
  part_sizes[5] = parts[5].size();
  part_sizes[6] = parts[6].size();
  std::array<std::byte, 192> directory{};
  std::memcpy(directory.data(), "KV03", 4);
  everett::file_detail::put(directory, 4, 2, 3);
  everett::file_detail::put(directory, 6, 2, 8);
  everett::file_detail::put(directory, 8, 8, result.bits);
  everett::file_detail::put(directory, 16, 8,
                         static_cast<std::uint32_t const *>(
                             desc.contents)[selected[survivors - 1] * descriptor_words + 1] *
                                 8 +
                             1);
  everett::file_detail::put(directory, 24, 8, ef.universe);
  everett::file_detail::put(directory, 32, 8, 1);
  directory[48] = std::byte(ef.low_width);
  std::size_t end = 192;
  for (unsigned i = 0; i < 8; i++) {
    auto first = everett::section_detail::align(end);
    auto bytes = part_sizes[i];
    everett::file_detail::put(directory, 64 + i * 16, 8, first);
    everett::file_detail::put(directory, 72 + i * 16, 8, bytes);
    end = first + bytes;
  }
  result.bytes = 96 + end;
  mapping output(path, result.bytes);
  auto base = static_cast<std::byte *>(output.data);
  // O_TRUNC followed by ftruncate gives a fresh zero-filled mapping. The
  // kernels write every payload/EF word; untouched alignment gaps stay zero.
  std::memcpy(base + 96, directory.data(), 192);
  for (unsigned i = 1; i < 8; i++)
    if (!parts[i].empty())
      std::memcpy(base + 96 + everett::file_detail::get(directory, 64 + i * 16, 8), parts[i].data(),
                  parts[i].size());
  result.assembly = elapsed(phase);
  phase = clock_type::now();
  auto target = [context.device newBufferWithBytesNoCopy:output.data
                                                  length:output.length
                                                 options:MTLResourceStorageModeShared
                                             deallocator:nil];
  require(target != nil, "packed output mmap import");
  command = [context.queue commandBuffer];
  context.dispatch(command, conservative_tombstones ? "tombstone_merge_emit_words" :
                   use_word_emitter ? "compressed_merge_emit_words" : kernel("merge_emit"),
                   (result.bits + 31) / 32, arena, target, offsets, survivors, desc, compact,
                   frames, result.bits, 72, 0, delta_a, delta_b, tree_base, other, prefix, tree, tombstones);
  if (gpu_output_ef) {
    for (auto [entry, section, threads] :
         {std::tuple{"ef_output_low", 1u, std::uint32_t(part_sizes[1] / 4)},
          std::tuple{"ef_output_high", 2u, std::uint32_t(part_sizes[2] / 4)},
          std::tuple{"ef_output_samples", 3u, sample_count},
          std::tuple{"ef_output_sparse", 4u, ef_entries}}) {
      auto destination =
          std::uint32_t((96 + everett::file_detail::get(directory, 64 + section * 16, 8)) / 4);
      context.dispatch(command, entry, threads, offsets, target, nil, survivors, sparse_counts,
                       sparse_starts, nil, result.bits, std::uint32_t(common.value_or(0)),
                       ef.low_width, destination);
    }
  }
  gpu::finish(command);
  result.emit = elapsed(phase);
  result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;
  phase = clock_type::now();
  everett::file_header<policy> header;
  header.record_count = survivors;
  header.common_value_width = common;
  header.extent = end * 8;
  auto encoded =
      everett::encode_file_header(header, everett::crc32c<everett_experiment::architecture>(std::span<std::byte const>(base + 96, end)));
  std::memcpy(base, encoded.data(), 96);
  require(ftruncate(output.fd, off_t(result.bytes)) == 0, "final output extent");
  result.assembly += elapsed(phase);
  result.total = elapsed(start);
  return result;
}
void merge_case(gpu &context, std::filesystem::path const &directory, std::uint32_t n,
                unsigned prefix, unsigned duplicates, unsigned ratio, unsigned trials) {
  auto a = make_input(directory / "left.kv", n, 0, prefix, duplicates),
       b = make_input(directory / "right.kv", n / ratio, 1, prefix, duplicates);
  for (unsigned trial = 0; trial < trials; trial++) {
    std::vector<std::byte> encoded;
    double cpu_merge_ms = 0, cpu_all_ms = 0;
    std::uint64_t cpu_count = 0;
    auto cpu_run = [&] {
      auto begin = clock_type::now();
      everett::sort_profile_merge_builder<policy, native> cpu(a, b);
      cpu.step(std::numeric_limits<std::uint64_t>::max());
      auto merged = cpu.finish();
      cpu_merge_ms = elapsed(begin);
      cpu_count = merged.size();
      encoded = everett::encoded_sort_sections<policy>::from(merged).materialize();
      mapping output(directory / "cpu.kv", encoded.size());
      std::memcpy(output.data, encoded.data(), encoded.size());
      require(ftruncate(output.fd, off_t(encoded.size())) == 0, "CPU output extent");
      cpu_all_ms = elapsed(begin);
    };
    merge_result result;
    if (trial % 2) {
      result = gpu_merge(context, *a, *b, directory / "gpu.kv");
      cpu_run();
    } else {
      cpu_run();
      result = gpu_merge(context, *a, *b, directory / "gpu.kv");
    }

    auto opened = native::open(everett::file<policy>::open(result.path));
    opened.scan();
    require(opened.size() == cpu_count, "merge count mismatch");
    std::ifstream stream(result.path, std::ios::binary);
    std::vector<char> actual((std::istreambuf_iterator<char>(stream)), {});
    require(actual.size() == encoded.size() &&
                std::memcmp(actual.data(), encoded.data(), encoded.size()) == 0,
            "GPU canonical KV03 differs from CPU encoder");
    std::cout << "merge," << n << ',' << prefix << ',' << duplicates << ',' << ratio << ',' << trial
              << ',' << cpu_merge_ms << ',' << cpu_all_ms << ',' << result.total << ','
              << result.decode << ',' << result.prepare << ',' << result.order << ',' << result.size
              << ',' << result.emit << ',' << result.assembly << ',' << result.gpu_ms << ','
              << result.count << ',' << result.bits << ',' << result.bytes << '\n';
  }
}

#include "adversarial.h"
#include "tombstone_test.h"
#include "cutover_cases.h"
#include "gpu_sort.h"
#include "index_rank_test.h"
#include "rank_shared.h"
int main(int argc, char **argv) {
  @autoreleasepool {
    try {
      require(argc >= 3, "usage: prototype kernels.metallib output-directory [quick]");
      std::filesystem::create_directories(argv[2]);
      gpu context(argv[1]);
      probe(context, argv[2]);
      for (auto name : {"scan_blocks", "scan_add", "rank_count", "rank_finish", "rank15_count",
                        "rank15_finish"})
        (void)context.pipeline(name);
      std::string mode = argc > 3 ? argv[3] : "rank";
      if (auto tile_option = mode.find("-tiles"); tile_option != std::string::npos) {
        use_prefix_tiles = true;
        mode.erase(tile_option, 6);
      }
      if (auto plan_option = mode.find("-plan"); plan_option != std::string::npos) {
        use_output_plan = true;
        mode.erase(plan_option, 5);
      }
      auto quick = argc > 3;
      if (mode == "rank-shared") {
        (void)context.pipeline("rank15_classes");
        std::cout << "kind,format,bits,trial,cpu_owning_ms,cpu_reused_ms,gpu_"
                     "reused_ms,gpu_command_ms\n";
        for (auto bits : {1u, 15u, 31u, 1921u, 2049u, 65537u})
          rank_shared_case(context, argv[2], bits, 1);
        for (auto bits : {1u << 20, 1u << 24, 1u << 28})
          rank_shared_case(context, argv[2], bits, 5);
        return 0;
      }
      bool collision_mode = mode.starts_with("collision512") || mode.starts_with("collision2048");
      bool merge_mode = collision_mode || mode == "merge" || mode == "merge-bench" ||
                        mode == "compressed" || mode == "compressed-bench" || mode == "complete" ||
                        mode == "complete-bench" || mode == "cached" || mode == "cached-bench" ||
                        mode == "optimized" || mode == "optimized-bench" || mode == "cutover" ||
                        mode == "tombstone";
      if (merge_mode) {
        conservative_tombstones = mode == "tombstone";
        if (collision_mode) {
          collision_2048 = mode.starts_with("collision2048");
          cancellation_path = mode.find("-tiled") != std::string::npos     ? collision_path::tiled
                              : mode.find("-compact") != std::string::npos ? collision_path::compact
                                                                           : collision_path::direct;
        }
        use_prefix_cache = collision_mode || mode.starts_with("cached") ||
                           mode.starts_with("optimized") || mode == "cutover" || conservative_tombstones;
        use_word_emitter = collision_mode || mode.starts_with("optimized") || mode == "cutover" ||
                           conservative_tombstones;
        gpu_output_ef = use_prefix_cache || mode.starts_with("complete");
        compressed_inputs = gpu_output_ef || mode.starts_with("compressed");
        require(!use_output_plan || gpu_output_ef, "output-plan mode requires GPU output EF");
        verify_compressed_descriptors =
            compressed_inputs && !mode.ends_with("-bench") && mode != "cutover";
        if (gpu_output_ef)
          for (auto name : {"ef_output_sparse_count", "ef_output_low", "ef_output_high",
                            "ef_output_samples", "ef_output_sparse"})
            (void)context.pipeline(name);
        if (compressed_inputs)
          for (auto name : {"parse_input", "prefix_leaf", "prefix_reduce", "prefix_leaf_tiles",
                            "prefix_reduce_tiles", "compressed_prefix",
                            "compressed_probe", "compressed_merge_order", "compressed_merge_keep",
                            "compressed_merge_sizes", "compressed_merge_emit"})
            (void)context.pipeline(name);
        if (use_output_plan)
          (void)context.pipeline("ef_output_sparse_plan");
        if (use_prefix_cache)
          for (auto name : {"compressed_cache", "cached_merge_order", "cached_merge_keep",
                            "cached_merge_sizes"})
            (void)context.pipeline(name);
        if (use_word_emitter)
          (void)context.pipeline("compressed_merge_emit_words");
        if (conservative_tombstones) {
          for (auto name : {"tombstone_initialize", "tombstone_redirect", "tombstone_merge_keep",
                            "tombstone_merge_sizes", "tombstone_merge_emit_words"})
            (void)context.pipeline(name);
          gpu_tombstone_test::run(context, argv[2]);
          return 0;
        }
        for (auto name : {"merge_order", "merge_keep", "merge_compact", "merge_sizes", "merge_emit",
                          "scan_blocks", "scan_add"})
          (void)context.pipeline(name);
        if (collision_mode)
          for (auto name :
               {"collision_clear", "collision_mark", "collision_mark_tiled",
                "collision_rank512_count", "collision_rank512_finish", "collision_rank2048_finish",
                "collision_rank_probe", "collision_rank2048_probe", "collision_scatter_a",
                "collision_scatter_a_tiled", "collision_scatter_b", "collision_compact_a",
                "collision_merge_compact", "collision_rank2048_clear",
                "collision_rank2048_scatter_a", "collision_rank2048_scatter_a_tiled",
                "collision_rank2048_scatter_b", "collision_rank2048_compact_a"})
            (void)context.pipeline(name);
        if (mode == "cutover") {
          require(argc >= 5 && argc <= 6, "cutover requires case index and optional trial count");
          auto index = std::stoul(argv[4]);
          auto trials = argc == 6 ? std::stoul(argv[5]) : 3;
          require(index < everett_gpu_cutover::cases().size() && trials > 0 && trials <= 64,
                  "cutover case/trial bounds");
          run_cutover(context, argv[2], unsigned(index), unsigned(trials));
          return 0;
        }
        std::cout << "kind,n,prefix,duplicates,ratio,trial,cpu_merge_ms,cpu_"
                     "all_ms,gpu_all_ms,"
                     "decode_ms,prepare_ms,order_ms,size_ms,emit_ms,assembly_"
                     "ms,gpu_command_ms,"
                     "survivors,bits,bytes\n";
        if (!mode.ends_with("-bench")) {
          if (collision_mode) {
            collision_rank_adversarial(context);
            collision_rank2048_adversarial(context);
          }
          if (use_word_emitter) {
            check_word_emit(context);
            index_rank_adversarial(context);
          }
          if (use_prefix_tiles)
            prefix_tree_adversarial(context);
          if (use_output_plan)
            output_plan_adversarial(context);
          ef_input_test(context);
          if (gpu_output_ef)
            ef_output_adversarial(context);
          adversarial(context, argv[2]);
          for (auto n : {1u, 7u, 16u, 129u, 4096u})
            merge_case(context, argv[2], n, 32, 50, 1, 1);
        } else {
          merge_case(context, argv[2], 4096, 32, 50, 1, 1);
          for (auto n : {16384u, 65536u, 262144u})
            merge_case(context, argv[2], n, 32, 50, 1, 3);
          for (auto prefix : {0u, 256u})
            merge_case(context, argv[2], 65536, prefix, 50, 1, 3);
          merge_case(context, argv[2], 65536, 32, 0, 16, 3);
          merge_case(context, argv[2], 65536, 32, 100, 1, 3);
        }
        return 0;
      }
      std::cout << "kind,format,bits,trial,cpu_ms,gpu_end_to_end_ms,gpu_"
                   "command_ms,count\n";
      for (auto bits : {1u, 15u, 31u, 1921u, 2049u, 65537u})
        rank_case(context, bits, 1);
      if (!quick)
        for (auto bits : {1u << 20, 1u << 24, 1u << 28})
          rank_case(context, bits, 5);
    } catch (std::exception const &error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
}
