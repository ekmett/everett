/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks physical framing across changes to registry value defaults.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/mapped_blob.h>
#include <everett/native_writer.h>
#include <everett/sections.h>
#if defined(EVERETT_REGISTRY_COMPAT_SQLITE)
#include <everett/sqlite_catalog.h>
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using namespace everett;
  using fixed = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<fixed_values<8>>>>>;
  using variable = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>>;
  using zero = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<fixed_values<0>>>>>;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid stored framing accepted");
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
      for (unsigned attempt = 0; attempt < 100; ++attempt) {
        auto path = std::filesystem::temp_directory_path() /
          ("everett-registry-compat-" + std::to_string(seed) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(path)) { root = std::move(path); return; }
      }
      throw std::runtime_error("cannot create compatibility fixture");
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  void write(std::filesystem::path const & path, std::span<std::byte const> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
  }
  void rehash(std::span<std::byte> bytes) {
    file_detail::put(bytes, 68, 4, file_detail::header_checksum(bytes));
  }
  std::vector<profile_record> rows(bool varied = false) {
    std::vector<profile_record> result;
    for (unsigned i = 0; i < 53; ++i) {
      char key[32]; std::snprintf(key, sizeof key, "shared/prefix/%04u", i);
      result.push_back({bit_string::from_bytes(key), bit_string::from_bytes(std::string(varied ? i % 13 : 8, char('a' + i % 26)))});
    }
    return result;
  }

  void envelopes() {
    file_header<fixed> source{file_kind::native_blob, 16, 2, 8};
    std::vector<std::byte> payload(16);
    auto encoded = encode_file(source, payload);
    auto widened = validate_file<variable>(encoded);
    require(widened.common_value_width == 8 && widened.policy_value_width == 8,
      "widening lost actual or declared value width");
    require(encode_file(widened, payload) == encoded, "reencoding changed stored writer declaration");
    auto narrowed = validate_file<zero>(encoded);
    require(narrowed.common_value_width == 8 && narrowed.policy_value_width == 8,
      "reader registry overrode stored framing");

    // Opaque envelopes can omit a common width while still declaring a fixed
    // writer layout. Keep both the declaration and its minimum payload size.
    source.common_value_width.reset();
    encoded = encode_file(source, payload);
    widened = validate_file<variable>(encoded);
    require(!widened.common_value_width && widened.policy_value_width == 8,
      "absent common width was turned into fixed framing");
    require(encode_file(widened, payload) == encoded, "opaque envelope declaration lost");
    auto bad = encoded;
    file_detail::put(bad, 56, 8, 3); rehash(bad);
    rejects([&] { (void)decode_file_header<variable>(bad); });
    widened.record_count = 3;
    rejects([&] { (void)encode_file_header(widened, 0); });

    auto mutate = [&](unsigned flags, std::uint64_t policy_width, std::uint64_t common) {
      auto bytes = encoded;
      file_detail::put(bytes, 12, 4, flags); file_detail::put(bytes, 32, 8, policy_width);
      file_detail::put(bytes, 40, 8, common); rehash(bytes);
      rejects([&] { (void)decode_file_header<variable>(bytes); });
    };
    mutate(0, 8, 0); // A variable writer's unused width must be zero.
    mutate(1, 8, 8); // A missing common field cannot retain nonzero bytes.
    mutate(3, 8, 7); // An explicit common width must agree with the writer.
    mutate(5, 8, 0); // Reserved flag bits remain invalid.

    file_header<zero> empty_values{file_kind::native_blob, 0, 2, 0};
    auto zero_bytes = encode_file(empty_values, std::span<std::byte const>{});
    auto zero_header = validate_file<variable>(zero_bytes);
    require(zero_header.common_value_width == 0 && zero_header.policy_value_width == 0,
      "present zero width became absent");
    empty_values.common_value_width.reset();
    auto absent_bytes = encode_file(empty_values, std::span<std::byte const>{});
    require(zero_bytes != absent_bytes && !validate_file<variable>(absent_bytes).common_value_width,
      "zero width and absent width were conflated");

    using bit = storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<>>>>;
    using other_k = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 7>;
    using other_w = storage_policy<everett::tip<everett::encoded_sort<everett::byte_encoding<>>>, 15, exponential_golomb<0>, 16>;
    rejects([&] { (void)decode_file_header<bit>(encoded); });
    rejects([&] { (void)decode_file_header<other_k>(encoded); });
    rejects([&] { (void)decode_file_header<other_w>(encoded); });
    using golomb_bits = storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<8>>>>, 15, golomb<3>>;
    auto bit_header = encode_file_header(file_header<golomb_bits>{}, 0);
    rejects([&] { (void)decode_file_header<bit>(bit_header); });
  }

  void profiles() {
    auto original = rows();
    auto array = profile_array<fixed>::build(original);
    auto metadata = array.metadata();
    profile_view<variable> view(array.bytes(), array.group_offsets().view(), metadata);
    view.validate_offset_metadata();
    profile_view<zero> narrower(array.bytes(), array.group_offsets().view(), metadata);
    narrower.validate_offset_metadata();
    auto reader = view.cursor();
    std::size_t at = 0;
    while (!reader.done()) {
      auto item = reader.peek();
      require(at < original.size() && bit_string::copy(item.key.prefix) == original[at].key &&
        bit_string::copy(item.value) == original[at].value, "cross-registry profile decode mismatch");
      ++at; reader.advance();
    }
    require(at == original.size(), "cross-registry profile count");
    auto wrong = metadata;
    wrong.policy_fixed_values = false; // Existing width eight is now noncanonical.
    rejects([&] { (void)profile_view<variable>(array.bytes(), array.group_offsets().view(), wrong); });
    wrong = metadata; wrong.common_value_width.reset();
    rejects([&] { (void)profile_view<variable>(array.bytes(), array.group_offsets().view(), wrong); });
    wrong = metadata; wrong.common_value_width = 7;
    rejects([&] { (void)profile_view<variable>(array.bytes(), array.group_offsets().view(), wrong); });

    // Variable framing remains explicit even under a currently fixed registry.
    auto varied = profile_array<variable>::build(rows(true));
    profile_view<fixed> varied_view(varied.bytes(), varied.group_offsets().view(), varied.metadata());
    require(!varied_view.metadata().common_value_width, "reader introduced a value stride");
    auto cursor = varied_view.cursor();
    for (unsigned i = 0; i < varied.size(); ++i) {
      require(cursor.peek().value.size() == (i % 13) * 8, "variable value parsed with registry width");
      cursor.advance();
    }
    require(cursor.done(), "variable stream did not terminate");
  }

  template <class P> blob_identity persist(std::filesystem::path const & root,
      std::vector<profile_record> const & original, unsigned first) {
    using blob = profile_blob<P>;
    auto native = std::make_shared<blob const>(blob::build(original));
    auto prepared = query_root<P>::build(native);
    std::vector<std::shared_ptr<blob const>> chain;
    std::vector<blob_identity> ids;
    for (auto current = prepared.head(); current; current = current->target()) {
      auto n = unsigned(chain.size()); chain.push_back(current);
      ids.push_back({id(first + 2 * n), id(first + 2 * n + 1)});
    }
    for (std::size_t i = chain.size(); i--;) {
      auto target = i + 1 < ids.size() ? std::optional(ids[i + 1]) : std::nullopt;
      write(root / object_path(ids[i].native, file_kind::native_blob),
        encode_native_sections(chain[i]->native()).materialize());
      write(root / object_path(ids[i].index, file_kind::fractional_index),
        encode_index_sections(*chain[i], ids[i].native, target).materialize());
    }
    return ids.front();
  }
  template <class P> void check_queries(std::filesystem::path const & root, blob_identity const & head,
      std::vector<profile_record> const & original) {
    auto query = open_mapped_query<P>(root, head);
    query.head()->scan();
    for (auto const & row : original) {
      auto cursor = query.cursor(row.key.view());
      unsigned count = 0;
      while (!cursor.done()) {
        cursor.step();
        if (!cursor.has_match()) continue;
        require(cursor.take_match().value == row.value, "cross-registry mapped query value");
        ++count;
      }
      require(count == 1, "cross-registry mapped query count");
    }
  }
  void mapped() {
    temporary directory;
    auto original = rows();
    auto head = persist<fixed>(directory.root, original, 1);
    check_queries<variable>(directory.root, head, original);
    check_queries<zero>(directory.root, head, original);
    auto varied = rows(true);
    auto other = persist<variable>(directory.root, varied, 20);
    check_queries<fixed>(directory.root, other, varied);
    using bit_fixed = storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<fixed_values<64>>>>>;
    using bit_variable = storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<>>>>;
    auto bit_head = persist<bit_fixed>(directory.root, original, 40);
    check_queries<bit_variable>(directory.root, bit_head, original);
    auto bit_other = persist<bit_variable>(directory.root, varied, 60);
    check_queries<bit_fixed>(directory.root, bit_other, varied);
  }

#if defined(EVERETT_REGISTRY_COMPAT_SQLITE) && (defined(__APPLE__) || defined(__linux__))
  std::vector<std::byte> catalog_policy(std::filesystem::path const & root,
      std::optional<std::vector<std::byte>> replacement = {}) {
    sqlite3 * db = nullptr;
    require(sqlite3_open((root / "catalog.sqlite3").c_str(), &db) == SQLITE_OK, "open catalog fixture");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> owner(db, sqlite3_close);
    catalog_detail::statement query(db, "SELECT policy FROM catalog_info WHERE singleton=1");
    require(query.row(), "missing catalog policy");
    auto result = query.blob(0);
    if (replacement) {
      // Deliberately corrupt the row through a separate test connection while
      // preserving the schema, so open must reject the descriptor itself.
      int enabled = 1;
      require(sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &enabled) == SQLITE_OK && !enabled,
        "disable fixture triggers");
      catalog_detail::statement update(db, "UPDATE catalog_info SET policy=? WHERE singleton=1");
      update.blob(1, *replacement); update.done();
    }
    return result;
  }
  void catalog() {
    temporary directory;
    { auto owner = sqlite_catalog<fixed>::create(directory.root, id(100)); }
    auto old_descriptor = catalog_policy(directory.root);
    auto widened = sqlite_catalog<variable>::open(directory.root);
    auto input = std::vector<profile_record>{{bit_string::from_bytes("a"), bit_string::from_bytes("x")},
      {bit_string::from_bytes("b"), bit_string::from_bytes("longer value")}};
    auto pair = profile_blob<variable>::build(input);
    blob_identity identity{id(101), id(102)};
    std::array outputs{catalog_object_reservation{identity.native, file_kind::native_blob},
      catalog_object_reservation{identity.index, file_kind::fractional_index}};
    object_attempt_id attempt(id(103).hex());
    widened.reserve("reserve", attempt, "builder", {}, outputs);
    auto native = encode_native_sections(pair.native());
    auto index = encode_index_sections(pair, identity.native);
    auto nr = native.seal(directory.root, identity.native, attempt);
    auto ir = index.seal(directory.root, identity.index, attempt);
    widened.record_sealed("native", nr); widened.record_sealed("index", ir);
    auto query = open_mapped_query<variable>(directory.root, identity);
    widened.register_chain("register", query, catalog_admission::scan);
    widened.save("save", "new-widths", identity);
    require(sqlite_catalog<fixed>::open(directory.root).find_save("new-widths") == identity,
      "fixed creation annotation blocked wider catalog writes");
    check_queries<fixed>(directory.root, identity, input);
    require(catalog_policy(directory.root) == old_descriptor, "opening mutated catalog policy");

    auto reject_descriptor = [&](std::vector<std::byte> wrong) {
      catalog_policy(directory.root, std::move(wrong));
      rejects([&] { (void)sqlite_catalog<variable>::open(directory.root); });
      catalog_policy(directory.root, old_descriptor);
    };
    auto wrong = old_descriptor; wrong.push_back(std::byte{0}); reject_descriptor(wrong);
    wrong = old_descriptor; wrong.pop_back(); reject_descriptor(wrong);
    wrong = old_descriptor; file_detail::put(wrong, 40, 8, 2); reject_descriptor(wrong);
    wrong = old_descriptor; file_detail::put(wrong, 40, 8, 0); reject_descriptor(wrong);
    for (unsigned field = 0; field < 5; ++field) {
      wrong = old_descriptor; wrong[field * 8] ^= std::byte{1}; reject_descriptor(wrong);
    }
    // A valid variable-width descriptor is compatible with a narrower reader;
    // those defaults do not reinterpret already-written native values.
    wrong = old_descriptor; file_detail::put(wrong, 40, 8, 0); file_detail::put(wrong, 48, 8, 0);
    catalog_policy(directory.root, wrong);
    require(sqlite_catalog<fixed>::open(directory.root).find_save("new-widths") == identity,
      "variable catalog annotation was treated as fixed framing");
    wrong = old_descriptor; file_detail::put(wrong, 48, 8, 0); catalog_policy(directory.root, wrong);
    require(sqlite_catalog<variable>::open(directory.root).find_save("new-widths") == identity,
      "fixed-zero catalog annotation was treated as absent");
  }
#endif
}

int main() try {
  envelopes(); profiles(); mapped();
#if defined(EVERETT_REGISTRY_COMPAT_SQLITE) && (defined(__APPLE__) || defined(__linux__))
  catalog();
#endif
  std::cout << "Stored widths remain file-local across registry changes\n";
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}
