/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/mapped_cola.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
namespace {
  using namespace everett;
  void require(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid mapped COLA operation accepted");
  }
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  struct temporary {
    std::filesystem::path root;
    unsigned next = 1;
    object_attempt_id attempt{std::string(32, 'f')};
    temporary() {
      auto text = (std::filesystem::temp_directory_path() / "everett-mapped-cola-builder-XXXXXX").string();
      auto result = ::mkdtemp(text.data());
      if (!result) throw std::runtime_error("mkdtemp"); root = result;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    object_id fresh() { return id(next++); }
  };
  template <class P> std::vector<profile_record> rows(unsigned count, unsigned salt, unsigned prefix) {
    std::vector<profile_record> result;
    for (unsigned i = 0; i < count; ++i) {
      char suffix[32]; std::snprintf(suffix, sizeof suffix, "%08u", i * 3 + salt % 3);
      auto key = bit_string::from_bytes(std::string(prefix, 'x') + suffix);
      if constexpr (P::unit == profile_unit::bit) {
        key.bytes.push_back(std::byte(i & 1 ? 0x80 : 0)); ++key.bit_size;
      }
      auto value = P::fixed_width ? bit_string{} : bit_string::from_bytes(std::to_string(salt) + ":" + std::to_string(i));
      result.push_back({std::move(key), std::move(value)});
    }
    return result;
  }
  template <class P> std::pair<object_id, std::shared_ptr<mapped_native<P> const>> persist_native(
      temporary & files, profile_array<P> const & source) {
    auto identity = files.fresh();
    auto encoded = encode_native_sections(source);
    encoded.seal(files.root, identity, files.attempt);
    return {identity, std::make_shared<mapped_native<P> const>(mapped_native<P>::open(files.root / object_path(identity, file_kind::native_blob)))};
  }
  template <class P> typename mapped_cola_blob<P>::pair_type persist(
      temporary & files, typename cola_index<P>::pair_type const & head) {
    std::vector<typename cola_index<P>::pair_type> chain;
    for (auto current = head; current; current = current->main_target()) chain.push_back(current);
    typename mapped_cola_blob<P>::pair_type target;
    for (std::size_t i = chain.size(); i-- > 0;) {
      auto native = persist_native(files, chain[i]->native());
      std::optional<object_id> side_id;
      std::shared_ptr<mapped_native<P> const> side;
      if (auto source = chain[i]->secondary_target()) {
        auto mapped = persist_native(files, *source); side_id = mapped.first; side = std::move(mapped.second);
      }
      auto index_id = files.fresh();
      auto encoded = encode_cola_sections(*chain[i], native.first,
        target ? std::optional{target->identity()} : std::nullopt, side_id);
      encoded.seal(files.root, index_id, files.attempt);
      auto index = std::make_shared<mapped_cola_index<P> const>(mapped_cola_index<P>::open(files.root / object_path(index_id, file_kind::fractional_index)));
      target = mapped_cola_blob<P>::bind({native.first, index_id}, std::move(native.second), std::move(index),
        std::move(target), std::move(side), side_id);
    }
    return target;
  }
  std::vector<std::byte> file_bytes(std::filesystem::path const & path) {
    auto file = mapped_file::open(path); auto slice = file.slice(0, file.size()); auto bytes = slice.bytes();
    return {bytes.begin(), bytes.end()};
  }
  template <class P> void matrix(unsigned count, unsigned prefix) {
    temporary files;
    using node = cola_index<P>;
    auto main_rows = rows<P>(count ? count + 8 : 0, 1, prefix);
    auto tail = std::make_shared<node const>(node::build(main_rows));
    auto middle_side = std::make_shared<profile_array<P> const>(profile_array<P>::build(rows<P>(count ? 13 : 0, 2, prefix)));
    auto main = std::make_shared<node const>(node::build(rows<P>(count ? 9 : 0, 3, prefix), tail, middle_side));
    auto native_rows = rows<P>(count, 4, prefix);
    auto side_rows = rows<P>(count ? count / 2 + 1 : 0, 5, prefix);
    auto native = std::make_shared<profile_array<P> const>(profile_array<P>::build(native_rows));
    auto side = std::make_shared<profile_array<P> const>(profile_array<P>::build(side_rows));
    auto expected = node::adopt_native(native, main, side);
    auto mapped_main = persist<P>(files, main);
    auto mapped_native = persist_native(files, *native), mapped_side = persist_native(files, *side);
    auto native_id = mapped_native.first, side_id = mapped_side.first;
    auto main_id = mapped_main->identity();
    auto native_path = files.root / object_path(native_id, file_kind::native_blob);
    auto original = file_bytes(native_path);
    auto native_address = mapped_native.second->view().bytes().data();
    std::weak_ptr<everett::mapped_native<P> const> native_pin = mapped_native.second, side_pin = mapped_side.second;
    std::weak_ptr<mapped_cola_blob<P> const> main_pin = mapped_main;
    mapped_cola_index_builder<P> builder(mapped_native.second, mapped_main, mapped_side.second);
    require(builder.step(0) == 0 && builder.size() == 0, "zero budget changed mapped builder");
    if (!builder.done()) builder.step(1);
    auto moved = std::move(builder);
    rejects([&] { builder.step(1); });
    mapped_native.second.reset(); mapped_side.second.reset(); mapped_main.reset();
    require(!native_pin.expired() && !side_pin.expired() && !main_pin.expired(), "paused builder lost source pins");
    if (!moved.done()) rejects([&] { (void)moved.finish(); });
    unsigned calls = 0;
    while (!moved.done()) {
      auto work = moved.step(7);
      require(work && work <= 7 && ++calls < 10000, "mapped builder stalled or exceeded budget");
    }
    auto artifact = moved.finish();
    require(!native_pin.expired() && !side_pin.expired() && !main_pin.expired(), "artifact lost exact input owners");
    require(artifact.native_owner() == native_pin.lock() && artifact.main_target() == main_pin.lock() &&
      artifact.secondary_target() == side_pin.lock(), "artifact replaced source owner");
    require(artifact.native().view().bytes().data() == native_address, "mapped native FC bytes were copied");
    require(file_bytes(native_path) == original, "received native file changed");
    auto reference = encode_cola_sections(expected, native_id, main_id, side_id).materialize();
    mapped_cola_index_builder<P> terminal(artifact.native_owner());
    while (!terminal.done()) terminal.step(3);
    auto terminal_artifact = terminal.finish();
    auto terminal_expected = node::adopt_native(native);
    require(encode_cola_sections(terminal_artifact, native_id).materialize() ==
      encode_cola_sections(terminal_expected, native_id).materialize(), "absent mapped targets changed terminal encoding");
    auto encoded = encode_cola_sections(artifact, native_id, main_id, side_id);
    require(encoded.materialize() == reference, "mapped construction changed exact IX03 bytes");
    auto output_id = files.fresh(); encoded.seal(files.root, output_id, files.attempt);
    auto output = std::make_shared<mapped_cola_index<P> const>(mapped_cola_index<P>::open(files.root / object_path(output_id, file_kind::fractional_index)));
    auto linked = mapped_cola_blob<P>::bind({native_id, output_id}, artifact.native_owner(), output,
      artifact.main_target(), artifact.secondary_target(), side_id);
    linked->scan();
    require(linked->virtual_size() == expected.virtual_size(), "sealed artifact has wrong augmented extent");
    // Complete each partial K-window; the in-memory reference has independently
    // encoded the same sources and provides only the routed lower boundary.
    auto own = std::make_shared<node const>(std::move(expected));
    cola_sample_cursor<P> boundaries(own);
    for (std::uint64_t group = 0; !boundaries.done(); ++group, boundaries.advance()) {
      auto boundary = boundaries.peek().key;
      for (auto const & record : native_rows) if (compare_bits(boundary, record.key.view()) <= 0) {
        auto context = profile_query_context<P>(record.key.view()).with_key(boundary);
        auto actual = linked->view().search_window(group, context);
        auto wanted = own->view().search_window(group, context);
        require(bool(actual.native) == bool(wanted.native), "mapped local query differs from owning path");
        if (actual.native) require(actual.native->ordinal == wanted.native->ordinal &&
          compare_bits(actual.native->value.view(), wanted.native->value.view()) == 0, "mapped native value differs");
      }
    }
    // Unlink after opening: paused/new construction consumes the retained maps,
    // while no code is allowed to reopen or rewrite the native input.
    mapped_cola_index_builder<P> unlinked(artifact.native_owner(), artifact.main_target(), artifact.secondary_target());
    if (!unlinked.done()) unlinked.step(1);
    std::filesystem::remove(native_path);
    while (!unlinked.done()) unlinked.step(11);
    auto rebuilt = unlinked.finish();
    require(encode_cola_sections(rebuilt, native_id, main_id, side_id).materialize() == reference,
      "unlink invalidated retained mapped construction");
  }
  template <class T> concept temporary_encoder = requires(T value, object_id const & identity) {
    encode_cola_sections(std::move(value), identity);
  };
  template <class T> concept root_preparation = requires { T::prepare_root(); };
  template <class P> void run() {
    static_assert(!temporary_encoder<mapped_cola_artifact<P>>);
    static_assert(!root_preparation<mapped_cola_artifact<P>>);
    matrix<P>(0, 0); matrix<P>(2, 0); matrix<P>(67, 4096);
    rejects([] { mapped_cola_index_builder<P> invalid(nullptr); });
  }
}
#endif
int main() {
#if defined(__APPLE__) || defined(__linux__)
  try {
    run<everett::storage_policy<everett::profile_unit::byte, everett::variable_values, 3, everett::exponential_golomb<0>, 16>>();
    run<everett::storage_policy<everett::profile_unit::bit, everett::variable_values, 7, everett::exponential_golomb<0>, 15>>();
    run<everett::storage_policy<everett::profile_unit::byte, everett::fixed_values<0>, 15, everett::exponential_golomb<0>, 16>>();
    run<everett::storage_policy<everett::profile_unit::bit, everett::fixed_values<0>, 31, everett::exponential_golomb<0>, 15>>();
    std::cout << "Mapped COLA construction checks passed\n";
  } catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
#endif
}
/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests incremental mapped COLA construction without native rewriting.
 */
