#include <everett/blob.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using namespace everett;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }

  template <class F>
  void rejects(F && f) {
    bool rejected = false;
    try { f(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid input accepted");
  }

  std::string numbered(unsigned n) {
    auto number = std::to_string(n);
    return "shared-prefix/" + std::string(5 - number.size(), '0') + number;
  }

  void front_roundtrip() {
    std::vector<front_record<9>> records;
    std::vector<std::string> keys{"", std::string(1, '\0'), std::string("\0x", 2), "a", "aa", "ab",
                                  "abc", "abd", std::string(1, char(128)), std::string(1, char(255))};
    for (unsigned i = 0; i != 140; ++i) keys.push_back(numbered(i));
    std::sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) { return compare_keys(a, b) < 0; });
    for (std::size_t i = 0; i != keys.size(); ++i) {
      records.push_back({keys[i], encode_blob_value(i % 7 ? std::optional<std::uint64_t>(i) : std::nullopt)});
    }
    for (std::uint64_t restart : {0, 3, 18}) {
      auto array = front_array<9>::build(records, {}, restart);
      require(array.size() == records.size(), "front-coded count");
      auto view = array.view();
      std::size_t count = 0;
      view.visit_all([&](front_item<9> item) {
        require(item.ordinal == count && item.key.prefix == records[count].key, "sequential front-code roundtrip");
        require(std::equal(item.value.begin(), item.value.end(), records[count].value.begin()), "value roundtrip");
        ++count;
        return true;
      });
      require(count == records.size(), "sequential visit count");
      for (std::size_t i = 0; i != records.size(); ++i) {
        auto full = view.reconstruct_at(i);
        require(full.prefix == records[i].key && full.full_size == records[i].key.size(), "backward reconstruction");
        for (std::size_t limit : {0u, 1u, 3u, 7u}) {
          auto part = view.reconstruct_at(i, limit);
          require(part.prefix == records[i].key.substr(0, limit), "partial backward reconstruction");
        }
      }
      for (std::size_t first = 0; first != records.size(); ++first) {
        auto last = std::min(records.size(), first + 15);
        auto anchor = first ? front_anchor::complete(records[first - 1].key) : front_anchor{};
        std::size_t i = first;
        view.visit_window(first, last, anchor, 7, [&](front_item<9> item) {
          require(item.key.prefix == records[i].key.substr(0, 7), "forward window prefix");
          require(item.key.full_size == records[i].key.size(), "forward window length");
          ++i;
          return true;
        });
        require(i == last, "window visit count");
      }
      for (std::size_t g = 0; g < (records.size() + 14) / 15; ++g) {
        auto start = view.group_offsets().offset(g, 9);
        auto i = g * 15;
        auto next = view.encoded_at(i).next_offset;
        require(start < next && next <= array.bytes().size(), "group offset and header navigation");
      }
    }
  }

  void surrogate_anchor_and_redundant_prefix() {
    std::vector<front_record<0>> records{{"aa", {}}, {"abc", {}}, {"abde", {}}};
    auto array = front_array<0>::build(records);
    require(array.view().encoded_at(1).shared == 1, "expected actual predecessor LCP");
    std::string query = "abd";
    auto anchor = front_anchor::complete("ab");
    unsigned visited = 0;
    array.view().visit_window(1, 3, anchor, query.size(), [&](front_item<0> item) {
      auto expected = visited++ ? 1 : -1;
      require(compare_prefix(item.key, query) == expected, "surrogate anchor comparison");
      return true;
    });
    require(visited == 2, "surrogate anchor visit count");

    // A shortened shared field is only a copy count. Re-emitted letters must
    // participate in the comparison even when shared < LCP(anchor, query).
    std::array<std::uint64_t, 3> ceilings{0, 0, 1};
    auto redundant = front_array<0>::build(records, ceilings);
    redundant.view().visit_window(1, 3, anchor, query.size(), [&](front_item<0> item) {
      require(item.key.prefix == records[item.ordinal].key.substr(0, query.size()), "redundant prefix roundtrip");
      return true;
    });
    require(redundant.view().reconstruct_at(2).prefix == "abde", "redundant backward reconstruction");

    // Exhaust every intermediate anchor between actual predecessor and key.
    std::vector<std::string> small{"", "a", "aa", "aaa", "aab", "ab", "aba", "abb", "b", "ba"};
    for (std::size_t x = 0; x != small.size(); ++x) {
      for (std::size_t y = x; y != small.size(); ++y) {
        std::array<front_record<0>, 2> pair{{{small[x], {}}, {small[y], {}}}};
        auto encoded = front_array<0>::build(pair);
        for (std::size_t a = x; a <= y; ++a) {
          for (std::uint64_t limit = 0; limit != 4; ++limit) {
            front_anchor partial{std::string_view(small[a]).substr(0, limit), small[a].size()};
            encoded.view().visit_window(1, 2, partial, limit, [&](front_item<0> item) {
              require(item.key.prefix == small[y].substr(0, limit), "intermediate anchor prefix convexity");
              return true;
            });
          }
        }
      }
    }
  }

  void fixed_slots_and_long_prefixes() {
    std::vector<front_record<0>> zero;
    std::vector<front_record<9>> nine;
    for (unsigned i = 0; i != 93; ++i) {
      auto key = numbered(i);
      zero.push_back({key, {}});
      nine.push_back({key, encode_blob_value(std::numeric_limits<std::uint64_t>::max() - i)});
    }
    auto a = front_array<0>::build(zero);
    auto b = front_array<9>::build(nine);
    auto groups = (zero.size() + 14) / 15;
    for (std::size_t g = 0; g <= groups; ++g) {
      require(a.group_offsets().view().residual(g) == b.group_offsets().view().residual(g),
              "fixed value slots must not enlarge Elias-Fano universe");
      require(b.group_offsets().view().offset(g, 9) - a.group_offsets().view().offset(g) ==
                std::min(g * 15, zero.size()) * 9, "fixed stride restoration");
    }
    std::string prefix(200000, 'x');
    std::vector<front_record<0>> long_records{{prefix + "a", {}}, {prefix + "b", {}}, {prefix + "c", {}}};
    auto long_array = front_array<0>::build(long_records);
    front_anchor partial{"xxx", prefix.size() + 1};
    long_array.view().visit_window(1, 3, partial, 3, [](front_item<0> item) {
      require(item.key.prefix == "xxx" && item.key.full_size == 200001, "long key partial reconstruction");
      return true;
    });
  }

  void lpfc_span() {
    std::vector<front_record<9>> records;
    for (unsigned i = 0; i != 800; ++i) records.push_back({numbered(i), encode_blob_value(i)});
    for (std::uint64_t factor : {3, 18}) {
      auto encoded = front_array<9>::build(records, {}, factor);
      std::uint64_t at = 0;
      std::uint64_t anchor = 0;
      std::size_t restarts = 0;
      for (std::size_t i = 0; i != records.size(); ++i) {
        auto record = encoded.view().encoded_at(i);
        if (!record.shared) {
          anchor = at;
          ++restarts;
        }
        require(at - anchor <= factor * records[i].key.size(), "LPFC actual byte-span bound");
        require(encoded.view().reconstruct_at(i).prefix == records[i].key, "LPFC reconstruction");
        at = record.next_offset;
      }
      require(restarts > 1 && restarts < records.size(), "LPFC exercises adaptive restarts");
    }
  }

  struct virtual_entry {
    std::string key;
    bool borrowed;
    std::size_t ordinal;
  };

  void check_blob(std::vector<blob_record> const & native, std::vector<std::string> const & borrowed) {
    auto encoded = blob::build(native, borrowed);
    require(encoded.native().size() == native.size() && encoded.borrowed().size() == borrowed.size(), "blob stream sizes");
    auto bidirectional = blob::build(native, borrowed, 18, {}, borrowed_prefix_policy::bidirectional);
    auto ordinary = blob::build(native, borrowed, 18, {}, borrowed_prefix_policy::ordinary);
    std::uint64_t shared_bytes = 0;
    std::uint64_t two_sided_bytes = 0;
    std::uint64_t ordinary_bytes = 0;
    for (std::size_t i = 0; i != borrowed.size(); ++i) {
      shared_bytes += encoded.borrowed().view().encoded_at(i).suffix.size();
      two_sided_bytes += bidirectional.borrowed().view().encoded_at(i).suffix.size();
      ordinary_bytes += ordinary.borrowed().view().encoded_at(i).suffix.size();
    }
    require(shared_bytes <= two_sided_bytes && two_sided_bytes <= 2 * ordinary_bytes,
            "shared-cut suffix bytes <= bidirectional <= twice ordinary FC");
    std::vector<virtual_entry> catalog;
    for (std::size_t i = 0; i != native.size(); ++i) catalog.push_back({native[i].key, false, i});
    for (std::size_t i = 0; i != borrowed.size(); ++i) catalog.push_back({borrowed[i], true, i});
    std::stable_sort(catalog.begin(), catalog.end(), [](auto const & a, auto const & b) {
      auto order = compare_keys(a.key, b.key);
      return order ? order < 0 : a.borrowed < b.borrowed;
    });
    for (std::size_t i = 0; i != borrowed.size(); ++i) {
      auto hit = std::find_if(native.begin(), native.end(), [&](auto const & record) { return record.key == borrowed[i]; });
      require(encoded.false_borrow(i) == (hit != native.end()), "false borrow equality flag");
    }
    std::size_t native_at = 0;
    std::size_t borrowed_at = 0;
    for (std::size_t g = 0; g != encoded.group_count(); ++g) {
      auto window = encoded.project(g);
      require(window.native_first == native_at && window.borrowed_first == borrowed_at, "rank15 lower projection");
      for (auto i = g * 15; i != std::min(catalog.size(), g * 15 + 15); ++i) {
        if (catalog[i].borrowed) ++borrowed_at;
        else ++native_at;
      }
      require(window.native_last == native_at && window.borrowed_last == borrowed_at, "rank15 upper projection");
    }
    if (catalog.empty()) {
      rejects([&] { encoded.search_window("a", 0, {}); });
      return;
    }
    std::vector<std::string> queries{"", "!", "a", "zzzz", std::string(1, char(255))};
    for (auto const & record : catalog) {
      queries.push_back(record.key);
      queries.push_back(record.key + '\0');
    }
    for (auto const & query : queries) {
      auto upper = std::upper_bound(catalog.begin(), catalog.end(), query,
        [](auto const & q, auto const & record) { return compare_keys(q, record.key) < 0; });
      auto ordinal = upper == catalog.begin() ? 0 : static_cast<std::size_t>(upper - catalog.begin() - 1);
      auto group = ordinal / 15;
      auto const & anchor_key = catalog[group * 15].key;
      // Prefix-only incoming state, including the case query precedes the root.
      front_anchor anchor{std::string_view(anchor_key).substr(0, query.size()), anchor_key.size()};
      auto result = encoded.search_window(query, group, anchor);
      auto expected = std::find_if(native.begin(), native.end(), [&](auto const & record) { return record.key == query; });
      require(bool(result.native) == (expected != native.end()), "blob bounded lookup presence oracle");
      if (result.native) {
        require(result.native->ordinal == std::size_t(expected - native.begin()), "blob exact native ordinal oracle");
        require(result.native->value == expected->value, "blob tombstone/value oracle");
      }
      auto expected_sample = std::upper_bound(borrowed.begin(), borrowed.end(), query,
        [](auto const & q, auto const & key) { return compare_keys(q, key) < 0; });
      require(bool(result.borrowed_predecessor) == (expected_sample != borrowed.begin()), "borrowed predecessor presence oracle");
      if (result.borrowed_predecessor) {
        auto const & predecessor = *result.borrowed_predecessor;
        auto expected_ordinal = static_cast<std::size_t>(expected_sample - borrowed.begin() - 1);
        require(predecessor.ordinal == expected_ordinal, "borrowed predecessor ordinal oracle");
        require(predecessor.target_ordinal == expected_ordinal * 15, "borrowed target mapping");
        require(predecessor.has_context, "conservative borrowed predecessor always has context");
        if (predecessor.has_context) {
          require(predecessor.prefix == borrowed[expected_ordinal].substr(0, query.size()), "borrowed partial context oracle");
          require(predecessor.full_size == borrowed[expected_ordinal].size(), "borrowed full length oracle");
        }
      }
    }
  }

  void blob_oracles() {
    check_blob({}, {});
    check_blob({{"", 0}, {"a", std::nullopt}}, {});
    check_blob({}, {"", "a", "a", "abc"});
    check_blob({{"aa", 1}, {"abc", 2}, {"abd", std::nullopt}}, {"ab", "abc", "abd"});
    std::mt19937 rng(21973);
    for (unsigned trial = 0; trial != 24; ++trial) {
      std::vector<blob_record> native;
      std::vector<std::string> borrowed;
      for (unsigned i = 0; i != 160; ++i) {
        auto key = numbered(i);
        if (rng() % 3) native.push_back({key, rng() % 5 ? std::optional<std::uint64_t>(i) : std::nullopt});
        if (rng() % 3) borrowed.push_back(key);
        if (!borrowed.empty() && borrowed.back() == key && rng() % 7 == 0) borrowed.push_back(key);
      }
      check_blob(native, borrowed);
    }
    // A long equality run crosses several virtual boundaries. The native q
    // lies before the selected window; false-borrow retrieves it correctly.
    std::vector<blob_record> native{{"a", 10}, {"q", 99}, {"z", std::nullopt}};
    std::vector<std::string> duplicate_samples(70, "q");
    check_blob(native, duplicate_samples);
  }

  void two_sided_borrowed_context() {
    std::vector<blob_record> native;
    for (unsigned i = 0; i != 13; ++i) native.push_back({"aaac" + numbered(i), i});
    native.push_back({"aamm", 100});
    native.push_back({"aazx", 101});
    std::vector<std::string> samples{"aaaa", "aaab", "aazz"};
    auto encoded = blob::build(native, samples, 18, {}, borrowed_prefix_policy::bidirectional);
    require(encoded.borrowed().view().encoded_at(1).shared == 2, "two-sided prefix clamps right LCP");
    auto result = encoded.search_window("aamn", 1, front_anchor::complete("aamm"));
    require(result.borrowed_predecessor && result.borrowed_predecessor->has_context &&
              result.borrowed_predecessor->prefix == "aaab", "preceding sample decodes from upper anchor");
    auto ordinary = blob::build(native, samples, 18, {}, borrowed_prefix_policy::ordinary);
    require(ordinary.borrowed().view().encoded_at(1).shared == 3, "ordinary encoding keeps larger left LCP");
    auto missing = ordinary.search_window("aamn", 1, front_anchor::complete("aamm"));
    require(missing.borrowed_predecessor && !missing.borrowed_predecessor->has_context,
            "ordinary policy reports missing preceding context explicitly");

    std::vector<std::string> keys{"", "a", "aaa", "aaaa", "aaaab", "aaac", "aabc", "ab", "aba", "abb", "b"};
    for (std::size_t begin = 0; begin != keys.size(); ++begin) {
      for (std::size_t end = begin + 1; end <= keys.size(); ++end) {
        auto slice = std::span<std::string const>(keys).subspan(begin, end - begin);
        auto two_sided = blob::build({}, slice, 18, {}, borrowed_prefix_policy::bidirectional);
        auto one_sided = blob::build({}, slice, 18, {}, borrowed_prefix_policy::ordinary);
        std::size_t one_bytes = 0;
        std::size_t two_bytes = 0;
        for (std::size_t i = 0; i != slice.size(); ++i) {
          one_bytes += one_sided.borrowed().view().encoded_at(i).suffix.size();
          two_bytes += two_sided.borrowed().view().encoded_at(i).suffix.size();
          for (auto const & upper : keys) {
            if (compare_keys(slice[i], upper) > 0 ||
                (i + 1 < slice.size() && compare_keys(upper, slice[i + 1]) > 0)) continue;
            two_sided.borrowed().view().visit_window(i, i + 1, front_anchor::complete(upper), 3,
              [&](front_item<0> item) {
                require(item.key.prefix == slice[i].substr(0, 3), "all upper anchors decode predecessor");
                return true;
              });
          }
        }
        require(two_bytes <= 2 * one_bytes, "two-sided suffix bytes bounded by twice ordinary FC");
      }
    }
  }

  void shared_boundary_policy() {
    std::vector<std::string> small_samples{"aaaa", "aaab", "aaac"};
    auto no_cuts = blob::build({}, small_samples);
    auto ordinary = blob::build({}, small_samples, 18, {}, borrowed_prefix_policy::ordinary);
    require(no_cuts.borrowed_policy() == borrowed_prefix_policy::shared_boundaries, "shared-cut policy is default");
    require(std::equal(no_cuts.borrowed().bytes().begin(), no_cuts.borrowed().bytes().end(),
                       ordinary.borrowed().bytes().begin(), ordinary.borrowed().bytes().end()),
            "no applicable shared cut preserves ordinary front coding");
    require(no_cuts.borrowed().view().encoded_at(2).shared == 3, "unused final sample need not be literal");

    std::vector<blob_record> native;
    for (unsigned i = 0; i != 13; ++i) native.push_back({"aaac" + numbered(i), i});
    native.push_back({"aamm", 100});
    native.push_back({"aazx", 101});
    std::vector<std::string> samples{"aaaa", "aaab", "aazz"};
    auto original = blob::build(native, samples);
    require(original.borrowed().view().encoded_at(1).shared == 2, "actual shared cut constrains predecessor");
    require(original.borrowed().view().encoded_at(2).shared == 2, "last sample without later cut keeps prefix");
    auto old_result = original.search_window("aamn", 1, front_anchor::complete("aamm"));
    require(old_result.borrowed_predecessor && old_result.borrowed_predecessor->prefix == "aaab",
            "shared-cut predecessor decodes from boundary");

    std::vector<std::string> shifted_samples{"a", "aaaa", "aaab", "aazz"};
    auto replacement = original.reindex(shifted_samples);
    require(&replacement.native() == &original.native(), "moving shared cuts preserves native allocation");
    require(replacement.borrowed().view().encoded_at(2).shared == 3, "reindex recomputes shifted cut prefix");
    require(original.borrowed().view().encoded_at(1).shared == 2, "snapshot keeps old cut prefix");
    auto new_result = replacement.search_window("aamn", 1, front_anchor::complete(native[12].key));
    require(new_result.borrowed_predecessor && new_result.borrowed_predecessor->prefix == "aaab",
            "shifted-cut predecessor decodes from new boundary");

    std::vector<blob_record> wide_gap;
    for (unsigned i = 0; i != 14; ++i) wide_gap.push_back({"aaac" + numbered(i), i});
    for (unsigned i = 0; i != 20; ++i) wide_gap.push_back({"aabb" + numbered(i), i + 100});
    auto several_cuts = blob::build(wide_gap, samples);
    require(several_cuts.borrowed().view().encoded_at(1).shared == 2,
            "rightmost applicable cut supplies tightest prefix ceiling");
    for (std::uint64_t group : {1, 2}) {
      auto const & boundary = wide_gap[static_cast<std::size_t>(15 * group - 2)].key;
      auto result = several_cuts.search_window(boundary, group, front_anchor::complete(boundary));
      require(result.native && result.borrowed_predecessor && result.borrowed_predecessor->prefix == "aaab",
              "one conservative predecessor serves every applicable cut");
    }
  }

  std::vector<std::string> sample_catalog(std::vector<blob_record> const & native,
                                           std::vector<std::string> const & borrowed) {
    std::vector<std::string> keys = borrowed;
    for (auto const & record : native) keys.push_back(record.key);
    std::sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) { return compare_keys(a, b) < 0; });
    std::vector<std::string> samples;
    for (std::size_t i = 0; i < keys.size(); i += 15) samples.push_back(keys[i]);
    return samples;
  }

  void cascade_lookup() {
    std::array<std::vector<blob_record>, 3> records;
    for (unsigned i = 0; i != 300; ++i) records[0].push_back({numbered(i * 3), i});
    for (unsigned i = 0; i != 31; ++i) {
      records[1].push_back({numbered(i * 19), i % 4 ? std::optional<std::uint64_t>(1000 + i) : std::nullopt});
    }
    for (unsigned i = 0; i != 7; ++i) records[2].push_back({numbered(i * 107), 2000 + i});
    std::array<std::vector<std::string>, 3> samples;
    samples[1] = sample_catalog(records[0], {});
    samples[2] = sample_catalog(records[1], samples[1]);
    for (auto policy : {borrowed_prefix_policy::shared_boundaries,
                        borrowed_prefix_policy::bidirectional, borrowed_prefix_policy::ordinary}) {
      std::array<blob, 3> levels{
        blob::build(records[0], {}, 18, {}, policy),
        blob::build(records[1], samples[1], 18, {}, policy),
        blob::build(records[2], samples[2], 18, {}, policy)};
      require(levels[2].virtual_size() <= 15, "cascade bootstrap fits one group");
      for (unsigned i = 0; i != 920; ++i) {
        std::string query = numbered(i);
        std::optional<std::optional<std::uint64_t>> expected;
        for (int level = 2; level >= 0 && !expected; --level) {
          for (auto const & record : records[level]) {
            if (record.key == query) { expected.emplace(record.value); break; }
          }
        }
        std::optional<std::optional<std::uint64_t>> actual;
        std::string prefix;
        front_anchor anchor;
        std::uint64_t group = 0;
        for (int level = 2; level >= 0; --level) {
          auto result = levels[level].search_window(query, group, anchor);
          if (result.native) { actual.emplace(result.native->value); break; }
          if (!result.borrowed_predecessor) break;
          auto const & next = *result.borrowed_predecessor;
          if (policy == borrowed_prefix_policy::ordinary && !next.has_context) {
            // Explicit slow reference fallback; ordinary mode has no bound on
            // this backward reconstruction. Conservative modes never use it.
            auto recovered = levels[level].borrowed().view().reconstruct_at(next.ordinal, query.size());
            prefix = recovered.prefix;
            anchor = {prefix, recovered.full_size};
          } else {
            require(next.has_context, "cascade carries complete query-relevant context");
            prefix = next.prefix;
            anchor = {prefix, next.full_size};
          }
          group = next.ordinal;
        }
        require(actual == expected, "three-level cascade lookup and tombstone oracle");
      }
    }
  }

  void index_repair_preserves_native() {
    std::vector<blob_record> native;
    std::vector<std::string> old_samples;
    std::vector<std::string> new_samples;
    for (unsigned i = 0; i != 60; ++i) {
      native.push_back({numbered(i * 2), i});
      old_samples.push_back(numbered(i * 2));
      new_samples.push_back(numbered(i * 2 + 1));
    }
    auto original = blob::build(native, old_samples);
    auto retained = original;
    std::vector<std::uint64_t> ceilings(new_samples.size(), 4);
    auto replacement = original.reindex(new_samples, ceilings);
    require(&replacement.native() == &original.native(), "index repair preserves native allocation");
    require(replacement.native().bytes().data() == retained.native().bytes().data(), "snapshot pins native bytes");
    for (std::size_t i = 0; i != old_samples.size(); ++i) {
      require(original.false_borrow(i) && retained.false_borrow(i), "old index remains valid");
      require(!replacement.false_borrow(i), "replacement false-borrow flags updated");
      require(replacement.borrowed().view().reconstruct_at(i).prefix == new_samples[i], "replacement conservative front code");
    }
    require(original.borrowed().view().reconstruct_at(17).prefix == old_samples[17], "old borrowed stream pinned");
  }

  void invalid_inputs() {
    std::array<front_record<0>, 2> unsorted{{{"b", {}}, {"a", {}}}};
    rejects([&] { front_array<0>::build(unsorted); });
    std::array<front_record<0>, 2> sorted{{{"a", {}}, {"ab", {}}}};
    rejects([&] { front_array<0>::build(sorted, {}, 2); });
    auto array = front_array<0>::build(sorted);
    rejects([&] { array.view().encoded_at(2); });
    rejects([&] { array.view().visit_window(1, 2, {}, 1, [](auto) { return true; }); });
    rejects([&] { array.view().visit_window(2, 1, {}, 1, [](auto) { return true; }); });
    std::array<std::byte, 1> truncated{std::byte{128}};
    front_view<0> corrupt(truncated, array.group_offsets().view(), 2);
    rejects([&] { corrupt.encoded_at(0); });
    std::array<std::byte, 11> overflow{};
    overflow.fill(std::byte{255});
    front_view<0> bad_varint(overflow, array.group_offsets().view(), 2);
    rejects([&] { bad_varint.encoded_at(0); });
    std::array<blob_record, 2> duplicate{{{"a", 1}, {"a", 2}}};
    rejects([&] { blob::build(duplicate); });
  }
}

int main() {
  try {
    front_roundtrip();
    surrogate_anchor_and_redundant_prefix();
    fixed_slots_and_long_prefixes();
    lpfc_span();
    blob_oracles();
    two_sided_borrowed_context();
    shared_boundary_policy();
    cascade_lookup();
    index_repair_preserves_native();
    invalid_inputs();
    std::cout << "Front-coded blob, partial anchor, LPFC, sparse offset and false-borrow oracles passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
 * \endlicense
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's storage front behavior.
 */
