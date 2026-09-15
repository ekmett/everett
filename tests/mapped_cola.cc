/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/mapped_cola.h>
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
    for (unsigned i = 0; i < 3 * P::group_size + 37; ++i) {
      auto key = bit_string::from_bytes("shared/prefix/" + std::string{char(i >> 8), char(i)});
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
  constexpr std::size_t envelope_bytes = 96, directory_bytes = 448, descriptor_offset = 160;
  void repair_crc(std::vector<std::byte> & bytes) {
    put_le(bytes, 64, 4, original_crc(std::span<std::byte const>(bytes).subspan(envelope_bytes)));
    put_le(bytes, 68, 4, 0);
    put_le(bytes, 68, 4, original_crc(std::span<std::byte const>(bytes).first(envelope_bytes)));
  }
  void put_id(std::span<std::byte> bytes, std::size_t at, object_id const & id) {
    auto digit = [](char c) { return unsigned(c <= '9' ? c - '0' : c - 'a' + 10); };
    for (unsigned i = 0; i != 16; ++i)
      bytes[at + i] = std::byte((digit(id.hex()[2 * i]) << 4) | digit(id.hex()[2 * i + 1]));
  }
  std::pair<std::size_t, std::size_t> section_range(std::span<std::byte const> bytes, unsigned section) {
    auto at = envelope_bytes + descriptor_offset + 16 * section;
    return {envelope_bytes + std::size_t(get_le(bytes, at, 8)), std::size_t(get_le(bytes, at + 8, 8))};
  }
  template <class P, class F> void with_index(std::vector<std::byte> const & bytes, F && action, std::size_t shift = 0) {
    temporary_directory directory;
    auto path = directory.path / "fixture.index";
    std::vector<std::byte> physical(shift, std::byte{0xa5});
    physical.insert(physical.end(), bytes.begin(), bytes.end());
    write_bytes(path, physical);
    auto mapping = mapped_file::open(path);
    action(mapped_cola_index<P>::from_slice(mapping.slice(shift, bytes.size())));
  }

  // Assemble IX03 independently from the documented field/section order. The
  // FC/rank arrays already have separate original-key oracles; this checks the
  // whole serialization, endian spelling, padding and envelope CRCs.
  template <class P> std::vector<std::byte> wire(cola_index<P> const & node,
      object_id const & native, std::optional<blob_identity> main, std::optional<object_id> side) {
    std::vector<std::byte> body(directory_bytes);
    for (unsigned i = 0; i != 4; ++i) body[i] = std::byte("IX03"[i]);
    put_le(body, 4, 2, 3); put_le(body, 6, 2, 18); put_le(body, 8, 8, node.virtual_size());
    put_id(body, 88, native);
    if (main) { body[82] = std::byte{1}; put_id(body, 104, main->native); put_id(body, 120, main->index); }
    if (side) { body[83] = std::byte{1}; put_id(body, 136, *side); }
    std::vector<std::vector<std::byte>> sections;
    auto words = [&](auto const & input) {
      std::vector<std::byte> bytes(input.size() * 8);
      for (std::size_t i = 0; i != input.size(); ++i) put_le(bytes, i * 8, 8, input[i]);
      sections.push_back(std::move(bytes));
    };
    std::uint64_t borrowed_count = 0;
    for (unsigned route = 0; route != 2; ++route) {
      auto const & profile = node.borrowed(route);
      auto const & offsets = profile.group_offsets();
      put_le(body, 16 + 8 * route, 8, profile.size());
      put_le(body, 32 + 8 * route, 8, profile.metadata().extent);
      put_le(body, 48 + 8 * route, 8, profile.metadata().terminal_key_units);
      put_le(body, 64 + 8 * route, 8, offsets.universe);
      put_le(body, 80 + route, 1, offsets.low_width);
      borrowed_count += profile.size();
      sections.emplace_back(profile.bytes().begin(), profile.bytes().end());
      words(offsets.low); words(offsets.high);
      std::vector<std::byte> samples(offsets.samples.size() * 16);
      for (std::size_t i = 0; i != offsets.samples.size(); ++i) {
        put_le(samples, i * 16, 8, offsets.samples[i].first);
        put_le(samples, i * 16 + 8, 8, offsets.samples[i].sparse);
      }
      sections.push_back(std::move(samples)); words(offsets.sparse);
    }
    for (unsigned route = 0; route != 2; ++route) {
      words(node.interleave(route).classes); words(node.interleave(route).checkpoints);
    }
    for (unsigned route = 0; route != 2; ++route) {
      auto flags = node.false_borrow_bits(route);
      sections.emplace_back(flags.begin(), flags.end());
    }
    for (unsigned route = 0; route != 2; ++route) words(node.cut_lcps(route));
    require(sections.size() == 18, "wire oracle section count");
    for (unsigned i = 0; i != sections.size(); ++i) {
      while (body.size() % 8) body.push_back(std::byte{0});
      put_le(body, descriptor_offset + 16 * i, 8, body.size());
      put_le(body, descriptor_offset + 16 * i + 8, 8, sections[i].size());
      body.insert(body.end(), sections[i].begin(), sections[i].end());
    }
    return encode_file(file_header<P>{file_kind::fractional_index, body.size() * (8 / P::bits_per_unit),
      borrowed_count, 0}, body);
  }

  template <class P> struct fixture {
    using owner = typename cola_index<P>::pair_type;
    using pair = typename mapped_cola_blob<P>::pair_type;
    temporary_directory directory;
    std::vector<bit_string> queries = keys<P>();
    std::array<std::vector<profile_record>, 3> rows;
    std::array<owner, 3> logical;
    std::vector<owner> original;
    std::vector<blob_identity> identities;
    std::vector<std::optional<object_id>> side_ids;
    std::vector<std::vector<std::byte>> native_bytes, index_bytes;
    std::vector<std::shared_ptr<mapped_native<P> const>> natives;
    std::vector<std::shared_ptr<mapped_cola_index<P> const>> indexes;
    std::vector<pair> pairs;

    fixture() {
      for (std::size_t i = 0; i != queries.size(); ++i) {
        rows[2].push_back({queries[i], value_for<P>(unsigned(i + 31))});
        if (i % 2 == 0) rows[1].push_back({queries[i], value_for<P>(unsigned(i + 61))});
        if (i % 3 != 1) rows[0].push_back({queries[i], value_for<P>(unsigned(i + 101))});
      }
      logical[2] = std::make_shared<cola_index<P> const>(cola_index<P>::build(rows[2]));
      logical[1] = std::make_shared<cola_index<P> const>(cola_index<P>::build(rows[1], logical[2], logical[2]->native_owner()));
      logical[0] = std::make_shared<cola_index<P> const>(cola_index<P>::build(rows[0], logical[1], logical[2]->native_owner()));
      auto prepared = cola_index<P>::prepare_root(logical[0], logical[1]->native_owner());
      for (auto current = prepared; current; current = current->main_target()) original.push_back(current);
      for (std::size_t i = 0; i != original.size(); ++i) {
        auto native = id(unsigned(2 * i + 1));
        for (std::size_t j = 0; j < i; ++j)
          if (original[j]->native_owner() == original[i]->native_owner()) native = identities[j].native;
        identities.push_back({native, id(unsigned(2 * i + 2))});
      }
      for (std::size_t i = 0; i != original.size(); ++i) {
        std::optional<object_id> side;
        if (auto target = original[i]->secondary_target())
          for (std::size_t j = 0; j != original.size(); ++j)
            if (original[j]->native_owner() == target) side = identities[j].native;
        require(bool(side) == bool(original[i]->secondary_target()), "fixture secondary is not a shared native");
        side_ids.push_back(side);
        auto main = i + 1 < identities.size() ? std::optional(identities[i + 1]) : std::nullopt;
        auto encoded = encode_cola_sections(*original[i], identities[i].native, main, side);
        auto moved = std::move(encoded);
        rejects([&] { encoded.chunks(); });
        rejects([&] { encoded.header(); });
        rejects([&] { encoded.materialize(); });
        auto chunks = moved.chunks();
        for (unsigned route = 0; route != 2; ++route) {
          auto bytes = original[i]->borrowed(route).bytes();
          if (!bytes.empty()) require(std::any_of(chunks.begin(), chunks.end(), [&](auto part) {
            return part.data() == bytes.data() && part.size() == bytes.size();
          }), "IX03 encoder copied an FC stream");
        }
        index_bytes.push_back(moved.materialize());
        require(index_bytes.back() == wire(*original[i], identities[i].native, main, side), "IX03 differs from independent wire assembly");
        moved.seal(directory.path, identities[i].index, object_attempt_id(id(unsigned(1000 + i)).hex()));
        auto native = encode_native_sections(original[i]->native());
        native_bytes.push_back(native.materialize());
        std::size_t previous = i;
        for (std::size_t j = 0; j < i; ++j) if (identities[j].native == identities[i].native) { previous = j; break; }
        if (previous == i) {
          native.seal(directory.path, identities[i].native, object_attempt_id(id(unsigned(2000 + i)).hex()));
          natives.push_back(std::make_shared<mapped_native<P> const>(mapped_native<P>::open(
            directory.path / object_path(identities[i].native, file_kind::native_blob))));
        } else natives.push_back(natives[previous]);
        indexes.push_back(std::make_shared<mapped_cola_index<P> const>(mapped_cola_index<P>::open(
          directory.path / object_path(identities[i].index, file_kind::fractional_index))));
      }
      pairs.resize(original.size());
      for (std::size_t i = original.size(); i--;) {
        auto side = secondary(i);
        pairs[i] = mapped_cola_blob<P>::bind(identities[i], natives[i], indexes[i],
          i + 1 < pairs.size() ? pairs[i + 1] : pair{}, side, side_ids[i]);
      }
      queries.push_back(bit_string::from_bytes("not-present"));
      queries.push_back(bit_string::from_bytes("shared/prefix/"));
      queries.push_back(prefix(queries.back().view(), queries.back().bit_size - P::bits_per_unit));
    }
    auto secondary(std::size_t i) const {
      std::shared_ptr<mapped_native<P> const> result;
      if (side_ids[i])
        for (std::size_t j = 0; j != identities.size(); ++j)
          if (identities[j].native == *side_ids[i]) result = natives[j];
      return result;
    }
    std::span<profile_record const> original_rows(profile_array<P> const * native) const {
      for (unsigned i = 0; i != 3; ++i) if (logical[i]->native_owner().get() == native) return rows[i];
      require(native->size() == 0, "unknown nonempty fixture native");
      return {};
    }
    std::size_t selected_layer() const {
      for (std::size_t i = 0; i != original.size(); ++i) if (original[i] == logical[0]) return i;
      throw std::logic_error("missing top logical layer");
    }
    struct match { std::size_t layer; bool side; std::uint64_t ordinal; bit_string value; };
    std::vector<match> expected(bit_view query) const {
      std::vector<match> result;
      for (std::size_t i = 0; i != original.size(); ++i)
        for (bool side : {false, true}) {
          auto native = side ? original[i]->secondary_target().get() : original[i]->native_owner().get();
          if (!native) continue;
          auto source = original_rows(native);
          for (std::size_t j = 0; j != source.size(); ++j)
            if (same_bits(source[j].key.view(), query)) result.push_back({i, side, j, source[j].value});
        }
      return result;
    }
    template <class Cursor> void check_cursor(Cursor & cursor, bit_view query) const {
      auto wanted = expected(query);
      std::size_t next = 0, visits = 0;
      while (!cursor.done()) {
        require(!cursor.step(0), "zero mapped COLA query budget");
        if (cursor.has_match()) {
          require(!cursor.step(7), "pending mapped COLA query lacks backpressure");
          auto found = cursor.take_match();
          require(next < wanted.size(), "mapped COLA query invented a match");
          auto const & expected = wanted[next++];
          require(found.source->identity() == identities[expected.layer] && found.secondary == expected.side &&
                  found.ordinal == expected.ordinal && same_bits(found.value.view(), expected.value.view()),
                  "mapped COLA query differs from original records/source identity");
        } else require(cursor.step(1) == 1 && ++visits <= original.size(), "mapped query revisited a node");
      }
      require(next == wanted.size(), "mapped COLA query missed a match");
    }
    void check() const {
      pairs.front()->scan();
      for (std::size_t i = 0; i != original.size(); ++i) {
        require(indexes[i]->native_id() == identities[i].native && indexes[i]->secondary_id() == side_ids[i], "mapped identity fields");
        auto expected_main = i + 1 < identities.size() ? std::optional(identities[i + 1]) : std::nullopt;
        require(indexes[i]->main_id() == expected_main, "mapped main exact pair field");
        for (unsigned route = 0; route != 2; ++route) {
          auto profile = indexes[i]->borrowed(route);
          require(profile.bytes().data() == indexes[i]->section(5 * route).data(), "mapped FC copied");
          auto offsets = profile.group_offsets();
          require(offsets.low_words().bytes().data() == indexes[i]->section(5 * route + 1).data() &&
                  offsets.high_words().bytes().data() == indexes[i]->section(5 * route + 2).data() &&
                  offsets.samples().bytes().data() == indexes[i]->section(5 * route + 3).data(), "mapped EF copied");
          auto rank = indexes[i]->interleave(route);
          require(rank.class_words().bytes().data() == indexes[i]->section(10 + 2 * route).data() &&
                  rank.checkpoint_words().bytes().data() == indexes[i]->section(11 + 2 * route).data() &&
                  indexes[i]->false_borrow_bits(route).data() == indexes[i]->section(14 + route).data() &&
                  indexes[i]->cut_lcps(route).bytes().data() == indexes[i]->section(16 + route).data(), "mapped navigation copied");
        }
      }
      auto root = mapped_cola_query_root<P>::adopt_prepared(pairs.front());
      auto disk = open_mapped_cola_query<P>(directory.path, identities.front());
      std::vector<pair> reopened;
      for (auto current = disk.head(); current; current = current->main_target()) reopened.push_back(current);
      require(reopened.size() == original.size(), "loader lost a main link");
      for (std::size_t i = 0; i != reopened.size(); ++i)
        for (std::size_t j = 0; j != reopened.size(); ++j) {
          if (identities[i].native == identities[j].native)
            require(reopened[i]->native_object() == reopened[j]->native_object(), "loader duplicated a shared native mapping");
          if (side_ids[i] && *side_ids[i] == identities[j].native)
            require(reopened[i]->secondary_target() == reopened[j]->native_object(), "loader did not share secondary native mapping");
        }
      for (auto const & query : queries) {
        auto temporary = displaced(query.view(), 5);
        auto cursor = root.cursor(temporary.view().subview(5, query.bit_size));
        temporary = {};
        check_cursor(cursor, query.view());
        auto disk_cursor = disk.cursor(query.view());
        check_cursor(disk_cursor, query.view());
      }
    }
  };

  template <class P> void corruptions(fixture<P> const & f) {
    auto layer = f.selected_layer();
    auto const & original = f.index_bytes[layer];
    auto open = [&](auto const & bytes) { with_index<P>(bytes, [](auto) {}); };
    auto scan_pair = [&](auto const & bytes) {
      with_index<P>(bytes, [&](auto index) {
        auto owner = std::make_shared<mapped_cola_index<P> const>(std::move(index));
        auto pair = mapped_cola_blob<P>::bind(f.identities[layer], f.natives[layer], owner,
          f.pairs[layer + 1], f.secondary(layer), f.side_ids[layer]);
        pair->scan();
      });
    };
    for (std::size_t offset : {std::size_t{0}, std::size_t{4}, std::size_t{6}, std::size_t{82},
                              std::size_t{83}, std::size_t{84}, std::size_t{152}}) {
      auto broken = original;
      broken[envelope_bytes + offset] = std::byte{255};
      repair_crc(broken);
      rejects([&] { open(broken); });
    }
    for (unsigned slot = 0; slot != 18; ++slot) {
      for (unsigned field = 0; field != 2; ++field) {
        auto broken = original;
        put_le(broken, envelope_bytes + descriptor_offset + 16 * slot + field * 8, 8,
               std::numeric_limits<std::uint64_t>::max());
        repair_crc(broken);
        rejects([&] { open(broken); });
      }
      auto broken = original;
      auto at = envelope_bytes + descriptor_offset + 16 * slot;
      put_le(broken, at, 8, get_le(broken, at, 8) + 1);
      repair_crc(broken);
      rejects([&] { open(broken); });
    }
    for (auto offset : {16u, 24u, 32u, 40u, 64u, 72u}) {
      auto broken = original;
      put_le(broken, envelope_bytes + offset, 8, std::numeric_limits<std::uint64_t>::max());
      repair_crc(broken);
      rejects([&] { open(broken); });
    }
    for (unsigned route = 0; route != 2; ++route) {
      for (auto slot : {2 + 5 * route, 11 + 2 * route, 14 + route, 16 + route}) {
        auto broken = original;
        auto [at, size] = section_range(broken, slot);
        require(size != 0, "semantic corruption needs nonempty section");
        broken[at] ^= std::byte{1};
        repair_crc(broken);
        rejects([&] { scan_pair(broken); });
      }
      auto count = f.indexes[layer]->borrowed(route).size();
      if (count % 8) {
        auto broken = original;
        auto [at, size] = section_range(broken, 14 + route);
        broken[at + size - 1] |= std::byte{128};
        repair_crc(broken);
        with_index<P>(broken, [&](auto mapped) { rejects([&] { mapped.scan(); }); });
      }
      auto broken = original;
      auto stream = f.indexes[layer]->borrowed(route);
      std::uint64_t ordinal = 0;
      auto record = stream.encoded_at(ordinal);
      while (record.suffix.empty() && ++ordinal < stream.size()) record = stream.encoded_at(ordinal);
      auto [at, size] = section_range(broken, 5 * route);
      require(size && !record.suffix.empty(), "borrowed mutation needs a literal");
      broken[at + record.suffix.offset() / 8] ^= std::byte(1u << (7 - record.suffix.offset() % 8));
      repair_crc(broken);
      rejects([&] { scan_pair(broken); });
    }
    auto missing_main = original;
    missing_main[envelope_bytes + 82] = std::byte{0};
    repair_crc(missing_main);
    rejects([&] { open(missing_main); });
    auto missing_side = original;
    missing_side[envelope_bytes + 83] = std::byte{0};
    repair_crc(missing_side);
    rejects([&] { open(missing_side); });

    for (auto shift : {1u, 3u, 7u}) with_index<P>(original, [&](auto mapped) {
      mapped.scan();
      require(mapped.borrowed(1).bytes().data() == mapped.section(5).data(), "unaligned IX03 copied FC");
    }, shift);
    with_index<P>(original, [&](auto mapped) {
      auto moved = std::move(mapped);
      require(!mapped.active() && moved.active(), "mapped IX03 move retained ownership");
      moved.scan();
      rejects([&] { mapped.borrowed(0); });
      rejects([&] { moved.section(18); });
      rejects([&] { moved.borrowed(2); });
    });
    using wrong_w = storage_policy<P::unit, typename P::value_layout, P::group_size,
      typename P::backspace_encoding, P::codec_block_size + 1>;
    rejects([&] { with_index<wrong_w>(original, [](auto) {}); });
    rejects([&] { with_index<P>(f.native_bytes[layer], [](auto) {}); });
    rejects([&] { encode_cola_sections(*f.original[layer], f.identities[layer].native); });
    rejects([&] { mapped_cola_blob<P>::bind({id(999), f.identities[layer].index}, f.natives[layer],
      f.indexes[layer], f.pairs[layer + 1], f.secondary(layer), f.side_ids[layer]); });
    rejects([&] { mapped_cola_blob<P>::bind(f.identities[layer], f.natives[layer],
      f.indexes[layer], {}, f.secondary(layer), f.side_ids[layer]); });
    rejects([&] { mapped_cola_blob<P>::bind(f.identities[layer], f.natives[layer],
      f.indexes[layer], f.pairs[layer + 1], f.secondary(layer), id(999)); });

    temporary_directory cyclic;
    for (std::size_t i = 0; i != f.original.size(); ++i) {
      write_bytes(cyclic.path / object_path(f.identities[i].native, file_kind::native_blob), f.native_bytes[i]);
      auto bytes = f.index_bytes[i];
      if (!i) {
        bytes[envelope_bytes + 82] = std::byte{1};
        put_id(bytes, envelope_bytes + 104, f.identities.front().native);
        put_id(bytes, envelope_bytes + 120, f.identities.front().index);
        repair_crc(bytes);
      }
      write_bytes(cyclic.path / object_path(f.identities[i].index, file_kind::fractional_index), bytes);
    }
    rejects([&] { open_mapped_cola_query<P>(cyclic.path, f.identities.front()); });
  }

  template <class P> void lifetime(fixture<P> & f) {
    auto query = f.queries.front();
    auto cursor = [&] {
      auto root = open_mapped_cola_query<P>(f.directory.path, f.identities.front());
      auto result = root.cursor(query.view());
      while (!result.has_match() && !result.done()) result.step();
      require(result.has_match(), "lifetime fixture has no pending match");
      return result;
    }();
    std::filesystem::remove_all(f.directory.path);
    f.pairs.clear(); f.indexes.clear(); f.natives.clear();
    // The root is already destroyed. Both cursor copies retain the exact
    // mapped sources after path names and independent handles disappear.
    auto copied = cursor;
    f.check_cursor(cursor, query.view());
    f.check_cursor(copied, query.view());
  }

#if defined(__unix__) || defined(__APPLE__)
  struct protected_pages {
    void * address;
    std::size_t size;
    protected_pages(void * p, std::size_t n) : address(p), size(n) {
      require(!::mprotect(address, size, PROT_NONE), "cannot protect IX03 payload pages");
    }
    ~protected_pages() { if (::mprotect(address, size, PROT_READ)) std::terminate(); }
  };
  template <class P> void metadata_only() {
    auto page_result = ::sysconf(_SC_PAGESIZE);
    require(page_result > 0, "page size unavailable");
    auto page = std::size_t(page_result);
    auto key = bit_string::from_bytes("a" + std::string(4 * page, 'p'));
    if constexpr (P::unit == profile_unit::bit) append_bit(key, true);
    std::array records{profile_record{key, value_for<P>(1)}};
    auto target = std::make_shared<cola_index<P> const>(cola_index<P>::build(records));
    auto head = cola_index<P>::build({}, target, target->native_owner());
    auto bytes = encode_cola_sections(head, id(900), blob_identity{id(901), id(902)}, id(901)).materialize();
    auto fixed = envelope_bytes + directory_bytes;
    require(fixed < page, "directory does not fit a page");
    auto displacement = page - fixed;
    std::vector<std::byte> physical(displacement, std::byte{0xa5});
    physical.insert(physical.end(), bytes.begin(), bytes.end());
    temporary_directory directory;
    auto path = directory.path / "guard.index";
    write_bytes(path, physical);
    auto mapping = mapped_file::open(path);
    auto whole = mapping.slice(0, mapping.size());
    auto source = mapping.slice(displacement, bytes.size());
    auto rounded = ((whole.size() + page - 1) / page) * page;
    auto base = const_cast<std::byte *>(whole.bytes().data());
    {
      protected_pages guard(base + page, rounded - page);
      auto index = mapped_cola_index<P>::from_slice(source);
      require(index.borrowed(0).size() == 1 && index.borrowed(1).size() == 1,
              "metadata-only open lost a borrowed stream");
      require(index.virtual_size() == 2 && index.main_id() && index.secondary_id(), "metadata-only identities/counts");
      (void)index.interleave(0); (void)index.interleave(1); (void)index.cut_lcps(1);
    }
    auto index = mapped_cola_index<P>::from_slice(source);
    index.scan();
  }
#endif

  template <class P> void empty_index() {
    auto empty = cola_index<P>::build({});
    auto encoder = encode_cola_sections(empty, id(800));
    auto replacement = encode_cola_sections(empty, id(801));
    replacement = std::move(encoder);
    rejects([&] { encoder.header(); });
    auto bytes = replacement.materialize();
    require(bytes == wire(empty, id(800), {}, {}), "empty IX03 wire differs");
    with_index<P>(bytes, [&](auto index) {
      index.scan();
      require(!index.virtual_size() && !index.borrowed(0).size() && !index.borrowed(1).size() &&
              !index.interleave(0).group_count() && !index.cut_lcps(1).size(), "empty IX03 metadata");
      auto moved = std::move(index);
      index = std::move(moved);
      require(index.active() && !moved.active(), "IX03 move assignment source state");
      index.scan();
    });
    for (auto offset : {104u, 136u}) {
      auto broken = bytes;
      broken[envelope_bytes + offset] = std::byte{1};
      repair_crc(broken);
      rejects([&] { with_index<P>(broken, [](auto) {}); });
    }
    rejects([&] { encode_cola_sections(empty, id(800), blob_identity{id(801), id(802)}); });
    rejects([&] { encode_cola_sections(empty, id(800), {}, id(803)); });
  }
  template <class P> concept temporary_encode = requires(cola_index<P> && index, object_id const & identity) {
    encode_cola_sections(std::move(index), identity);
  };

  template <class P> void matrix() {
    static_assert(!temporary_chunks<encoded_cola_sections<P>>);
    static_assert(!temporary_view<cola_index<P>>);
    static_assert(!temporary_encode<P>);
    empty_index<P>();
    fixture<P> f;
    f.check();
    corruptions(f);
    lifetime(f);
  }
}

int main() {
  try {
    matrix<storage_policy<profile_unit::byte, variable_values, 3, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::bit, fixed_values<13>, 3, golomb<3>, 16>>();
    matrix<storage_policy<profile_unit::byte, fixed_values<0>, 7, exponential_golomb<0>, 16>>();
    matrix<storage_policy<profile_unit::bit, variable_values, 7, exponential_golomb<2>, 15>>();
    matrix<storage_policy<profile_unit::byte, fixed_values<7>, 15, exponential_golomb<0>, 16>>();
    matrix<storage_policy<profile_unit::bit, variable_values, 15, golomb<17>, 15>>();
    matrix<storage_policy<profile_unit::byte, variable_values, 31, exponential_golomb<0>, 15>>();
    matrix<storage_policy<profile_unit::bit, fixed_values<0>, 31, exponential_golomb<1>, 16>>();
#if defined(__unix__) || defined(__APPLE__)
    metadata_only<storage_policy<profile_unit::byte, variable_values, 15>>();
    metadata_only<storage_policy<profile_unit::bit, fixed_values<13>, 7, golomb<3>, 16>>();
#endif
    std::cout << "mapped COLA tests passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
