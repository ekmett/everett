/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Runs the complete compressed Metal path on the exact fixed-key logical fixtures.
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main kv_original_main
#include "../gpu_merge/prototype.mm"
#undef main
#pragma clang diagnostic pop
#include "cases.h"

std::shared_ptr<native const> encode_matched(std::filesystem::path const &path,
    std::vector<fixed_fixture::record> const &rows) {
  everett::sort_profile_writer<policy> writer;
  for (auto const &row : rows) {
    auto key = matched_fixture::key_bytes(row.k);
    std::string value;
    if (!row.value.empty()) value.assign(reinterpret_cast<char const *>(row.value.data()), row.value.size());
    writer.append<sort_type>(key, std::optional<std::string>(std::move(value)));
  }
  auto built = writer.finish();
  auto bytes = everett::encoded_sort_sections<policy>::from(built).materialize();
  write_bytes(path, bytes);
  auto result = std::make_shared<native const>(native::open(everett::file<policy>::open(path)));
  result->scan(); return result;
}
void verify_logical(native const &input, std::vector<fixed_fixture::record> const &expected) {
  auto cursor = input.view().cursor();
  std::size_t index = 0;
  while (!cursor.done()) {
    require(index < expected.size(), "KV logical count");
    auto frame = cursor.peek();
    auto key = matched_fixture::key_bytes(expected[index].k);
    require(frame.key.prefix.size() == 129, "KV logical key width");
    for (unsigned i = 0; i < 16; ++i)
      require(everett::profile_detail::load_bits(frame.key.prefix, 1 + 8 * i, 8) == static_cast<unsigned char>(key[i]), "KV logical key");
    everett::sort_bit_reader reader(frame.value);
    auto value = everett::sort_codec<sort_type>::value_codec::read(reader);
    require(value.has_value() && reader.empty() && value->size() == expected[index].value.size() &&
      (value->empty() || std::memcmp(value->data(), expected[index].value.data(), value->size()) == 0), "KV logical value");
    ++index; cursor.advance();
  }
  require(index == expected.size(), "KV logical EOF");
}
int main(int argc, char **argv) {
  @autoreleasepool { try {
    require(argc == 5, "usage: kv kernels.metallib scratch case-index trial-count");
    auto index = unsigned(std::stoul(argv[3])), trials = unsigned(std::stoul(argv[4]));
    auto f = matched_fixture::make(index);
    auto expected = matched_fixture::merged(f);
    std::filesystem::path root(argv[2]); std::filesystem::create_directories(root);
    matched_fixture::save(root, f, expected);
    auto a = encode_matched(root / "input-a.kv", f.a), b = encode_matched(root / "input-b.kv", f.b);
    verify_logical(*a, f.a); verify_logical(*b, f.b);
    everett::sort_profile_merge_builder<policy, native> cpu(a, b);
    cpu.step(std::numeric_limits<std::uint64_t>::max());
    auto built = cpu.finish();
    auto oracle = everett::encoded_sort_sections<policy>::from(built).materialize();
    gpu context(argv[1]); std::cerr << "device=" << context.device.name.UTF8String << '\n';
    compressed_inputs = use_prefix_cache = use_word_emitter = gpu_output_ef = true;
    use_prefix_tiles = use_output_plan = conservative_tombstones = verify_compressed_descriptors = false;
    cancellation_path = collision_path::disabled;
    // The untimed warmup creates every pipeline used by the measured path.
    std::size_t value_bytes = 0;
    for (auto const &row : expected) value_bytes += row.value.size();
    matched_fixture::header();
    for (int trial = -1; trial < int(trials); ++trial) {
      merge_result result;
      auto begin = clock_type::now();
      @autoreleasepool { result = gpu_merge(context, *a, *b, root / "output.kv"); }
      auto complete = elapsed(begin);
      std::ifstream stream(result.path, std::ios::binary);
      std::vector<char> actual((std::istreambuf_iterator<char>(stream)), {});
      require(actual.size() == oracle.size() && std::memcmp(actual.data(), oracle.data(), oracle.size()) == 0, "KV canonical output");
      auto opened = native::open(everett::file<policy>::open(result.path)); opened.scan();
      if (trial < 0) verify_logical(opened, expected);
      matched_fixture::row(index, f.variable ? "kv-variable" : "kv-fixed16", trial,
        std::filesystem::file_size(root / "input-a.kv"), std::filesystem::file_size(root / "input-b.kv"),
        result.bytes, result.count, value_bytes, complete, result.total, result.gpu_ms, -1);
    }
    return 0;
  } catch (std::exception const &e) { std::cerr << e.what() << '\n'; return 1; } }
}
