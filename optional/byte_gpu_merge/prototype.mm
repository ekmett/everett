/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Merges actual byte-profile files on Metal and checks complete CPU files.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include "metal.h"
#include "prepared_input.h"
#include <everett/native_merge.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <pthread.h>
#include <pthread/qos.h>
#include <random>
#include <set>

using namespace everett_byte_gpu;
using clock_type = std::chrono::steady_clock;
static double elapsed(clock_type::time_point start) {
  return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
}

struct imported_payload {
  id<MTLBuffer> buffer;
  std::uint32_t delta;
  std::uintptr_t base;
  imported_payload(gpu & context, compressed_blocks const & input) {
    auto first = UINTPTR_MAX, last = std::uintptr_t{0};
    auto include = [&](std::span<std::byte const> section) {
      if (section.empty()) return;
      auto at = reinterpret_cast<std::uintptr_t>(section.data());
      first = std::min(first, at); last = std::max(last, at + section.size());
    };
    include(input.payload);
    for (auto part : input.ef_sections) include(part);
    require(last > first, "source mapping sections absent");
    auto page = std::size_t(sysconf(_SC_PAGESIZE));
    base = first / page * page;
    auto length = (last - base + page - 1) / page * page;
    auto payload = reinterpret_cast<std::uintptr_t>(input.payload.data());
    require(payload >= base && payload - base <= UINT32_MAX, "source payload delta");
    delta = std::uint32_t(payload - base);
    buffer = [context.device newBufferWithBytesNoCopy:reinterpret_cast<void *>(base)
      length:length options:MTLResourceStorageModeShared deallocator:nil];
    require(buffer != nil, "input mmap import rejected");
  }
};

static void write_bytes(std::filesystem::path const & path, std::span<std::byte const> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
  require(bool(stream), "fixture write failed");
}
static std::vector<std::byte> read_bytes(std::filesystem::path const & path) {
  std::vector<std::byte> bytes(std::filesystem::file_size(path));
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
  require(bool(stream), "result read failed");
  return bytes;
}
static void mapped_write(std::filesystem::path const & path, std::span<std::byte const> bytes) {
  mapping target(path, bytes.size());
  std::memcpy(target.data, bytes.data(), bytes.size());
  require(ftruncate(target.fd, off_t(bytes.size())) == 0, "output clip failed");
}

struct merge_result {
  double parse = 0, order = 0, size = 0, emit = 0, assembly = 0, gpu_ms = 0, total = 0;
  std::uint32_t count = 0, payload_bytes = 0;
  std::optional<std::uint64_t> common;
  std::size_t bytes = 0;
};

static merge_result merge_impl(gpu & context, native const & a, native const & b,
    std::filesystem::path const & path) {
  merge_result result;
  compressed_blocks left(a), right(b);
  auto total = std::uint64_t(left.count) + right.count;
  require(total < (1u << 24), "GPU merged record bound");
  auto n = std::uint32_t(total);
  if (!n) {
    auto start = clock_type::now();
    auto array = everett::profile_array<policy>::build({});
    auto encoded = everett::encode_native_sections(array).materialize();
    mapped_write(path, encoded);
    result.common = 0; result.bytes = encoded.size(); result.assembly = elapsed(start);
    return result;
  }
  auto phase = clock_type::now();
  imported_payload left_map(context, left), right_map(context, right);
  auto arena = left_map.buffer, other = right_map.buffer;
  auto delta_a = left_map.delta, delta_b = right_map.delta;
  auto tree_base = std::bit_ceil(n);
  auto desc = context.buffer(std::size_t(n) * sizeof(descriptor));
  auto tree = context.buffer(std::size_t(tree_base) * 8), prefix = context.buffer(4);
  auto status = context.buffer(8); std::memset(status.contents, 0, 8);
  auto left_packet = left.ef_parameters(left_map.base), right_packet = right.ef_parameters(right_map.base);
  auto left_ef = context.buffer(sizeof(left_packet), left_packet.data());
  auto right_ef = context.buffer(sizeof(right_packet), right_packet.data());
  auto command = [context.queue commandBuffer];
  context.dispatch(command, "byte_parse_input", left.block_count, arena, desc, status, left.count,
    nil, left_ef, nil, 0, 0, left.block_count, delta_a, left.extent);
  context.dispatch(command, "byte_parse_input", right.block_count, other, desc, status, right.count,
    nil, right_ef, nil, left.count, 1, right.block_count, delta_b, right.extent);
  context.build_prefix_tree(command, tree, status, desc, n, left.count, false);
  gpu::finish(command);
  auto flags = static_cast<std::uint32_t const *>(status.contents);
  require(!flags[0], "GPU byte input framing/retention validation failed");
  require(std::uint64_t(flags[1]) * n < (1u << 28), "GPU worst-case output exceeds bound");
  auto records = static_cast<descriptor const *>(desc.contents);
  if (left.count) require(records[left.count - 1].key_bytes * 8 == left.terminal_key_bits, "left terminal key");
  if (right.count) require(records[n - 1].key_bytes * 8 == right.terminal_key_bits, "right terminal key");
  result.parse = elapsed(phase);
  result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;

  phase = clock_type::now();
  auto ordered = context.buffer(std::size_t(n) * 4), keep = context.buffer(std::size_t(n) * 4);
  auto compact = context.buffer(std::size_t(n) * 4), count = context.buffer(4);
  command = [context.queue commandBuffer];
  context.dispatch(command, "compressed_prefix", 1, arena, prefix, nil, n, desc, nil, nil,
    left.count, right.count, 0, delta_a, delta_b, tree_base, other, nil, tree);
  context.dispatch(command, "compressed_cache", n, arena, desc, nil, n, desc, nil, nil,
    left.count, right.count, 0, delta_a, delta_b, tree_base, other, prefix, tree);
  context.dispatch(command, "merge_order", (n + 7) / 8, arena, ordered, nil, n, desc, nil, nil,
    left.count, right.count, 0, delta_a, delta_b, tree_base, other, prefix, tree);
  context.dispatch(command, "merge_keep", n, arena, keep, nil, n, desc, ordered, nil,
    0, 0, 0, delta_a, delta_b, tree_base, other, prefix, tree);
  auto positions = context.scan(command, keep, n);
  context.dispatch(command, "merge_compact", n, keep, compact, count, n, positions, ordered);
  gpu::finish(command);
  result.count = *static_cast<std::uint32_t const *>(count.contents);
  require(result.count && result.count <= n, "GPU survivor count invalid");
  result.order = elapsed(phase);
  result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;

  auto survivors = result.count;
  phase = clock_type::now();
  auto lengths = context.buffer(std::size_t(survivors) * 4);
  auto frames = context.buffer(std::size_t(survivors) * 12);
  auto width_status = context.buffer(4); std::memset(width_status.contents, 0, 4);
  command = [context.queue commandBuffer];
  context.dispatch(command, "byte_value_width", survivors, nil, nil, width_status, survivors, desc, compact);
  context.dispatch(command, "byte_merge_sizes", survivors, arena, lengths, width_status, survivors,
    desc, compact, frames, 0, 0, 0, delta_a, delta_b, tree_base, other, prefix, tree);
  auto offsets = context.scan(command, lengths, survivors);
  gpu::finish(command);
  auto starts = static_cast<std::uint32_t const *>(offsets.contents);
  auto lens = static_cast<std::uint32_t const *>(lengths.contents);
  auto selected = static_cast<std::uint32_t const *>(compact.contents);
  result.payload_bytes = starts[survivors - 1] + lens[survivors - 1];
  require(result.payload_bytes < (1u << 28), "GPU output payload bound");
  if (!*static_cast<std::uint32_t const *>(width_status.contents))
    result.common = records[selected[0]].value_bits >> 3;
  result.size = elapsed(phase);
  result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;

  phase = clock_type::now();
  auto fixed = std::uint64_t(survivors) * result.common.value_or(0);
  require(fixed <= result.payload_bytes, "GPU output fixed stride extent");
  auto universe = std::uint32_t(result.payload_bytes - fixed);
  auto ef_entries = (survivors + 14) / 15 + 1, sample_count = (ef_entries + 255) / 256;
  auto quotient = universe / ef_entries;
  auto low_width = quotient ? std::bit_width(quotient) - 1 : 0;
  auto sparse_counts = context.buffer(std::size_t(sample_count) * 4);
  command = [context.queue commandBuffer];
  context.dispatch(command, "ef_output_sparse_count", sample_count, offsets, sparse_counts, nil,
    survivors, nil, nil, nil, result.payload_bytes, std::uint32_t(result.common.value_or(0)), low_width);
  auto sparse_starts = context.scan(command, sparse_counts, sample_count);
  gpu::finish(command);
  result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;
  auto last = sample_count - 1;
  auto sparse_count = static_cast<std::uint32_t const *>(sparse_counts.contents)[last] +
    static_cast<std::uint32_t const *>(sparse_starts.contents)[last];
  std::array<std::size_t, 5> sizes{result.payload_bytes,
    std::size_t((std::uint64_t(ef_entries) * low_width + 63) >> 6) * 8,
    std::size_t(((universe >> low_width) + ef_entries + 63) >> 6) * 8,
    std::size_t(sample_count) * 16, std::size_t(sparse_count) * 8};
  std::array<std::byte, 128> directory{};
  std::memcpy(directory.data(), "KV02", 4);
  everett::file_detail::put(directory, 4, 2, 2);
  everett::file_detail::put(directory, 6, 2, 5);
  everett::file_detail::put(directory, 8, 8, result.payload_bytes);
  everett::file_detail::put(directory, 16, 8, records[selected[survivors - 1]].key_bytes);
  everett::file_detail::put(directory, 24, 8, universe);
  directory[32] = std::byte(low_width);
  std::size_t end = directory.size();
  for (unsigned i = 0; i != sizes.size(); ++i) {
    auto first = everett::section_detail::align(end);
    everett::file_detail::put(directory, 48 + i * 16, 8, first);
    everett::file_detail::put(directory, 56 + i * 16, 8, sizes[i]);
    end = first + sizes[i];
  }
  result.bytes = 96 + end;
  mapping output(path, result.bytes);
  auto base = static_cast<std::byte *>(output.data);
  std::memcpy(base + 96, directory.data(), directory.size());
  auto target = [context.device newBufferWithBytesNoCopy:output.data length:output.length
    options:MTLResourceStorageModeShared deallocator:nil];
  require(target != nil, "output mmap import rejected");
  result.assembly = elapsed(phase);
  phase = clock_type::now();
  command = [context.queue commandBuffer];
  context.dispatch(command, "byte_merge_emit_words", (result.payload_bytes + 3) / 4,
    arena, target, offsets, survivors, desc, compact, frames,
    result.payload_bytes, (96 + 128) / 4, std::uint32_t(result.common.value_or(0)),
    delta_a, delta_b, tree_base, other, prefix, tree);
  for (auto [entry, section, threads] : {
      std::tuple{"ef_output_low", 1u, std::uint32_t(sizes[1] / 4)},
      std::tuple{"ef_output_high", 2u, std::uint32_t(sizes[2] / 4)},
      std::tuple{"ef_output_samples", 3u, sample_count},
      std::tuple{"ef_output_sparse", 4u, ef_entries}}) {
    auto destination = std::uint32_t((96 + everett::file_detail::get(directory, 48 + section * 16, 8)) / 4);
    context.dispatch(command, entry, threads, offsets, target, nil, survivors, sparse_counts,
      sparse_starts, nil, result.payload_bytes, std::uint32_t(result.common.value_or(0)), low_width, destination);
  }
  gpu::finish(command);
  result.emit = elapsed(phase);
  result.gpu_ms += (command.GPUEndTime - command.GPUStartTime) * 1000;
  phase = clock_type::now();
  everett::file_header<policy> header;
  header.record_count = survivors; header.common_value_width = result.common; header.extent = end;
  auto encoded = everett::encode_file_header(header,
    everett::crc32c<everett_experiment::architecture>(std::span<std::byte const>(base + 96, end)));
  std::memcpy(base, encoded.data(), encoded.size());
  require(ftruncate(output.fd, off_t(result.bytes)) == 0, "output clip failed");
  result.assembly += elapsed(phase);
  return result;
}
static merge_result gpu_merge(gpu & context, native const & a, native const & b,
    std::filesystem::path const & path) {
  auto start = clock_type::now();
  merge_result result;
  @autoreleasepool { result = merge_impl(context, a, b, path); }
  result.total = elapsed(start); // Includes scratch destruction and unmapping.
  return result;
}

struct row {
  std::string key;
  std::optional<std::string> value;
  bool operator==(row const &) const = default;
};
using rows = std::vector<row>;
static std::vector<everett::profile_record> encode_rows(rows const & input) {
  std::vector<everett::profile_record> records;
  for (auto const & row : input) {
    std::string value(1, row.value ? '\1' : '\0');
    if (row.value) value += *row.value;
    records.push_back({everett::bit_string::from_bytes(row.key), everett::bit_string::from_bytes(value)});
  }
  return records;
}
static rows merged(rows const & a, rows const & b) {
  std::map<std::string, std::optional<std::string>> values;
  for (auto const & source : {&a, &b})
    for (auto const & r : *source) values[r.key] = r.value;
  rows result;
  for (auto const & [key, value] : values) result.push_back({key, value});
  return result;
}
static std::shared_ptr<native const> make_input(std::filesystem::path const & path,
    rows const & input, bool force_variable = false, unsigned conservative = 0) {
  auto records = encode_rows(input);
  if (force_variable) {
    everett::profile_native_writer<policy> writer;
    for (auto const & r : records) writer.append(r.key.view(), r.value.view());
    auto array = writer.finish();
    write_bytes(path, everett::encode_native_sections(array).materialize());
  } else {
    if (conservative)
      for (std::size_t i = 1; i != records.size(); ++i)
        if (i % conservative == 0) records[i].retained_limit_bits = 0;
    auto array = everett::profile_array<policy>::build(records);
    write_bytes(path, everett::encode_native_sections(array).materialize());
  }
  auto result = std::make_shared<native const>(native::open(path));
  result->scan();
  return result;
}
static void check_rows(native const & source, rows const & expected) {
  auto cursor = source.view().cursor();
  std::size_t i = 0;
  while (!cursor.done()) {
    require(i < expected.size(), "too many decoded rows");
    auto item = cursor.peek();
    auto key = item.key.prefix;
    auto const & r = expected[i++];
    require(key.size() == r.key.size() * 8 && (!key.size() ||
      std::memcmp(key.storage().data(), r.key.data(), r.key.size()) == 0), "decoded key mismatch");
    auto value = everett::bit_string::copy(item.value);
    require(value.bit_size >= 8, "decoded optional tag absent");
    require(value.bytes[0] == std::byte(r.value ? 1 : 0), "decoded optional tag mismatch");
    require(value.bytes.size() == 1 + (r.value ? r.value->size() : 0), "decoded value width mismatch");
    if (r.value && !r.value->empty())
      require(std::memcmp(value.bytes.data() + 1, r.value->data(), r.value->size()) == 0, "decoded value mismatch");
    cursor.advance();
  }
  require(i == expected.size(), "missing decoded rows");
}

// Width discovery sink for the CPU comparison when headers alone cannot prove
// a common output width. This runs a genuine encoded merge, but stores no keys
// or payloads; its complete cost is inside the CPU timer.
struct width_sink {
  using policy_type = policy;
  std::uint64_t count = 0;
  std::optional<std::uint64_t> common = 0;
  bool closed = false;
  std::uint64_t size() const noexcept { return count; }
  bool finished() const noexcept { return closed; }
  bool failed() const noexcept { return false; }
  std::optional<std::uint64_t> common_value_width() const noexcept { return std::nullopt; }
  void append(std::uint64_t, everett::bit_view, everett::bit_view value) {
    auto bytes = value.size() >> 3;
    if (!count) common = bytes;
    else if (common && *common != bytes) common.reset();
    ++count;
  }
  std::optional<std::uint64_t> finish() { closed = true; return common; }
};
struct cpu_result { double total; bool width_pass; std::size_t bytes; };
static cpu_result cpu_merge(std::shared_ptr<native const> const & a,
    std::shared_ptr<native const> const & b, std::filesystem::path const & path, bool infer_output) {
  auto start = clock_type::now();
  cpu_result result{};
  {
    std::optional<std::uint64_t> common;
    auto ca = a->view().metadata().common_value_width;
    auto cb = b->view().metadata().common_value_width;
    if (!a->size() && !b->size()) common = 0;
    else if (!a->size() && cb) common = cb;
    else if (!b->size() && ca) common = ca;
    else if (ca && cb && ca == cb) common = ca;
    else if (infer_output) {
      result.width_pass = true;
      everett::native_merge_builder<policy, native, everett::replace_native_value, width_sink>
        discovery(width_sink{}, a, b);
      discovery.step(UINT64_MAX);
      common = discovery.finish();
    }
    everett::native_merge_builder<policy, native> merge(a, b, {}, common);
    merge.step(UINT64_MAX);
    auto array = merge.finish();
    auto sections = everett::encode_native_sections(array);
    auto body_bytes = sections.header().extent;
    result.bytes = 96 + body_bytes;
    mapping output(path, result.bytes);
    auto data = static_cast<std::byte *>(output.data), cursor = data + 96;
    for (auto chunk : sections.chunks()) {
      if (!chunk.empty()) std::memcpy(cursor, chunk.data(), chunk.size());
      cursor += chunk.size();
    }
    auto header = everett::encode_file_header(sections.header(),
      everett::crc32c<everett_experiment::architecture>(std::span<std::byte const>(data + 96, body_bytes)));
    std::memcpy(data, header.data(), header.size());
    require(ftruncate(output.fd, off_t(result.bytes)) == 0, "CPU output clip");
  }
  result.total = elapsed(start);
  return result;
}

struct fixture { std::string name; rows a, b; bool variable_inputs = false; unsigned conservative = 0; };
static fixture generated(std::string name, unsigned count, unsigned other,
    unsigned prefix, unsigned duplicates, bool fixed, bool tombstones) {
  fixture f{std::move(name), {}, {}};
  for (unsigned side = 0; side != 2; ++side) {
    auto & target = side ? f.b : f.a;
    for (unsigned i = 0; i != (side ? other : count); ++i) {
      auto id = std::uint64_t(i) * 4 + (side && i % 100 >= duplicates ? 1 : 0);
      std::string key(prefix, 'p');
      if (prefix > 4) key[prefix / 2] = 0;
      for (int byte = 7; byte >= 0; --byte) key.push_back(char(id >> (byte * 8)));
      std::optional<std::string> value;
      if (!tombstones || (i + side) % 11)
        value = std::string(fixed ? 16 : (i * 37 + side) % 513, char(i + side));
      target.push_back({std::move(key), std::move(value)});
    }
  }
  return f;
}
static std::vector<fixture> correctness_cases() {
  std::vector<fixture> result;
  result.push_back({"empty", {}, {}});
  result.push_back({"sole-empty-key", {{"", std::string("")}}, {{"", std::nullopt}}});
  result.push_back({"all-width-one", {{"", std::nullopt}, {"a", std::string("")}},
    {{"", std::string("")}, {"a", std::nullopt}}});
  for (unsigned n : {1u, 14u, 15u, 16u, 29u, 30u, 31u, 127u, 256u}) {
    result.push_back(generated("fixed-" + std::to_string(n), n, n, 32, 33, true, false));
    result.push_back(generated("mixed-" + std::to_string(n), n, n / 3, 0, 75, false, true));
  }
  result.push_back(generated("left-only", 37, 0, 11, 0, false, true));
  result.push_back(generated("right-only", 0, 37, 11, 0, true, false));
  result.push_back(generated("equal-newer", 257, 257, 9, 100, false, true));
  result.push_back(generated("long-prefix", 97, 71, 4096, 60, false, true));
  result.push_back(generated("conservative-long-prefix", 97, 71, 4096, 60, false, true));
  result.back().conservative = 7;
  result.push_back(generated("common-from-variable-input", 43, 43, 5, 100, true, false));
  result.back().variable_inputs = true;
  result.push_back(generated("variable-single-source-fixed", 0, 19, 4, 100, true, false));
  result.back().variable_inputs = true;
  result.push_back(generated("fixed-to-tombstones", 33, 33, 4, 100, true, false));
  for (auto & r : result.back().b) r.value.reset();
  result.push_back(generated("asymmetric-older", 4097, 7, 16, 66, false, true));
  result.push_back(generated("asymmetric-newer", 3, 4097, 16, 66, true, false));
  fixture prefixes{"proper-prefix", {}, {}};
  std::vector<std::string> keys{"", std::string(1, 0), std::string(2, 0), std::string("\0\xff", 2),
    "a", std::string("a\0", 2), "aa", std::string("a\xff", 2), std::string(1, char(128)), std::string(1, char(255))};
  for (std::size_t i = 0; i != keys.size(); ++i) {
    prefixes.a.push_back({keys[i], i % 3 ? std::optional<std::string>("") : std::nullopt});
    if (i % 2) prefixes.b.push_back({keys[i], i % 3 ? std::nullopt : std::optional<std::string>("updated")});
  }
  result.push_back(std::move(prefixes));
  for (unsigned seed = 0; seed != 8; ++seed) {
    std::mt19937_64 random(seed + 192);
    fixture f{"random-" + std::to_string(seed), {}, {}};
    for (unsigned side = 0; side != 2; ++side) {
      std::map<std::string, std::optional<std::string>> records;
      for (unsigned i = 0; i != 257; ++i) {
        auto n = random() % 48;
        std::string key(n, '\0');
        for (auto & c : key) c = char(random());
        records[key] = random() % 5 ? std::optional<std::string>(std::string(random() % 260, char(random()))) : std::nullopt;
      }
      auto & target = side ? f.b : f.a;
      for (auto const & [key, value] : records) target.push_back({key, value});
    }
    result.push_back(std::move(f));
  }
  return result;
}

static void run_case(gpu & context, std::filesystem::path const & root, fixture const & f,
    unsigned trials, bool timing) {
  auto directory = root / f.name;
  std::filesystem::create_directories(directory);
  auto a = make_input(directory / "older.kv", f.a, f.variable_inputs, f.conservative);
  auto b = make_input(directory / "newer.kv", f.b, f.variable_inputs, f.conservative + (f.conservative != 0));
  auto expected = merged(f.a, f.b);
  auto records = encode_rows(expected);
  auto array = everett::profile_array<policy>::build(records);
  auto oracle = everett::encode_native_sections(array).materialize();
  write_bytes(directory / "oracle.kv", oracle);
  for (int trial = timing ? -1 : 0; trial < int(trials); ++trial) {
    merge_result result;
    cpu_result cpu{};
    if (trial & 1) {
      result = gpu_merge(context, *a, *b, directory / "gpu.kv");
      cpu = cpu_merge(a, b, directory / "cpu.kv", !timing);
    } else {
      cpu = cpu_merge(a, b, directory / "cpu.kv", !timing);
      result = gpu_merge(context, *a, *b, directory / "gpu.kv");
    }
    require(read_bytes(directory / "gpu.kv") == oracle, "complete GPU file differs from CPU batch writer");
    require(read_bytes(directory / "cpu.kv") == oracle, "complete CPU merge file differs from batch writer");
    auto checked = native::open(directory / "gpu.kv"); checked.scan(); check_rows(checked, expected);
    require(result.count == expected.size(), "GPU output record count differs");
    if (timing)
      std::cout << f.name << ',' << trial << ',' << f.a.size() << ',' << f.b.size() << ',' << expected.size()
        << ',' << std::filesystem::file_size(directory / "older.kv") << ',' << std::filesystem::file_size(directory / "newer.kv")
        << ',' << result.bytes << ',' << result.common.value_or(0) << ',' << cpu.width_pass << ',' << cpu.total << ','
        << result.total << ',' << result.gpu_ms << ',' << result.parse << ',' << result.order << ',' << result.size << ','
        << result.emit << ',' << result.assembly << '\n';
  }
  if (!timing) std::cout << "check," << f.name << ",passed," << expected.size() << ',' << oracle.size() << '\n';
}
static void malformed_cases(gpu & context, std::filesystem::path const & root) {
  auto source = make_input(root / "valid-malformed-base.kv", {{"a", std::nullopt}});
  auto empty = make_input(root / "empty-malformed-peer.kv", {});
  auto original = read_bytes(root / "valid-malformed-base.kv");
  auto payload = 96 + source->layout().sections[0].offset;
  auto value = payload + (source->view().encoded_at(0).value.offset() >> 3);
  auto sample = 96 + source->layout().sections[3].offset;
  for (unsigned mode = 0; mode != 4; ++mode) {
    auto bytes = original;
    if (mode == 0) { bytes[payload] = std::byte{128}; bytes[payload + 1] = std::byte{0}; }
    if (mode == 1) bytes[value] = std::byte{2};
    if (mode == 2) bytes[payload + 1] = std::byte{127};
    if (mode == 3) everett::file_detail::put(bytes, sample, 8, UINT32_MAX);
    // Keep the container checksum coherent: rejection is parser/EF validation,
    // not a deliberately stale CRC caught by a full CPU recovery scan.
    auto header = everett::file<policy>::open(root / "valid-malformed-base.kv").header();
    auto encoded = everett::encode_file_header(header,
      everett::crc32c<everett_experiment::architecture>(std::span<std::byte const>(bytes).subspan(96)));
    std::copy(encoded.begin(), encoded.end(), bytes.begin());
    auto path = root / ("malformed-" + std::to_string(mode) + ".kv");
    write_bytes(path, bytes);
    auto bad = native::open(path); // Deliberately skip scan: exercise GPU rejection.
    bool rejected = false;
    try { (void)gpu_merge(context, bad, *empty, root / "rejected-output.kv"); }
    catch (std::runtime_error const & error) {
      rejected = std::string_view(error.what()) == "GPU byte input framing/retention validation failed";
      if (!rejected) throw;
    }
    require(rejected, "malformed input accepted by GPU parser");
    std::cout << "reject," << mode << ",passed\n";
  }
}
#include "../gpu_merge/ef_output_test.h"

int main(int argc, char ** argv) {
  try { @autoreleasepool {
    if (argc < 4 || argc > 5) throw std::invalid_argument("usage: prototype metallib output-directory check|bench [trials]");
    require(!pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0), "thread QoS request failed");
    int priority{}; qos_class_t qos{};
    require(!pthread_get_qos_class_np(pthread_self(), &qos, &priority) &&
      qos == QOS_CLASS_USER_INITIATED && !priority, "thread QoS verification failed");
    std::filesystem::path root(argv[2]); std::filesystem::create_directories(root);
    gpu context(argv[1]);
    std::vector<std::string> entries{"scan_blocks", "scan_add", "byte_parse_input", "prefix_leaf", "prefix_reduce",
      "compressed_prefix", "compressed_cache", "merge_order", "merge_keep", "merge_compact", "byte_value_width",
      "byte_merge_sizes", "byte_merge_emit_words", "ef_output_sparse_count", "ef_output_low", "ef_output_high",
      "ef_output_samples", "ef_output_sparse"};
    for (auto const & entry : entries) (void)context.pipeline(entry);
    std::cerr << "device=" << context.device.name.UTF8String << ",unified=" << context.device.hasUnifiedMemory
      << ",thread_qos=USER_INITIATED,relative_priority=0\n";
    std::string mode(argv[3]);
    if (mode == "check") {
      static_assert(gpu_registry<policy, everett::replace_native_value>::enabled);
      static_assert(!gpu_registry<everett::string_policy, everett::replace_native_value>::enabled);
      for (auto const & f : correctness_cases()) run_case(context, root, f, 1, false);
      malformed_cases(context, root);
      ef_output_adversarial(context); // Same abstract-unit EF kernels, including sparse exceptions.
    } else if (mode == "bench") {
      auto trials = argc == 5 ? unsigned(std::stoul(argv[4])) : 5;
      std::cout << std::setprecision(17)
        << "case,trial,older,newer,output_records,input_a_bytes,input_b_bytes,output_bytes,common_value_bytes,cpu_width_pass,cpu_ms,gpu_complete_ms,gpu_device_ms,parse_ms,order_ms,size_ms,emit_ms,assembly_ms\n";
      for (unsigned n : {4096u, 32768u, 131072u}) {
        run_case(context, root, generated("fixed-" + std::to_string(n), n, n, 24, 33, true, false), trials, true);
        run_case(context, root, generated("variable-" + std::to_string(n), n, n, 24, 33, false, true), trials, true);
      }
    } else throw std::invalid_argument("unknown mode");
  } } catch (std::exception const & e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
