/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Provides the optional byte-merge driver's Metal buffers and scratch mappings.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>

inline void require(bool value, char const * message) {
  if (!value) throw std::runtime_error(message);
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
struct mapping {
  int fd = -1;
  std::size_t length = 0;
  void *data = MAP_FAILED;
  explicit mapping(std::filesystem::path const &path, std::size_t bytes) {
    auto page = std::size_t(sysconf(_SC_PAGESIZE));
    length = (bytes + page - 1) / page * page;
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "open scratch");
    try {
      if (ftruncate(fd, off_t(length)) != 0)
        throw std::system_error(errno, std::generic_category(), "truncate scratch");
      data = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (data == MAP_FAILED)
        throw std::system_error(errno, std::generic_category(), "map scratch");
    } catch (...) {
      close(fd); fd = -1;
      throw;
    }
  }
  mapping(mapping const &) = delete;
  mapping & operator=(mapping const &) = delete;
  ~mapping() {
    if (data != MAP_FAILED)
      munmap(data, length);
    if (fd >= 0)
      close(fd);
  }
};
