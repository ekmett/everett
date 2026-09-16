/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks optional GPU merge framing against independent logical cases.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

// Included after prototype.mm's concrete GPU/input helpers. No test operation
// is part of a benchmark timing sample.
namespace gpu_adversarial {
using row = std::pair<std::string, std::optional<std::string>>;
using rows = std::vector<row>;
struct fixture {
  std::string name;
  rows older, newer;
  std::optional<std::pair<std::uint64_t, std::uint8_t>> first_payload;
};

inline rows sorted(rows input) {
  std::sort(input.begin(), input.end(),
            [](auto const &a, auto const &b) { return a.first < b.first; });
  for (std::size_t i = 1; i < input.size(); ++i)
    require(input[i - 1].first < input[i].first, "adversarial input duplicate");
  return input;
}

inline std::shared_ptr<native const> input(std::filesystem::path const &path, rows const &records) {
  everett::sort_profile_writer<policy> writer;
  for (auto const &[key, value] : records)
    writer.append<sort_type>(key, value);
  auto array = writer.finish();
  auto encoded = everett::encoded_sort_sections<policy>::from(array).materialize();
  write_bytes(path, encoded);
  auto result = std::make_shared<native const>(native::open(everett::file<policy>::open(path)));
  result->scan();
  return result;
}

inline unsigned key_bit(std::string const & key, std::uint64_t at) {
  return (static_cast<unsigned char>(key[at >> 3]) >> (7 - (at & 7))) & 1;
}

inline std::vector<everett::sort_profile_frame> frames(native const & source, rows const & records) {
  require(source.size() == records.size(), "adversarial frame count");
  std::vector<everett::sort_profile_frame> result;
  if (records.empty()) return result;
  auto view = source.view();
  auto frame = view.encoded_at(0);
  for (std::size_t i = 0; i != records.size(); ++i) {
    result.push_back(frame);
    auto retained = frame.retained ? frame.retained - 1 : 0;
    auto bits = std::uint64_t(records[i].first.size()) * 8;
    require(frame.key_units == bits + 1, "adversarial frame logical size");
    // The independent predecessor walk probes both sides of retained cuts,
    // key endpoints, and interior bits. No GPU metadata is used as an oracle.
    std::array<std::uint64_t, 9> probes{0, 7, 8, bits / 2,
      bits ? bits - 1 : 0, retained ? retained - 1 : 0, retained,
      retained + 1, bits ? (i * 713 + 19) % bits : 0};
    for (auto bit : probes) if (bit < bits) {
      auto owner = i;
      while ((result[owner].retained ? result[owner].retained - 1 : 0) > bit) {
        require(owner != 0, "adversarial missing prefix owner");
        --owner;
      }
      auto const & writer = result[owner];
      auto start = writer.retained ? writer.retained - 1 : 0;
      require(bit - start < writer.literal[1].size(), "adversarial owner literal extent");
      require(everett::profile_detail::load_bits(writer.literal[1], bit - start, 1) == key_bit(records[i].first, bit),
        "adversarial inherited bit owner");
    }
    if (i + 1 != records.size()) frame = view.next(frame);
  }
  return result;
}

inline void check(gpu &context, std::filesystem::path const &root, fixture const &test) {
  auto directory = root / test.name;
  std::filesystem::create_directories(directory);
  auto a = input(directory / "older.kv", test.older);
  auto b = input(directory / "newer.kv", test.newer);
  auto older_frames = frames(*a, test.older), newer_frames = frames(*b, test.newer);
  // std::map is an independent chronological replacement oracle. Keeping an
  // explicit null value distinguishes a retained tombstone from a dropped key.
  std::map<std::string, std::optional<std::string>> expected;
  for (auto const &[key, value] : test.older)
    expected.insert_or_assign(key, value);
  for (auto const &[key, value] : test.newer)
    expected.insert_or_assign(key, value);
  require(!expected.empty(), "double-empty is outside the bounded GPU entry point");

  std::string const * previous = nullptr;
  for (auto const & [key, value] : expected) {
    (void)value;
    auto find = [&](rows const & source) {
      return std::lower_bound(source.begin(), source.end(), key,
        [](row const & candidate, std::string const & wanted) { return candidate.first < wanted; });
    };
    auto newer = find(test.newer);
    bool use_newer = newer != test.newer.end() && newer->first == key;
    auto const & source = use_newer ? test.newer : test.older;
    auto found = use_newer ? newer : find(test.older);
    auto const & frame = (use_newer ? newer_frames : older_frames)[std::size_t(found - source.begin())];
    auto retained = frame.retained ? frame.retained - 1 : 0;
    std::uint64_t common = 0, bits = std::uint64_t(key.size()) * 8;
    if (previous) {
      auto limit = std::min(bits, std::uint64_t(previous->size()) * 8);
      while (common < limit && key_bit(*previous, common) == key_bit(key, common)) ++common;
    }
    require(common >= retained, "adversarial output suffix inheritance lemma");
    auto literal = frame.literal[1].subview(common - retained, bits - common);
    auto full = everett::sort_codec_detail::string_bits(key).subview(common, bits - common);
    require(everett::compare_bits(literal, full) == 0, "adversarial direct source suffix");
    previous = &key;
  }

  everett::sort_profile_merge_builder<policy, native> cpu(a, b);
  cpu.step(std::numeric_limits<std::uint64_t>::max());
  auto merged = cpu.finish();
  auto encoded = everett::encoded_sort_sections<policy>::from(merged).materialize();
  auto result = gpu_merge(context, *a, *b, directory / "gpu.kv");
  auto opened = native::open(everett::file<policy>::open(result.path));
  opened.scan();
  require(opened.size() == expected.size(), "adversarial output count");
  auto cursor = opened.view().cursor();
  for (auto const &[key, value] : expected) {
    require(!cursor.done(), "adversarial truncated output");
    auto record = cursor.peek();
    auto key_bits = record.key.prefix.subview(1, record.key.prefix.size() - 1);
    auto actual_key = everett::sort_profile_key<everett::fc_string_key<>>::decode_order(key_bits);
    everett::sort_bit_reader reader(record.value);
    auto actual_value = everett::sort_codec<sort_type>::value_codec::read(reader);
    require(actual_key == key && actual_value == value && reader.empty(),
            "adversarial replacement semantics");
    cursor.advance();
  }
  require(cursor.done(), "adversarial extra output");
  std::ifstream stream(result.path, std::ios::binary);
  std::vector<char> actual((std::istreambuf_iterator<char>(stream)), {});
  require(actual.size() == encoded.size() &&
              std::memcmp(actual.data(), encoded.data(), encoded.size()) == 0,
          "adversarial canonical KV03 byte mismatch");
  if (test.first_payload) {
    // Independent hand-derived one-record wire strings: EG0(1)=010 for
    // absolute selector retention, EG0(0)=1 for the empty suffix, then 0
    // (tombstone) or 1,EG0(0)=11 (present empty value).
    auto payload = opened.view().data();
    require(payload.size() == test.first_payload->first &&
                std::to_integer<std::uint8_t>(payload.storage()[0]) == test.first_payload->second,
            "adversarial hand-derived bit pattern");
  }
  std::cout << "adversarial," << test.name << ",passed," << result.count << ',' << result.bits
            << '\n';
}

inline std::string binary_key(unsigned i) {
  std::string key("prefix\0", 7);
  key.push_back(char(i >> 8));
  key.push_back(char(i));
  return key;
}

inline std::vector<fixture> cases() {
  std::vector<fixture> result;
  result.push_back({"empty-key-tombstone", {}, {{"", std::nullopt}}, {{5, 0x50}}});
  result.push_back({"empty-key-present-empty", {{"", std::string{}}}, {}, {{6, 0x5c}}});
  rows keys{{"", "old"},
            {std::string(1, '\0'), "zero"},
            {"a", "a"},
            {std::string("a\0", 2), "a-zero"},
            {std::string("a\0\0", 3), std::nullopt},
            {std::string("a\0\x80", 3), "high"},
            {std::string("a\x01", 2), "one"},
            {std::string("a\x7f", 2), "127"},
            {std::string("a\x80", 2), "128"},
            {std::string("a\xff", 2), "255"},
            {"b", "last"}};
  result.push_back({"proper-prefix-and-binary",
                    sorted(keys),
                    sorted({{"", std::string{}},
                            {"a", std::nullopt},
                            {std::string("a\0", 2), std::string("v\0\xff", 3)},
                            {std::string("a\0\0", 3), "revived"},
                            {"b", std::nullopt}}),
                    {}});
  result.push_back({"empty-newer", sorted(keys), {}, {}});
  result.push_back({"empty-older", {}, sorted(keys), {}});
  for (unsigned bit = 0; bit != 8; ++bit)
    result.push_back({"lcp-bit-" + std::to_string(bit),
                      {{std::string(1, '\0'), std::string{}}},
                      {{std::string(1, char(1u << bit)), std::string("\0\xff", 2)}},
                      {}});
  for (unsigned n : {7u, 8u, 9u, 14u, 15u, 16u, 29u, 30u, 31u, 255u, 256u, 257u}) {
    rows older, newer, all_deleted;
    for (unsigned i = 0; i != n; ++i) {
      auto key = binary_key(i);
      auto value = std::string(i % 33, char(i));
      older.emplace_back(key, value);
      all_deleted.emplace_back(key, std::nullopt);
      if (i % 3 != 1)
        newer.emplace_back(key, i % 2 ? std::optional<std::string>{value + "new"} : std::nullopt);
    }
    result.push_back({"boundary-mixed-" + std::to_string(n), older, newer, {}});
    result.push_back(
        {"boundary-tombstones-" + std::to_string(n), std::move(older), std::move(all_deleted), {}});
  }
  rows short_run, long_run;
  for (unsigned i = 0; i != 513; ++i) {
    auto key = binary_key(i);
    long_run.emplace_back(key, std::string(16, char(i)));
    if (i == 0 || i == 255 || i == 512)
      short_run.emplace_back(key, std::nullopt);
  }
  result.push_back({"skewed-newer-short", long_run, short_run, {}});
  result.push_back({"skewed-older-short", std::move(short_run), std::move(long_run), {}});
  for (unsigned n : {2u, 3u, 14u, 15u, 16u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 128u, 129u}) {
    for (bool descending : {false, true}) {
      rows older, newer;
      for (unsigned i = 0; i != n; ++i) {
        auto key = descending ? std::string(n - i, 'a') : std::string(i, 'a');
        if (descending && i) key.push_back('b');
        if (i % 3 != 1) older.emplace_back(key, std::string(i % 17, char(i)));
        if (i % 3 != 0) newer.emplace_back(key, i % 4 ? std::optional<std::string>{std::string(i % 11, char(255 - i))} : std::nullopt);
      }
      result.push_back({std::string(descending ? "retention-down-" : "retention-up-") + std::to_string(n),
        sorted(std::move(older)), sorted(std::move(newer)), {}});
    }
  }
  unsigned seed = 0;
  for (unsigned prefix : {0u, 1u, 7u, 31u, 63u, 127u, 255u, 511u}) {
    std::mt19937 random(0x78364b1u + seed++);
    std::string stem(prefix, '\0');
    for (auto & byte : stem) byte = char(random() & 255);
    std::map<std::string, bool> keys;
    keys.emplace(stem, true);
    unsigned count = 31 + seed * 4;
    while (keys.size() != count) {
      auto key = stem;
      auto length = 1 + random() % 15;
      for (unsigned i = 0; i != length; ++i) key.push_back(char(random() & 255));
      keys.emplace(std::move(key), true);
    }
    rows older, newer;
    unsigned i = 0;
    for (auto const & [key, ignored] : keys) {
      (void)ignored;
      std::string value(random() % 24, '\0');
      for (auto & byte : value) byte = char(random() & 255);
      if (i % 3 != 1) older.emplace_back(key, value);
      if (i % 3 != 0) newer.emplace_back(key, i % 5 ? std::optional<std::string>{value + '\0'} : std::nullopt);
      ++i;
    }
    result.push_back({"random-binary-prefix-" + std::to_string(prefix), std::move(older), std::move(newer), {}});
  }
  // Repeated whole-word gathers, length-code transitions and partial final
  // words. Each case crosses two W15 cuts and preserves binary value bytes.
  for (unsigned family = 0; family != 3; ++family) {
    std::array<unsigned, 3> lengths = family == 0 ? std::array{31u, 32u, 33u} :
      family == 1 ? std::array{127u, 128u, 129u} : std::array{511u, 512u, 513u};
    for (bool reverse : {false, true}) {
      rows older, newer;
      for (unsigned i = 0; i != 34; ++i) {
        auto key = binary_key(i);
        auto length = lengths[(i + unsigned(reverse)) % lengths.size()];
        std::string value(length, '\0');
        for (unsigned byte = 0; byte != length; ++byte) value[byte] = char((byte * 193 + i * 17) & 255);
        older.emplace_back(key, value);
        if (i % 3 != 1) {
          std::reverse(value.begin(), value.end());
          newer.emplace_back(key, i % 5 ? std::optional<std::string>{std::move(value)} : std::nullopt);
        }
      }
      if (reverse) std::swap(older, newer);
      result.push_back({"value-word-gathers-" + std::to_string(family) + (reverse ? "-reverse" : ""),
        std::move(older), std::move(newer), {}});
    }
  }
  return result;
}
} // namespace gpu_adversarial

inline void adversarial(gpu &context, std::filesystem::path const &directory) {
  for (auto const &test : gpu_adversarial::cases()) {
    try {
      gpu_adversarial::check(context, directory / "adversarial", test);
    } catch (std::exception const &error) {
      throw std::runtime_error(test.name + ": " + error.what());
    }
  }
}
