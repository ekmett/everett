/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks mapped query chains against independent original-key oracles.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/mapped_blob.h>
#include <diet/native_writer.h>
#include <diet/sections.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  using namespace diet;

  template <class T> concept temporary_chunks = requires(T && value) { std::move(value).chunks(); };
  template <class T> concept temporary_view = requires(T && value) { std::move(value).view(); };

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid persisted section accepted");
  }

  // Read original key bits directly. Expected ordering, LCPs, matches and
  // merged origins do not depend on Diet's optimized comparison or codecs.
  bool original_bit(bit_view value, std::uint64_t i) {
    require(i < value.size(), "oracle bit outside input");
    auto at = value.offset() + i;
    return (std::to_integer<unsigned>(value.storage()[at / 8]) >> (7 - at % 8)) & 1;
  }
  struct comparison { std::uint64_t common; int order; };
  comparison compare_original(bit_view a, bit_view b) {
    auto common = std::min(a.size(), b.size());
    std::uint64_t i = 0;
    for (; i < common; ++i)
      if (original_bit(a, i) != original_bit(b, i))
        return {i, original_bit(a, i) ? 1 : -1};
    return {i, a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0};
  }
  bool same_bits(bit_view a, bit_view b) { return compare_original(a, b).order == 0; }
  void append_bit(bit_string & value, bool bit) {
    if (value.bit_size % 8 == 0) value.bytes.push_back(std::byte{0});
    if (bit) value.bytes.back() |= std::byte(1u << (7 - value.bit_size % 8));
    ++value.bit_size;
  }
  bit_string prefix(bit_view source, std::uint64_t count) {
    require(count <= source.size(), "oracle prefix outside input");
    bit_string result;
    for (std::uint64_t i = 0; i < count; ++i) append_bit(result, original_bit(source, i));
    return result;
  }
  bit_string displaced(bit_view source, unsigned shift) {
    bit_string result;
    for (unsigned i = 0; i < shift; ++i) append_bit(result, true);
    for (std::uint64_t i = 0; i < source.size(); ++i) append_bit(result, original_bit(source, i));
    for (unsigned i = 0; i < 7; ++i) append_bit(result, false);
    return result;
  }
  std::uint32_t original_crc(std::span<std::byte const> bytes) {
    std::uint32_t crc = ~std::uint32_t{0};
    for (auto byte : bytes) {
      crc ^= std::to_integer<unsigned>(byte);
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0);
    }
    return ~crc;
  }
  void put_le(std::span<std::byte> bytes, std::size_t at, unsigned width, std::uint64_t value) {
    require(width <= 8 && at <= bytes.size() && width <= bytes.size() - at, "oracle integer bounds");
    for (unsigned i = 0; i < width; ++i) bytes[at + i] = std::byte((value >> (8 * i)) & 255);
  }
  std::uint64_t get_le(std::span<std::byte const> bytes, std::size_t at, unsigned width) {
    require(width <= 8 && at <= bytes.size() && width <= bytes.size() - at, "oracle integer bounds");
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) result |= std::uint64_t(std::to_integer<unsigned>(bytes[at + i])) << (8 * i);
    return result;
  }

  template <class P> bit_string value_for(unsigned seed) {
    bit_string value;
    auto units = P::value_width.value_or(seed % 11);
    for (std::uint64_t i = 0; i < units * P::bits_per_unit; ++i)
      append_bit(value, ((seed * 37 + i * 19) >> (i % 7)) & 1);
    return value;
  }
  template <class P> std::vector<bit_string> keys() {
    std::vector<bit_string> result;
    for (std::string key : {"", "a", "aa", "ab", "b", "c", "z"})
      result.push_back(bit_string::from_bytes(key));
    result.push_back(bit_string::from_bytes(std::string(1, '\0')));
    result.push_back(bit_string::from_bytes(std::string(1, char(255))));
    for (unsigned i = 0; i < 43; ++i) {
      auto key = bit_string::from_bytes("shared/prefix/" + std::string(1, char(i)));
      if constexpr (P::unit == profile_unit::bit)
        for (unsigned j = 0; j < i % 7; ++j) append_bit(key, (i >> j) & 1);
      result.push_back(std::move(key));
    }
    if constexpr (P::unit == profile_unit::bit) {
      result.push_back(bit_string::from_bits("0"));
      result.push_back(bit_string::from_bits("01"));
      result.push_back(bit_string::from_bits("011"));
      result.push_back(bit_string::from_bits("0111"));
      result.push_back(bit_string::from_bits("1"));
    }
    std::sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      return compare_original(a.view(), b.view()).order < 0;
    });
    result.erase(std::unique(result.begin(), result.end(), [](auto const & a, auto const & b) {
      return same_bits(a.view(), b.view());
    }), result.end());
    return result;
  }

  using rows_type = std::vector<std::vector<profile_record>>;
  struct expected_match {
    std::size_t native_layer;
    std::uint64_t ordinal;
    bit_string value;
  };
  std::vector<expected_match> matches(rows_type const & rows, bit_view query) {
    std::vector<expected_match> result;
    for (std::size_t layer = 0; layer < rows.size(); ++layer)
      for (std::size_t i = 0; i < rows[layer].size(); ++i)
        if (same_bits(rows[layer][i].key.view(), query))
          result.push_back({layer, std::uint64_t(i), rows[layer][i].value});
    return result;
  }
  struct occurrence { bit_string key; bool borrowed; std::uint64_t ordinal; };
  struct expected_index {
    std::vector<bit_string> borrowed;
    std::vector<occurrence> merged;
    std::vector<std::uint64_t> classes, cuts;
    std::vector<bool> false_borrows;
  };
  template <class P> expected_index index_oracle(std::span<profile_record const> native,
      std::span<occurrence const> target) {
    expected_index result;
    for (std::size_t i = 0; i < target.size(); i += P::group_size)
      result.borrowed.push_back(target[i].key);
    for (auto const & key : result.borrowed) {
      bool is_false = false;
      for (auto const & row : native) is_false |= same_bits(row.key.view(), key.view());
      result.false_borrows.push_back(is_false);
    }
    std::size_t n = 0, b = 0;
    while (n < native.size() || b < result.borrowed.size()) {
      bool take_borrowed = b < result.borrowed.size() &&
        (n == native.size() || compare_original(result.borrowed[b].view(), native[n].key.view()).order < 0);
      auto const & key = take_borrowed ? result.borrowed[b] : native[n].key;
      if (result.merged.size() % P::group_size == 0) {
        result.classes.push_back(0);
        result.cuts.push_back(b ? compare_original(result.borrowed[b - 1].view(), key.view()).common : 0);
      }
      if (take_borrowed) {
        ++result.classes.back();
        result.merged.push_back({key, true, b++});
      } else result.merged.push_back({key, false, n++});
    }
    return result;
  }

  template <class P> struct fixture {
    rows_type rows;
    std::vector<bit_string> queries;
    std::vector<expected_index> indexes;
    std::shared_ptr<profile_blob<P> const> original;
    query_root<P> root;

    fixture() : rows(make_rows()), queries(keys<P>()), indexes(rows.size()), original(build()), root(query_root<P>::build(original)) {
      queries.push_back(bit_string::from_bytes("not-present"));
      for (std::size_t i = 0, end = queries.size(); i < end; i += 7) {
        auto units = queries[i].bit_size / P::bits_per_unit;
        if (units) queries.push_back(prefix(queries[i].view(), (units - 1) * P::bits_per_unit));
      }
    }
    static rows_type make_rows() {
      auto source = keys<P>();
      rows_type result(4);
      for (std::size_t i = 0; i < source.size(); ++i) {
        if (i % 2 == 0) result[0].push_back({source[i], value_for<P>(unsigned(i + 1))});
        if (i % 3 != 1) result[2].push_back({source[i], value_for<P>(unsigned(i + 23))});
        result[3].push_back({source[i], value_for<P>(unsigned(i + 47))});
      }
      return result; // Layer one deliberately has only borrowed occurrences.
    }
    std::shared_ptr<profile_blob<P> const> build() {
      indexes.back() = index_oracle<P>(rows.back(), {});
      for (std::size_t i = rows.size() - 1; i; --i)
        indexes[i - 1] = index_oracle<P>(rows[i - 1], indexes[i].merged);
      auto tail = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(rows.back()));
      std::vector<std::shared_ptr<profile_blob<P> const>> stages;
      for (std::size_t i = rows.size() - 1; i; --i)
        stages.push_back(std::make_shared<profile_blob<P> const>(profile_blob<P>::build(rows[i - 1])));
      index_pipeline<P> pipeline(tail, std::move(stages));
      while (!pipeline.done()) require(pipeline.step(17) != 0, "fixture pipeline stalled");
      return pipeline.finish();
    }
  };

  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
#if defined(__unix__) || defined(__APPLE__)
      auto pattern = (std::filesystem::temp_directory_path() / "diet-mapped-blob-XXXXXX").string();
      auto result = ::mkdtemp(pattern.data());
      if (!result) throw std::system_error(errno, std::generic_category(), "create mapped blob fixture");
      path = result;
#else
      for (unsigned i = 0; i < 10000; ++i) {
        auto candidate = std::filesystem::temp_directory_path() / ("diet-mapped-blob-" + std::to_string(i));
        if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
      }
      throw std::runtime_error("cannot reserve mapped blob fixture directory");
#endif
    }
    ~temporary_directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  object_id id(unsigned n) {
    char value[33];
    std::snprintf(value, sizeof value, "abcdef0123456789abcdef01%08x", n);
    return object_id(value);
  }
  void write_bytes(std::filesystem::path const & path, std::span<std::byte const> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
    require(bool(output), "cannot write mapped blob fixture");
  }
  template <class P> struct persisted_fixture {
    using pair_type = std::shared_ptr<mapped_blob<P> const>;
    temporary_directory directory;
    fixture<P> source;
    std::vector<std::shared_ptr<profile_blob<P> const>> original;
    std::vector<blob_identity> identities;
    std::vector<std::vector<std::byte>> native_bytes, index_bytes;
    std::vector<std::shared_ptr<mapped_native<P> const>> natives;
    std::vector<std::shared_ptr<mapped_index<P> const>> indexes;
    std::vector<pair_type> pairs;

    persisted_fixture() {
      for (auto current = source.root.head(); current; current = current->target()) original.push_back(current);
      for (std::size_t i = 0; i < original.size(); ++i) {
        auto native_id = id(unsigned(2 * i + 1));
        for (std::size_t j = 0; j < i; ++j)
          if (&original[j]->native() == &original[i]->native()) { native_id = identities[j].native; break; }
        identities.push_back({std::move(native_id), id(unsigned(2 * i + 2))});
      }
      for (std::size_t i = 0; i < original.size(); ++i) {
        auto native = encode_native_sections(original[i]->native());
        auto moved_native = std::move(native);
        rejects([&] { (void)native.header(); });
        rejects([&] { (void)native.chunks(); });
        rejects([&] { (void)native.materialize(); });
        auto native_chunks = moved_native.chunks();
        auto payload = original[i]->native().bytes();
        if (!payload.empty())
          require(std::any_of(native_chunks.begin(), native_chunks.end(), [&](auto part) {
            return part.data() == payload.data() && part.size() == payload.size();
          }), "native serializer copied the source payload");
        native_bytes.push_back(moved_native.materialize());
        auto target = i + 1 < identities.size() ? std::optional<blob_identity>(identities[i + 1]) : std::nullopt;
        auto index = encode_index_sections(*original[i], identities[i].native, target);
        auto moved_index = std::move(index);
        rejects([&] { (void)index.header(); });
        rejects([&] { (void)index.chunks(); });
        auto index_chunks = moved_index.chunks();
        payload = original[i]->borrowed().bytes();
        if (!payload.empty())
          require(std::any_of(index_chunks.begin(), index_chunks.end(), [&](auto part) {
            return part.data() == payload.data() && part.size() == payload.size();
          }), "index serializer copied the source payload");
        index_bytes.push_back(moved_index.materialize());
        auto native_path = directory.path / object_path(identities[i].native, file_kind::native_blob);
        auto index_path = directory.path / object_path(identities[i].index, file_kind::fractional_index);
        std::size_t previous_native = i;
        for (std::size_t j = 0; j < i; ++j)
          if (identities[j].native == identities[i].native) { previous_native = j; break; }
        if (previous_native == i) write_bytes(native_path, native_bytes.back());
        else require(native_bytes[previous_native] == native_bytes.back(), "shared native identity changed bytes");
        write_bytes(index_path, index_bytes.back());
        if (previous_native == i)
          natives.push_back(std::make_shared<mapped_native<P> const>(mapped_native<P>::open(native_path)));
        else natives.push_back(natives[previous_native]);
        indexes.push_back(std::make_shared<mapped_index<P> const>(mapped_index<P>::open(index_path)));
      }
      pairs.resize(original.size());
      for (std::size_t i = original.size(); i--;) {
        auto target = i + 1 < pairs.size() ? pairs[i + 1] : pair_type{};
        pairs[i] = mapped_blob<P>::bind(identities[i], natives[i], indexes[i], target);
      }
    }
    std::size_t routing_layers() const { return original.size() - source.rows.size(); }
    void check_sections() const {
      for (std::size_t i = 0; i < pairs.size(); ++i) {
        natives[i]->scan();
        indexes[i]->scan();
        pairs[i]->scan();
        auto native = natives[i]->view();
        auto borrowed = indexes[i]->borrowed();
        require(native.bytes().data() == natives[i]->section(0).data(), "mapped native stream copied");
        require(borrowed.bytes().data() == indexes[i]->section(0).data(), "mapped borrowed stream copied");
        auto check_offsets = [&](auto view, auto const & owner) {
          auto offsets = view.group_offsets();
          require(offsets.low_words().bytes().data() == owner.section(1).data(), "mapped EF low section copied");
          require(offsets.high_words().bytes().data() == owner.section(2).data(), "mapped EF high section copied");
          require(offsets.samples().bytes().data() == owner.section(3).data(), "mapped EF samples copied");
          require(offsets.sparse_words().bytes().data() == owner.section(4).data(), "mapped EF sparse section copied");
        };
        check_offsets(native, *natives[i]);
        check_offsets(borrowed, *indexes[i]);
        auto rank = indexes[i]->interleave();
        require(rank.class_words().bytes().data() == indexes[i]->section(5).data(), "mapped rank classes copied");
        require(rank.checkpoint_words().bytes().data() == indexes[i]->section(6).data(), "mapped rank checkpoints copied");
        require(indexes[i]->false_borrow_bits().data() == indexes[i]->section(7).data(), "mapped false flags copied");
        require(indexes[i]->cut_lcps().bytes().data() == indexes[i]->section(8).data(), "mapped cut LCPs copied");
        if (i < routing_layers()) continue;
        auto const & expected = source.indexes[i - routing_layers()];
        require(rank.group_count() == expected.classes.size() && indexes[i]->cut_lcps().size() == expected.cuts.size(),
                "persisted group count mismatch");
        std::uint64_t prefix_count = 0;
        for (std::size_t group = 0; group < expected.classes.size(); ++group) {
          require(rank.rank(group) == prefix_count && rank.class_at(group) == expected.classes[group],
                  "persisted rank disagrees with independent merge");
          require(indexes[i]->cut_lcps()[group] == expected.cuts[group], "persisted cut LCP mismatch");
          prefix_count += expected.classes[group];
        }
        require(rank.count() == expected.borrowed.size(), "persisted final rank mismatch");
        auto flags = indexes[i]->false_borrow_bits();
        for (std::size_t b = 0; b < expected.false_borrows.size(); ++b)
          require(bool((std::to_integer<unsigned>(flags[b / 8]) >> (b % 8)) & 1) == expected.false_borrows[b],
                  "persisted false-borrow mismatch");
        std::size_t ordinal = 0;
        borrowed.visit_all([&](auto item) {
          require(ordinal < expected.borrowed.size() && same_bits(item.key.prefix, expected.borrowed[ordinal].view()),
                  "persisted borrowed key mismatch");
          ++ordinal;
          return true;
        });
        require(ordinal == expected.borrowed.size(), "persisted borrowed length mismatch");
      }
    }
    template <class Cursor> void check_cursor(Cursor & cursor, bit_view query) const {
      auto expected = matches(source.rows, query);
      std::size_t found = 0, work = 0;
      while (!cursor.done()) {
        require(cursor.step(0) == 0, "zero mapped query budget");
        if (cursor.has_match()) {
          require(cursor.step(3) == 0, "mapped pending match lost backpressure");
          auto match = cursor.take_match();
          require(found < expected.size(), "mapped query produced extra match");
          auto const & wanted = expected[found++];
          require(match.source->identity() == identities[routing_layers() + wanted.native_layer] && match.ordinal == wanted.ordinal &&
                  same_bits(match.value.view(), wanted.value.view()), "mapped query differs from original-key oracle");
        } else {
          auto visited = cursor.step(1);
          require(visited == 1 && ++work <= pairs.size(), "mapped query failed bounded progress");
        }
      }
      require(found == expected.size(), "mapped query missed a native layer");
    }
    void check_queries() const {
      auto root = query_root<P, mapped_blob<P>>::adopt_prepared(pairs.front());
      auto reopened = open_mapped_query<P>(directory.path, identities.front());
      std::vector<std::shared_ptr<mapped_blob<P> const>> disk_pairs;
      for (auto pair = reopened.head(); pair; pair = pair->target()) disk_pairs.push_back(pair);
      require(disk_pairs.size() == pairs.size(), "metadata open lost a chain link");
      for (std::size_t i = 0; i < disk_pairs.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
          if (identities[i].native == identities[j].native)
            require(disk_pairs[i]->native_object() == disk_pairs[j]->native_object(), "metadata open duplicated shared native owner");
      for (auto const & query : source.queries) {
        auto shifted = displaced(query.view(), 3);
        auto view = shifted.view().subview(3, query.bit_size);
        auto cursor = root.cursor(view);
        shifted = {}; // A cursor owns its query, independent of its input view.
        check_cursor(cursor, query.view());
        auto disk_cursor = reopened.cursor(query.view());
        check_cursor(disk_cursor, query.view());
      }
    }
  };

  constexpr std::size_t envelope_bytes = 96;
  void repair_crc(std::vector<std::byte> & bytes) {
    require(bytes.size() >= envelope_bytes, "CRC repair needs an envelope");
    put_le(bytes, 64, 4, original_crc(std::span<std::byte const>(bytes).subspan(envelope_bytes)));
    put_le(bytes, 68, 4, 0);
    put_le(bytes, 68, 4, original_crc(std::span<std::byte const>(bytes).first(envelope_bytes)));
  }
  std::pair<std::size_t, std::size_t> section_range(std::span<std::byte const> bytes,
      bool index, std::size_t section) {
    auto at = envelope_bytes + (index ? section_detail::index_descriptor_offset : section_detail::native_descriptor_offset) +
              section * section_detail::descriptor_bytes;
    auto offset = get_le(bytes, at, 8), length = get_le(bytes, at + 8, 8);
    require(offset <= bytes.size() - envelope_bytes && length <= bytes.size() - envelope_bytes - offset,
            "fixture section outside container");
    return {envelope_bytes + static_cast<std::size_t>(offset), static_cast<std::size_t>(length)};
  }
  template <class P, class F> void with_native(std::vector<std::byte> const & bytes, F && action,
      std::size_t displacement = 0) {
    temporary_directory directory;
    auto path = directory.path / "fixture.kv";
    std::vector<std::byte> physical(displacement, std::byte{0xa5});
    physical.insert(physical.end(), bytes.begin(), bytes.end());
    write_bytes(path, physical);
    auto mapping = mapped_file::open(path);
    action(mapped_native<P>::from_slice(mapping.slice(displacement, bytes.size())));
  }
  template <class P, class F> void with_index(std::vector<std::byte> const & bytes, F && action,
      std::size_t displacement = 0) {
    temporary_directory directory;
    auto path = directory.path / "fixture.index";
    std::vector<std::byte> physical(displacement, std::byte{0xa5});
    physical.insert(physical.end(), bytes.begin(), bytes.end());
    write_bytes(path, physical);
    auto mapping = mapped_file::open(path);
    action(mapped_index<P>::from_slice(mapping.slice(displacement, bytes.size())));
  }

  template <class P> void shape_tests(persisted_fixture<P> const & fixture) {
    auto layer = fixture.routing_layers();
    auto const & native = fixture.native_bytes[layer];
    auto const & index = fixture.index_bytes[layer];
    {
      auto native_id = fixture.identities[layer].native;
      auto kept = std::move(native_id);
      require(kept == fixture.identities[layer].native, "object identity move changed destination");
      // The standard permits a moved-from string to retain its value. If the
      // move leaves a noncanonical identity, serialization must reject it.
      if (native_id.hex().size() != 32)
        rejects([&] { (void)encode_index_sections(*fixture.original[layer], native_id, fixture.identities[layer + 1]); });
      for (bool native_part : {false, true}) {
        auto target = fixture.identities[layer + 1];
        auto & part = native_part ? target.native : target.index;
        auto saved = std::move(part);
        require(saved == (native_part ? fixture.identities[layer + 1].native : fixture.identities[layer + 1].index),
                "target identity move changed destination");
        if (part.hex().size() != 32)
          rejects([&] { (void)encode_index_sections(*fixture.original[layer], fixture.identities[layer].native, target); });
      }
    }
    rejects([&] { with_native<P>(index, [](auto) {}); });
    rejects([&] { with_index<P>(native, [](auto) {}); });
    using wrong_width = storage_policy<typename P::registry_type, P::group_size, typename P::backspace_encoding, P::codec_block_size + 1>;
    rejects([&] { with_native<wrong_width>(native, [](auto) {}); });
    rejects([&] { with_index<wrong_width>(index, [](auto) {}); });
    for (auto shift : {1u, 3u, 7u}) {
      with_native<P>(native, [&](auto mapped) {
        mapped.scan();
        require(mapped.view().bytes().data() == mapped.section(0).data(), "unaligned native view copied");
      }, shift);
      with_index<P>(index, [&](auto mapped) { mapped.scan(); }, shift);
    }
    with_native<P>(native, [](auto mapped) {
      rejects([&] { (void)mapped.section(5); });
      rejects([&] { (void)mapped.view().encoded_at(mapped.size()); });
      { auto moved = std::move(mapped); moved.scan(); }
      rejects([&] { (void)mapped.view(); });
      rejects([&] { (void)mapped.section(0); });
    });
    with_index<P>(index, [](auto mapped) {
      rejects([&] { (void)mapped.section(9); });
      rejects([&] { (void)mapped.interleave().class_at(mapped.interleave().group_count()); });
      { auto moved = std::move(mapped); moved.scan(); }
      rejects([&] { (void)mapped.borrowed(); });
      rejects([&] { (void)mapped.interleave(); });
      rejects([&] { (void)mapped.cut_lcps(); });
    });
    for (bool is_index : {false, true}) {
      auto const & encoded = is_index ? index : native;
      auto open = [&](auto const & bytes) {
        if (is_index) with_index<P>(bytes, [](auto) {});
        else with_native<P>(bytes, [](auto) {});
      };
      auto table = envelope_bytes + (is_index ? section_detail::index_descriptor_offset : section_detail::native_descriptor_offset);
      auto count = is_index ? 9u : 5u;
      require(get_le(encoded, envelope_bytes + 4, 2) == 2 &&
              encoded[envelope_bytes + 3] == std::byte{'2'}, "mapped profile format version");
      {
        auto old_version = encoded;
        put_le(old_version, envelope_bytes + 4, 2, 1);
        repair_crc(old_version);
        rejects([&] { open(old_version); });
        auto old_magic = encoded;
        old_magic[envelope_bytes + 3] = std::byte{'1'};
        repair_crc(old_magic);
        rejects([&] { open(old_magic); });
      }
      for (unsigned s = 0; s < count; ++s) {
        for (unsigned field = 0; field < 2; ++field) {
          auto broken = encoded;
          put_le(broken, table + s * 16 + field * 8, 8, std::numeric_limits<std::uint64_t>::max());
          repair_crc(broken);
          rejects([&] { open(broken); });
        }
        auto broken = encoded;
        put_le(broken, table + s * 16, 8, 0); // Header/directory overlap.
        repair_crc(broken);
        rejects([&] { open(broken); });
        broken = encoded;
        auto original_offset = get_le(broken, table + s * 16, 8);
        put_le(broken, table + s * 16, 8, original_offset + 1);
        repair_crc(broken);
        rejects([&] { open(broken); });
        auto length = get_le(encoded, table + s * 16 + 8, 8);
        if (length) {
          broken = encoded;
          put_le(broken, table + s * 16 + 8, 8, length - 1);
          repair_crc(broken);
          rejects([&] { open(broken); });
        }
      }
      for (auto at : {envelope_bytes, envelope_bytes + 4, envelope_bytes + 6, envelope_bytes + 32}) {
        auto broken = encoded;
        broken[at] = std::byte{0xff};
        repair_crc(broken);
        rejects([&] { open(broken); });
      }
      for (auto relative : is_index ? std::vector<unsigned>{34, 39, 96, 111} : std::vector<unsigned>{33, 40, 47}) {
        auto broken = encoded;
        broken[envelope_bytes + relative] = std::byte{1};
        repair_crc(broken);
        rejects([&] { open(broken); });
      }
      for (auto length : {std::size_t{0}, std::size_t{95}, envelope_bytes,
            envelope_bytes + (is_index ? section_detail::index_directory_bytes : section_detail::native_directory_bytes) - 1,
            encoded.size() - 1}) {
        auto broken = encoded;
        broken.resize(length);
        rejects([&] { open(broken); });
      }
      auto trailing = encoded;
      trailing.push_back(std::byte{0});
      rejects([&] { open(trailing); });
    }
    // Target presence is one canonical bit, and an absent target has no IDs.
    auto broken = index;
    broken[envelope_bytes + 33] = std::byte{2};
    repair_crc(broken);
    rejects([&] { with_index<P>(broken, [](auto) {}); });
    broken = fixture.index_bytes.back();
    broken[envelope_bytes + 64] = std::byte{1};
    repair_crc(broken);
    rejects([&] { with_index<P>(broken, [](auto) {}); });
  }

  template <class P> void semantic_tests(persisted_fixture<P> const & fixture) {
    auto layer = fixture.routing_layers();
    auto const & native = fixture.native_bytes[layer];
    auto const & index = fixture.index_bytes[layer];
    {
      std::array records{profile_record{bit_string::from_bytes("aa"), value_for<P>(3)},
                         profile_record{bit_string::from_bytes("ab"), value_for<P>(4)}};
      std::array<std::uint64_t, 2> ceilings{0, 0};
      auto redundant = profile_array<P>::build(records, ceilings);
      require(redundant.view().encoded_at(1).retained == 0 &&
              compare_original(records[0].key.view(), records[1].key.view()).common / P::bits_per_unit != 0,
              "redundant FC fixture did not change retained prefix");
      auto encoded = encode_native_sections(redundant).materialize();
      with_native<P>(encoded, [&](auto mapped) {
        std::size_t ordinal = 0;
        mapped.view().visit_all([&](auto item) {
          require(same_bits(item.key.prefix, records[ordinal++].key.view()), "redundant FC changed decoded keys");
          return true;
        });
        require(ordinal == records.size(), "redundant FC lost keys");
        rejects([&] { mapped.scan(); });
      });
    }
    {
      std::array records{profile_record{bit_string::from_bytes(std::string(1, char(0x80))), value_for<P>(3)},
                         profile_record{bit_string::from_bytes(std::string(1, char(0x81))), value_for<P>(4)}};
      std::array<std::uint64_t, 2> ceilings{0, 0};
      auto source = profile_array<P>::build(records, ceilings);
      auto literal_bit = source.view().encoded_at(1).suffix.offset();
      auto broken = encode_native_sections(source).materialize();
      auto [first, length] = section_range(broken, false, 0);
      require(literal_bit / 8 < length, "reverse-order literal outside stream");
      broken[first + literal_bit / 8] &= std::byte(~(1u << (7 - literal_bit % 8)));
      repair_crc(broken);
      with_native<P>(broken, [&](auto mapped) {
        std::vector<bit_string> decoded;
        mapped.view().visit_all([&](auto item) { decoded.push_back(bit_string::copy(item.key.prefix)); return true; });
        require(decoded.size() == 2 && compare_original(decoded[0].view(), decoded[1].view()).order > 0,
                "reverse first-unit fixture did not reverse keys");
        rejects([&] { mapped.scan(); });
      });
    }
    if constexpr (P::unit == profile_unit::byte) {
      std::array records{profile_record{bit_string::from_bytes("ab"), value_for<P>(3)},
                         profile_record{bit_string::from_bytes("ab"), value_for<P>(4)}};
      auto source = profile_array<P>::build(records);
      auto second_header = source.view().encoded_at(0).next_offset;
      auto broken = encode_native_sections(source).materialize();
      auto [first, length] = section_range(broken, false, 0);
      require(second_header < length && broken[first + second_header] == std::byte{0}, "proper-prefix fixture header");
      broken[first + second_header] = std::byte{1}; // Retain one unit, literal remains empty.
      put_le(broken, envelope_bytes + 16, 8, 1);
      repair_crc(broken);
      with_native<P>(broken, [&](auto mapped) {
        std::vector<bit_string> decoded;
        mapped.view().visit_all([&](auto item) { decoded.push_back(bit_string::copy(item.key.prefix)); return true; });
        auto expected = bit_string::from_bytes("a");
        require(decoded.size() == 2 && same_bits(decoded[1].view(), expected.view()),
                "backspace/empty-literal fixture did not shorten key");
        rejects([&] { mapped.scan(); });
      });
    }
    // Every mutation has a valid body and header CRC. Shape construction must
    // not scan the changed navigation arrays; an explicit scan rejects them.
    for (auto section : {2u, 3u}) {
      auto broken = native;
      auto [first, length] = section_range(broken, false, section);
      require(length >= 8, "missing native semantic fixture section");
      put_le(broken, first, 8, section == 2 ? 0 : std::numeric_limits<std::uint64_t>::max());
      repair_crc(broken);
      with_native<P>(broken, [](auto mapped) { rejects([&] { mapped.scan(); }); });
    }
    {
      auto broken = native;
      auto terminal = get_le(broken, envelope_bytes + 16, 8);
      put_le(broken, envelope_bytes + 16, 8, terminal + 1);
      repair_crc(broken);
      with_native<P>(broken, [](auto mapped) { rejects([&] { mapped.scan(); }); });
    }
    if constexpr (P::unit == profile_unit::bit) {
      auto extent = get_le(native, envelope_bytes + 8, 8);
      if (extent % 8) {
        auto broken = native;
        auto [first, length] = section_range(broken, false, 0);
        require(length != 0, "partial bit extent has no bytes");
        broken[first + length - 1] |= std::byte{1};
        repair_crc(broken);
        with_native<P>(broken, [](auto mapped) { rejects([&] { mapped.scan(); }); });
      }
    }
    for (unsigned slot = 1; slot < 5; ++slot) {
      auto [previous, size] = section_range(native, false, slot - 1);
      auto [next, ignored] = section_range(native, false, slot);
      (void)ignored;
      if (next > previous + size) {
        auto broken = native;
        broken[previous + size] = std::byte{1};
        repair_crc(broken);
        with_native<P>(broken, [](auto mapped) { rejects([&] { mapped.scan(); }); });
        break;
      }
    }
    for (auto section : {5u, 6u, 7u, 8u}) {
      auto broken = index;
      auto [first, length] = section_range(broken, true, section);
      require(length != 0, "missing index semantic fixture section");
      if (section == 6) put_le(broken, first, 8, 1); // First rank checkpoint must be zero.
      else if (section == 8) put_le(broken, first, 8, 1); // No borrowed predecessor at cut zero.
      else broken[first] ^= std::byte{1};
      repair_crc(broken);
      with_index<P>(broken, [&](auto mapped) {
        auto index_owner = std::make_shared<mapped_index<P> const>(std::move(mapped));
        auto pair = mapped_blob<P>::bind(fixture.identities[layer], fixture.natives[layer], index_owner, fixture.pairs[layer + 1]);
        rejects([&] { pair->scan(); });
      });
    }
    // An equal-count replacement target does not become the encoded target
    // merely because its shape fits. Check either half of its exact identity.
    auto wrong_native = fixture.identities[layer];
    wrong_native.native = id(9001);
    rejects([&] { mapped_blob<P>::bind(wrong_native, fixture.natives[layer], fixture.indexes[layer], fixture.pairs[layer + 1]); });
    rejects([&] { mapped_blob<P>::bind(fixture.identities[layer], fixture.natives[layer], fixture.indexes[layer]); });
    for (auto at : {64u, 80u}) {
      auto broken = index;
      broken[envelope_bytes + at] ^= std::byte{1};
      repair_crc(broken);
      with_index<P>(broken, [&](auto mapped) {
        auto owner = std::make_shared<mapped_index<P> const>(std::move(mapped));
        rejects([&] { mapped_blob<P>::bind(fixture.identities[layer], fixture.natives[layer], owner, fixture.pairs[layer + 1]); });
      });
    }
    {
      // A fresh, internally correct index with the right sample count but the
      // wrong sample keys defeats shape/count-only validation. Prefixing every
      // borrowed key preserves its order and changes even the empty sample.
      std::vector<bit_string> wrong_samples;
      for (auto const & borrowed : fixture.source.indexes.front().borrowed) {
        bit_string wrong;
        for (unsigned bit = 0; bit < P::bits_per_unit; ++bit) append_bit(wrong, true);
        for (std::uint64_t bit = 0; bit < borrowed.bit_size; ++bit) append_bit(wrong, original_bit(borrowed.view(), bit));
        wrong_samples.push_back(std::move(wrong));
      }
      auto wrong = fixture.original[layer]->reindex(wrong_samples);
      auto encoded = encode_index_sections(wrong, fixture.identities[layer].native, fixture.identities[layer + 1]).materialize();
      with_index<P>(encoded, [&](auto mapped) {
        mapped.scan();
        auto owner = std::make_shared<mapped_index<P> const>(std::move(mapped));
        auto pair = mapped_blob<P>::bind(fixture.identities[layer], fixture.natives[layer], owner, fixture.pairs[layer + 1]);
        rejects([&] { pair->scan(); });
      });
    }
    if (fixture.pairs[layer]->virtual_size() > P::group_size)
      rejects([&] { query_root<P, mapped_blob<P>>::adopt_prepared(fixture.pairs[layer]); });

    for (bool cycle : {false, true}) {
      temporary_directory directory;
      for (std::size_t i = 0; i < fixture.identities.size(); ++i) {
        write_bytes(directory.path / object_path(fixture.identities[i].native, file_kind::native_blob), fixture.native_bytes[i]);
        if (!cycle && i == 1) continue; // Missing exact downstream index.
        auto bytes = fixture.index_bytes[i];
        if (cycle && i == 0) {
          bytes[envelope_bytes + 33] = std::byte{1};
          auto write_identity = [&](std::size_t at, object_id const & object) {
            auto digit = [](char c) { return unsigned(c <= '9' ? c - '0' : c - 'a' + 10); };
            for (unsigned j = 0; j < 16; ++j)
              bytes[envelope_bytes + at + j] = std::byte((digit(object.hex()[2 * j]) << 4) | digit(object.hex()[2 * j + 1]));
          };
          write_identity(64, fixture.identities.front().native);
          write_identity(80, fixture.identities.front().index);
          repair_crc(bytes);
        }
        write_bytes(directory.path / object_path(fixture.identities[i].index, file_kind::fractional_index), bytes);
      }
      rejects([&] { (void)open_mapped_query<P>(directory.path, fixture.identities.front()); });
    }
  }

  // CRC-valid mutations exercise the semantic reader, not just its checksum.
  // Accepted native changes must round-trip through a separate sequential
  // decoder/writer. A bound index has only one canonical encoding for its
  // unchanged native and target owners, so accepted index bytes must agree.
  template <class P> void mutation_tests(persisted_fixture<P> const & fixture) {
    auto layer = fixture.routing_layers();
    std::size_t accepted_native = 0, changed_native = 0, rejected_native = 0, rejected_index = 0;
    auto check_native = [&](std::vector<std::byte> const & bytes) {
      bool scanned = false;
      try {
        with_native<P>(bytes, [&](auto mapped) {
          mapped.scan();
          scanned = true;
          auto view = mapped.view();
          profile_native_writer<P> writer(view.metadata().common_value_width);
          auto cursor = view.cursor();
          std::uint64_t count = 0;
          while (!cursor.done()) {
            auto item = cursor.peek();
            writer.append(item.key.prefix, item.value);
            cursor.advance();
            require(++count <= view.size(), "accepted native mutation decoder exceeded count");
          }
          require(count == view.size(), "accepted native mutation decoder lost records");
          auto rebuilt = writer.finish();
          require(encode_native_sections(rebuilt).materialize() == bytes,
                  "accepted native mutation was not canonical round-trip output");
          ++accepted_native;
          changed_native += bytes != fixture.native_bytes[layer];
        });
      } catch (std::logic_error const &) {
        if (scanned) throw;
        ++rejected_native;
      } catch (std::overflow_error const &) {
        if (scanned) throw;
        ++rejected_native;
      }
    };
    auto check_index = [&](std::vector<std::byte> const & bytes) {
      bool scanned = false;
      try {
        with_index<P>(bytes, [&](auto mapped) {
          auto owner = std::make_shared<mapped_index<P> const>(std::move(mapped));
          auto pair = mapped_blob<P>::bind(fixture.identities[layer], fixture.natives[layer],
                                           std::move(owner), fixture.pairs[layer + 1]);
          pair->scan();
          scanned = true;
          require(bytes == fixture.index_bytes[layer], "accepted index mutation changed its exact pair");
        });
      } catch (std::logic_error const &) {
        if (scanned) throw;
        ++rejected_index;
      } catch (std::overflow_error const &) {
        if (scanned) throw;
        ++rejected_index;
      }
    };
    auto mutate = [&](auto const & original, auto && check) {
      check(original);
      // Visit every byte of the envelope, directory, payload and auxiliary
      // arrays. Repairing both CRCs prevents a checksum-only rejection.
      for (std::size_t at = 0; at != original.size(); ++at) {
        auto bytes = original;
        bytes[at] ^= std::byte(1u << (at % 8));
        repair_crc(bytes);
        check(bytes);
      }
      // Contiguous overwrites stress multi-byte counts and offsets. The
      // sequence is deterministic, including the all-zero/all-one cases.
      std::uint64_t random = 0xbaf317e5938726d1ull;
      for (unsigned trial = 0; trial != 128; ++trial) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        auto bytes = original;
        auto at = std::size_t(random % bytes.size());
        auto length = std::min<std::size_t>(1 + ((random >> 32) % 8), bytes.size() - at);
        auto fill = std::byte(trial & 1 ? 255 : 0);
        std::fill_n(bytes.begin() + at, length, fill);
        repair_crc(bytes);
        check(bytes);
      }
    };
    mutate(fixture.native_bytes[layer], check_native);
    mutate(fixture.index_bytes[layer], check_index);
    require(accepted_native > 1 && changed_native && rejected_native && rejected_index,
            "mutation fixture did not cover valid changes and semantic rejections");
    std::cout << "CRC-valid " << (P::unit == profile_unit::byte ? "byte" : "bit")
      << " mutations: " << changed_native << " changed native round trips, "
      << rejected_native << " native rejections, " << rejected_index << " index rejections\n";
  }

  template <class P> void empty_test() {
    temporary_directory directory;
    auto empty = profile_blob<P>::build({});
    blob_identity identity{id(6001), id(6002)};
    auto native = encode_native_sections(empty.native()).materialize();
    auto index = encode_index_sections(empty, identity.native).materialize();
    write_bytes(directory.path / object_path(identity.native, file_kind::native_blob), native);
    write_bytes(directory.path / object_path(identity.index, file_kind::fractional_index), index);
    auto root = open_mapped_query<P>(directory.path, identity);
    root.head()->scan();
    for (auto query : {bit_string{}, bit_string::from_bytes("missing")}) {
      auto cursor = root.cursor(query.view());
      require(cursor.done() && !cursor.has_match() && cursor.step(10) == 0, "empty mapped root was not empty");
    }
  }

#if defined(__unix__) || defined(__APPLE__)
  struct protected_pages {
    void * first;
    std::size_t length;
    protected_pages(void * first, std::size_t length) : first(first), length(length) {
      require(::mprotect(first, length, PROT_NONE) == 0, "cannot protect mapped fixture pages");
    }
    ~protected_pages() { if (::mprotect(first, length, PROT_READ) != 0) std::terminate(); }
  };
  template <class P> void guard_tests() {
    auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    auto large = bit_string::from_bytes("a" + std::string(4 * page, 'x'));
    if constexpr (P::unit == profile_unit::bit) { append_bit(large, true); append_bit(large, false); }
    std::array records{profile_record{large, value_for<P>(7)}};
    auto target = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(records));
    auto empty = std::make_shared<profile_blob<P> const>(profile_blob<P>::build({}));
    index_pipeline<P> pipeline(target, {empty});
    while (!pipeline.done()) require(pipeline.step(3) != 0, "guard pipeline stalled");
    auto head = pipeline.finish();
    std::array bodies{encode_native_sections(target->native()).materialize(),
      encode_index_sections(*head, id(3001), blob_identity{id(3002), id(3003)}).materialize()};
    temporary_directory directory;
    for (unsigned kind = 0; kind < 2; ++kind) {
      auto path = directory.path / (kind ? "guard.index" : "guard.kv");
      auto fixed = envelope_bytes + (kind ? section_detail::index_directory_bytes : section_detail::native_directory_bytes);
      require(fixed < page, "fixed section directory exceeds guard page");
      auto displacement = page - fixed;
      std::vector<std::byte> physical(displacement, std::byte{0xa5});
      physical.insert(physical.end(), bodies[kind].begin(), bodies[kind].end());
      write_bytes(path, physical);
      auto mapping = mapped_file::open(path);
      auto whole = mapping.slice(0, mapping.size());
      auto slice = mapping.slice(displacement, bodies[kind].size());
      auto bytes = slice.bytes();
      auto base = const_cast<std::byte *>(whole.bytes().data());
      require(bytes.size() > 2 * page && reinterpret_cast<std::uintptr_t>(base) % page == 0 &&
              reinterpret_cast<std::uintptr_t>(bytes.data() + fixed) % page == 0,
              "guard fixture lacks whole payload pages");
      auto rounded = ((whole.size() - 1) / page + 1) * page;
      {
        protected_pages guard(base + page, rounded - page);
        // Normal typed open must read only the envelope and fixed directory.
        // Every payload byte, including the first FC byte and all navigation
        // arrays, is inaccessible after the directory's exact page boundary.
        if (kind) {
          auto stored = mapped_index<P>::from_slice(slice);
          require(stored.borrowed().size() == 1, "guarded index metadata mismatch");
          (void)stored.interleave();
          (void)stored.cut_lcps();
        } else {
          auto stored = mapped_native<P>::from_slice(slice);
          require(stored.view().size() == 1, "guarded native metadata mismatch");
        }
      }
      {
        protected_pages guard(base, rounded);
        // Trusted envelope open itself remains zero-touch; typed section
        // access is a separate metadata operation and is not attempted here.
        auto trusted = file<P>::from_slice(slice, file_open_mode::trusted);
        require(trusted.body().size() == bytes.size() - envelope_bytes, "trusted slice metadata mismatch");
      }
      if (kind) mapped_index<P>::from_slice(slice).scan();
      else mapped_native<P>::from_slice(slice).scan();
    }
  }
#if defined(__APPLE__) || defined(__linux__)
  template <class P> void seal_test() {
    temporary_directory directory;
    std::array records{profile_record{bit_string::from_bytes("a"), value_for<P>(3)},
                       profile_record{bit_string::from_bytes("b"), value_for<P>(4)}};
    auto pair = profile_blob<P>::build(records);
    blob_identity identity{id(7001), id(7002)};
    auto native = encode_native_sections(pair.native());
    auto index = encode_index_sections(pair, identity.native);
    auto native_receipt = native.seal(directory.path, identity.native,
      object_attempt_id("1234567890abcdef1234567890abcdef"));
    auto index_receipt = index.seal(directory.path, identity.index,
      object_attempt_id("1234567890abcdef1234567890abcdee"));
    auto root = open_mapped_query<P>(directory.path, identity);
    root.head()->scan();
    auto cursor = root.cursor(records[1].key.view());
    require(cursor.step() == 1 && cursor.has_match(), "sealed mapped query missed key");
    auto match = cursor.take_match();
    require(match.ordinal == 1 && same_bits(match.value.view(), records[1].value.view()) && cursor.done(),
            "sealed mapped query differs from source");
    auto native_file = file<P>::open(native_receipt.path);
    require(native_file.header().extent == (native_receipt.bytes - envelope_bytes) * (8 / P::bits_per_unit),
            "outer envelope confused container bytes with inner FC units");
    require(index_receipt.path == directory.path / object_path(identity.index, file_kind::fractional_index),
            "sealed index path mismatch");
  }
#endif
#endif

  template <class P> void lifetime_test(persisted_fixture<P> & fixture) {
    auto const & query = fixture.source.rows.front().front().key;
    auto cursor = [&] {
      auto root = query_root<P, mapped_blob<P>>::adopt_prepared(fixture.pairs.front());
      return root.cursor(query.view());
    }();
    while (!cursor.has_match() && !cursor.done()) require(cursor.step() == 1, "lifetime query stalled");
    require(cursor.has_match(), "lifetime fixture has no pending match");
    auto copy = cursor;
    auto moved = std::move(copy);
    std::vector<mapped_blob<P> const *> addresses;
    std::vector<std::weak_ptr<mapped_blob<P> const>> weak;
    for (auto const & pair : fixture.pairs) { addresses.push_back(pair.get()); weak.push_back(pair); }
    auto routing = fixture.routing_layers();
    auto expected = matches(fixture.source.rows, query.view());
    fixture.natives.clear();
    fixture.indexes.clear();
    fixture.pairs.clear();
    require(std::filesystem::remove_all(fixture.directory.path) != 0, "lifetime fixture unlink failed");
    auto drain = [&](auto & current) {
      std::size_t count = 0, steps = 0;
      while (!current.done()) {
        if (current.has_match()) {
          auto match = current.take_match();
          require(count < expected.size(), "lifetime query extra match");
          auto const & wanted = expected[count++];
          require(match.source.get() == addresses[routing + wanted.native_layer] && match.ordinal == wanted.ordinal &&
                  same_bits(match.value.view(), wanted.value.view()), "lifetime query lost exact mapping/source");
        } else require(current.step() == 1 && ++steps <= addresses.size(), "lifetime query did not progress");
      }
      require(count == expected.size(), "lifetime query lost a match");
    };
    drain(moved);
    require(cursor.has_match(), "copied mapped cursor consumed original pending match");
    drain(cursor);
    require(std::all_of(weak.begin(), weak.end(), [](auto const & item) { return item.expired(); }),
            "finished mapped cursors retained discarded source mappings");
  }

  template <class P> void run_policy(bool exhaustive_shapes) {
    static_assert(!temporary_chunks<encoded_sections<P>>);
    static_assert(!temporary_view<mapped_native<P>>);
    persisted_fixture<P> fixture;
    fixture.check_sections();
    fixture.check_queries();
    if (exhaustive_shapes) {
      shape_tests(fixture);
      semantic_tests(fixture);
      mutation_tests(fixture);
    }
    if constexpr (P::fixed_width) {
      auto broken = fixture.native_bytes[fixture.routing_layers()];
      put_le(broken, 12, 4, get_le(broken, 12, 4) & ~std::uint64_t{2});
      put_le(broken, 40, 8, 0);
      repair_crc(broken);
      (void)decode_file_header<P>(broken); // Valid opaque envelope, invalid typed container.
      rejects([&] { with_native<P>(broken, [](auto) {}); });
    }
    lifetime_test(fixture);
  }
}

int main() {
  try {
    using byte_var = storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 3, exponential_golomb<0>, 16>;
    using bit_var = storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 7, exponential_golomb<0>, 15>;
    run_policy<byte_var>(true);
    run_policy<bit_var>(true);
    empty_test<byte_var>();
    empty_test<bit_var>();
    run_policy<storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<fixed_values<0>>>>, 15, exponential_golomb<0>, 16>>(false);
    run_policy<storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<fixed_values<5>>>>, 31, golomb<3>, 15>>(false);
#if defined(__unix__) || defined(__APPLE__)
    guard_tests<byte_var>();
    guard_tests<bit_var>();
#if defined(__APPLE__) || defined(__linux__)
    seal_test<byte_var>();
    seal_test<bit_var>();
#endif
#endif
    std::cout << "mapped blob tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
