/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks conservative tombstone caps and explicitly authorized cleanup.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

namespace gpu_tombstone_test {
using gpu_adversarial::row;
using gpu_adversarial::rows;
using cap_map = std::map<std::string, std::uint64_t>;

// Public admission-cap API: full logical input keys are fixture data. This is
// test setup, never a timed host preparation path.
inline std::shared_ptr<native const> capped_input(std::filesystem::path const &path,
                                                rows const &input, cap_map const &caps) {
  everett::sort_profile_writer<policy> writer;
  for (auto const &[key, value] : input) {
    std::optional<std::uint64_t> cap;
    if (auto found = caps.find(key); found != caps.end()) cap = found->second;
    writer.append<sort_type>(key, value, cap);
  }
  auto result = writer.finish();
  auto wire = everett::encoded_sort_sections<policy>::from(result).materialize();
  write_bytes(path, wire);
  auto opened = std::make_shared<native const>(native::open(everett::file<policy>::open(path)));
  opened->scan();
  return opened;
}

struct expected_row {
  std::optional<std::string> value;
  everett::sort_profile_frame frame;
  std::uint64_t cap;
};

struct encoded_optional_replace {
  everett::bit_view operator()(everett::bit_view, everett::bit_view newer) const { return newer; }
  bool is_tombstone(everett::bit_view value) const { return !value.at(0); }
};

inline void check(gpu &context, std::filesystem::path const &directory, std::string const &name,
                  rows const &older, rows const &newer, bool certify) {
  auto root = directory / (name + (certify ? "-certified" : "-legacy"));
  std::filesystem::create_directories(root);
  auto a = capped_input(root / "older.kv", older, {});
  auto old_frames = gpu_adversarial::frames(*a, older);
  cap_map caps;
  if (certify) {
    for (auto const &[key, value] : newer) if (!value) {
      auto found = std::lower_bound(older.begin(), older.end(), key,
        [](row const &candidate, std::string const &wanted) { return candidate.first < wanted; });
      if (found != older.end() && found->first == key)
        caps.emplace(key, old_frames[std::size_t(found - older.begin())].retained);
    }
  }
  auto b = capped_input(root / "newer.kv", newer, caps);
  auto new_frames = gpu_adversarial::frames(*b, newer);
  std::map<std::string, expected_row> winners;
  for (std::size_t i = 0; i < older.size(); ++i)
    winners.emplace(older[i].first, expected_row{older[i].second, old_frames[i], old_frames[i].retained});
  unsigned certified = 0, legacy = 0;
  for (std::size_t i = 0; i < newer.size(); ++i) {
    auto cap = new_frames[i].retained;
    auto found = winners.find(newer[i].first);
    if (!newer[i].second && found != winners.end()) {
      auto raw = [](std::uint64_t n) { return n ? n - 1 : 0; };
      if (raw(cap) <= raw(found->second.cap)) ++certified;
      else ++legacy;
      cap = std::min(cap, found->second.cap);
    }
    winners.insert_or_assign(newer[i].first, expected_row{newer[i].second, new_frames[i], cap});
  }
  if (certify) require(legacy == 0, "certified tombstone retention exceeds target");
  if (name == "five-to-seven") {
    require(certify ? certified == 1 : legacy == 1, "5-to7 donor direction untested");
    require(winners.at(std::string(1, char(5))).cap == 6, "5-bit key-local cap changed");
  }
  for (auto coverage : {merge_coverage::preserve_tombstones, merge_coverage::complete_older_history}) {
    bool cleanup = coverage == merge_coverage::complete_older_history;
    everett::sort_profile_writer<policy> oracle;
    everett::bit_string previous;
    std::size_t expected_count = 0;
    for (auto const &[key, value] : winners) {
      if (cleanup && !value.value) continue;
      auto bits = everett::sort_profile_query<policy, sort_type>(key);
      auto common = everett::compare_common_bits(previous.view(), bits.view()).common_bits;
      if (!value.value) common = std::min(common, std::max(std::uint64_t(1), value.cap));
      std::array<everett::bit_view, 1> spans{bits.view()};
      oracle.append_frame(value.frame, spans, common, value.frame.value);
      previous = std::move(bits);
      ++expected_count;
    }
    auto expected = oracle.finish();
    auto wire = everett::encoded_sort_sections<policy>::from(expected).materialize();
    if (!cleanup) {
      // Independently exercise the production capped CPU merger, in addition
      // to the logical-map/full-key encoder above. Unary predicate keeps the
      // merge on borrowed spans rather than materializing keys for callbacks.
      everett::sort_profile_merge_builder<policy, native, encoded_optional_replace> cpu(a, b);
      cpu.step(std::numeric_limits<std::uint64_t>::max());
      auto composed = cpu.finish();
      auto cpu_wire = everett::encoded_sort_sections<policy>::from(composed).materialize();
      require(cpu_wire == wire && cpu.materialized_keys() == 0, "CPU capped merge oracle mismatch");
    }
    auto result = gpu_merge(context, *a, *b, root / (cleanup ? "cleanup.kv" : "preserved.kv"), coverage);
    auto opened = native::open(everett::file<policy>::open(result.path));
    opened.scan();
    require(opened.size() == expected_count && result.count == expected_count, "cleanup output count");
    std::ifstream stream(result.path, std::ios::binary);
    std::vector<char> actual((std::istreambuf_iterator<char>(stream)), {});
    require(actual.size() == wire.size() && std::memcmp(actual.data(), wire.data(), wire.size()) == 0,
            "tombstone output differs from CPU canonical encoder");
    auto cursor = opened.view().cursor();
    for (auto const &[key, value] : winners) {
      if (cleanup && !value.value) continue;
      require(!cursor.done(), "missing cleanup row");
      auto item = cursor.peek();
      auto decoded_key = everett::sort_profile_key<everett::fc_string_key<>>::decode_order(
        item.key.prefix.subview(1, item.key.prefix.size() - 1));
      everett::sort_bit_reader reader(item.value);
      auto decoded_value = everett::sort_codec<sort_type>::value_codec::read(reader);
      require(decoded_key == key && decoded_value == value.value && reader.empty(), "cleanup logical oracle");
      cursor.advance();
    }
    require(cursor.done(), "extra cleanup row");
    std::cout << "tombstone," << name << ',' << certify << ',' << cleanup << ",passed,"
              << certified << ',' << legacy << ',' << result.count << ',' << result.bits << '\n';
  }
}

inline std::string ramp(unsigned bits, unsigned ones) {
  std::string result((bits + 7) / 8, char(0));
  for (unsigned i = 0; i < ones; ++i)
    result[i / 8] = char(static_cast<unsigned char>(result[i / 8]) | (128u >> (i % 8)));
  return result;
}

inline void run(gpu &context, std::filesystem::path const &directory) {
  auto one = [](unsigned x) { return std::string(1, char(x)); };
  for (bool certify : {false, true}) {
    check(context, directory, "five-to-seven", {{one(0), "a"}, {one(5), "b"}, {one(6), "c"}},
          {{one(4), "d"}, {one(5), std::nullopt}}, certify);
    // Cleanup's final key combines1 inherited bit from the canceled0x80
    // literal and7 bits from its own0xc0 literal in the same output word.
    check(context, directory, "two-owner-subbyte", {{one(0), "a"}, {one(128), "b"}, {one(192), "c"}},
          {{one(128), std::nullopt}}, certify);
    check(context, directory, "tombstone-resurrection", {{"", std::nullopt}, {"prefix", "old"},
          {"prefix/a", std::nullopt}, {"prefix/b", "last"}}, {{"", "new"}, {"prefix", std::nullopt},
          {"prefix/a", "restored"}, {"prefix/aa", std::nullopt}}, certify);
    for (unsigned bits : {7u, 14u, 15u, 16u, 30u, 31u, 32u, 127u, 255u, 513u}) {
      rows older, newer;
      for (unsigned i = 0; i <= bits; ++i) {
        auto key = ramp(bits, i);
        older.emplace_back(key, std::string(80, char(i)));
        if (i && i != bits) newer.emplace_back(key, std::nullopt);
      }
      check(context, directory, "adjacent-ramp-" + std::to_string(bits), older, newer, certify);
      for (auto &[key, value] : older) { (void)key; value.reset(); }
      // All-absent winners, including a final partial W15 block and EOF.
      check(context, directory, "all-deleted-" + std::to_string(bits), older, newer, certify);
    }
    check(context, directory, "newer-only", {}, gpu_adversarial::sorted({{"", std::nullopt}, {"a", std::nullopt},
          {"ab", "kept"}, {std::string("a\0z", 3), std::nullopt}}), certify);
  }
}
} // namespace gpu_tombstone_test
