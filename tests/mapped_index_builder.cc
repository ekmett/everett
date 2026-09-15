/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests direct mapped-native indexing against exact files and original-key oracles.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/index_builder.h>
#include <diet/mapped_blob.h>
#include <diet/profile_index.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
  using namespace diet;

  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid mapped index operation accepted");
  }
  template <class T> concept temporary_index_encoding = requires(T && value, object_id const & id) {
    encode_index_sections(std::move(value), id);
  };
  template <class T> concept bound_finish = requires(T & value) { value.finish(); };
  template <class A, class B> bool same_range(A const & a, B const & b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }

  // The oracle reads original bits directly and sorts their 0/1 strings.
  // It does not use Diet's key comparison, rank, FC or sampling routines.
  std::string bits(bit_view value) {
    std::string result;
    for (std::uint64_t i = 0; i < value.size(); ++i) {
      auto at = value.offset() + i;
      auto bit = (std::to_integer<unsigned>(value.storage()[at / 8]) >> (7 - at % 8)) & 1;
      result.push_back(bit ? '1' : '0');
    }
    return result;
  }
  std::uint64_t common(std::string const & a, std::string const & b) {
    std::uint64_t i = 0;
    while (i < std::min(a.size(), b.size()) && a[i] == b[i]) ++i;
    return i;
  }
  template <class P> bit_string value(unsigned seed) {
    auto units = P::value_width.value_or(seed % 13);
    std::string result(units * P::bits_per_unit, '0');
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = (i + seed) % 3 ? '1' : '0';
    return bit_string::from_bits(result);
  }
  template <class P> std::vector<bit_string> keys() {
    std::vector<bit_string> result;
    for (std::string key : {"", "a", "aa", "ab", "b", "zz"})
      result.push_back(bit_string::from_bytes(key));
    result.push_back(bit_string::from_bytes(std::string(1, '\0')));
    result.push_back(bit_string::from_bytes(std::string(1, char(255))));
    for (unsigned i = 0; i < 47; ++i) {
      auto key = bit_string::from_bytes("long/shared/prefix/" + std::string(1, char(i)));
      if constexpr (P::unit == profile_unit::bit) {
        auto text = bits(key.view());
        for (unsigned j = 0; j < i % 7; ++j) text += (i >> j) & 1 ? '1' : '0';
        key = bit_string::from_bits(text);
      }
      result.push_back(std::move(key));
    }
    if constexpr (P::unit == profile_unit::bit)
      for (std::string text : {"0", "01", "011", "1"}) result.push_back(bit_string::from_bits(text));
    std::sort(result.begin(), result.end(), [](auto const & a, auto const & b) { return bits(a.view()) < bits(b.view()); });
    result.erase(std::unique(result.begin(), result.end(), [](auto const & a, auto const & b) {
      return bits(a.view()) == bits(b.view());
    }), result.end());
    return result;
  }

  struct occurrence { std::string key; bool borrowed; std::uint64_t ordinal; };
  std::vector<occurrence> merged(std::span<profile_record const> native,
      std::span<occurrence const> target, std::uint64_t stride) {
    std::vector<occurrence> result;
    for (std::size_t i = 0; i < native.size(); ++i) result.push_back({bits(native[i].key.view()), false, i});
    for (std::size_t i = 0; i < target.size(); i += stride) result.push_back({target[i].key, true, i / stride});
    std::stable_sort(result.begin(), result.end(), [](auto const & a, auto const & b) {
      return a.key == b.key ? a.borrowed < b.borrowed : a.key < b.key;
    });
    return result;
  }

  struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
#if defined(__unix__) || defined(__APPLE__)
      auto pattern = (std::filesystem::temp_directory_path() / "diet-mapped-index-XXXXXX").string();
      auto result = ::mkdtemp(pattern.data());
      if (!result) throw std::system_error(errno, std::generic_category(), "create mapped index fixture");
      path = result;
#else
      for (unsigned i = 0; i < 10000; ++i) {
        auto candidate = std::filesystem::temp_directory_path() / ("diet-mapped-index-" + std::to_string(i));
        if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
      }
      throw std::runtime_error("cannot reserve mapped index fixture directory");
#endif
    }
    ~temporary_directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  object_id id(unsigned number) {
    char tail[9];
    std::snprintf(tail, sizeof tail, "%08x", number);
    return object_id(std::string(24, 'a') + tail);
  }
  template <class P> struct native_input {
    object_id identity;
    std::filesystem::path path;
    std::shared_ptr<mapped_native<P> const> owner;
  };
  template <class P> struct storage {
    temporary_directory directory;
    unsigned next = 1;
    object_id fresh() { return id(next++); }
    object_attempt_id attempt() { return object_attempt_id(fresh().hex()); }
    native_input<P> native(profile_array<P> const & array) {
      auto identity = fresh();
      auto encoding = encode_native_sections(array);
      auto receipt = encoding.seal(directory.path, identity, attempt());
      auto owner = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(receipt.path));
      owner->scan();
      return {std::move(identity), receipt.path, std::move(owner)};
    }
    typename mapped_blob<P>::pair_type bind(profile_index<P> const & index, native_input<P> const & native,
        typename mapped_blob<P>::pair_type target = {}) {
      auto identity = fresh();
      auto encoding = encode_index_sections(index, native.identity,
        target ? std::optional(target->identity()) : std::nullopt);
      auto receipt = encoding.seal(directory.path, identity, attempt());
      auto mapped = std::make_shared<mapped_index<P> const>(mapped_index<P>::open(receipt.path));
      return mapped_blob<P>::bind({native.identity, std::move(identity)}, native.owner, std::move(mapped), std::move(target));
    }
  };

  template <class P, class Native, class Target>
  void drain(index_builder<P, Native> & builder, std::shared_ptr<Target const> target,
      std::span<occurrence const> target_order, std::span<occurrence const> output, bool coded) {
    std::optional<sample_cursor<P, Target>> sampler;
    if (target) sampler.emplace(target);
    profile_sample_encoder<P> encoder;
    profile_sample_decoder<P> decoder;
    std::uint64_t received = 0, emitted = 0, consumed = 0, iterations = 0;
    while (!builder.done()) {
      require(++iterations < 100000, "mapped index stalled");
      if (builder.needs_input()) {
        if (sampler && !sampler->done()) {
          auto sample = sampler->peek();
          auto ordinal = received * P::group_size;
          require(ordinal < target_order.size() && sample.target_ordinal == ordinal &&
            bits(sample.key) == target_order[ordinal].key &&
            (sample.source_role == stream_role::borrowed) == target_order[ordinal].borrowed &&
            sample.source_ordinal == target_order[ordinal].ordinal, "mapped sampler independent occurrence oracle");
          if (coded) builder.push(encoder.encode(sample.key, sample.target_ordinal));
          else builder.push(sample.key, sample.target_ordinal);
          sampler->advance();
          ++received;
        } else builder.close_input();
      }
      require(builder.step(0) == 0, "mapped index zero budget");
      auto budget = iterations % 3 == 0 ? P::group_size + 1 : iterations % 3 == 1 ? 1 : P::group_size;
      auto used = builder.step(budget);
      require(used <= budget, "mapped index record budget");
      consumed += used;
      if (builder.has_output()) {
        require(builder.step(budget) == 0, "mapped index output backpressure");
        auto ordinal = emitted * P::group_size;
        require(ordinal < output.size(), "mapped index extra output");
        if (coded) {
          auto frame = builder.take_coded_output();
          auto key = decoder.accept(frame);
          require(frame.target_ordinal == ordinal && bits(key) == output[ordinal].key, "coded mapped output oracle");
        } else {
          auto sample = builder.take_output();
          require(sample.target_ordinal == ordinal && bits(sample.key.view()) == output[ordinal].key, "mapped output oracle");
        }
        ++emitted;
      }
    }
    require(consumed == output.size() && builder.size() == output.size(), "mapped index exact work count");
    require(received == builder.received_samples() &&
      emitted == output.size() / P::group_size + (output.size() % P::group_size != 0), "mapped index sample counts");
  }

  template <class P> void index_oracle(profile_index<P> const & index,
      std::span<profile_record const> native, std::span<occurrence const> order) {
    require(index.native_size() == native.size() && index.virtual_size() == order.size(), "index-only counts");
    auto borrowed = index.borrowed().view().cursor();
    auto ranks = index.interleave().view();
    std::uint64_t prefix_count = 0, population = 0;
    std::string predecessor;
    for (std::size_t i = 0; i < order.size(); ++i) {
      auto group = i / P::group_size;
      if (i % P::group_size == 0) {
        require(ranks.rank(group) == prefix_count, "independent group rank");
        require(index.cut_lcps()[group] == (prefix_count ? common(predecessor, order[i].key) : 0), "independent cut LCP");
        population = 0;
      }
      if (order[i].borrowed) {
        require(!borrowed.done() && bits(borrowed.peek().key.prefix) == order[i].key && borrowed.peek().value.empty(),
          "borrowed key oracle");
        bool expected_false = false;
        for (auto const & row : native) expected_false |= bits(row.key.view()) == order[i].key;
        auto flag = (std::to_integer<unsigned>(index.false_borrow_bits()[prefix_count / 8]) >> (prefix_count % 8)) & 1;
        require(bool(flag) == expected_false, "independent false-borrow flag");
        predecessor = order[i].key;
        ++prefix_count; ++population;
        borrowed.advance();
      }
      if (i % P::group_size == P::group_size - 1 || i + 1 == order.size())
        require(ranks.class_at(group) == population, "independent class population");
    }
    require(borrowed.done() && index.borrowed().size() == prefix_count, "borrowed end oracle");
  }

  template <class P> struct layer {
    std::vector<profile_record> rows;
    std::vector<occurrence> order;
    std::shared_ptr<profile_blob<P> const> owning;
    typename mapped_blob<P>::pair_type mapped;
  };
  template <class P> layer<P> build_layer(storage<P> & files, std::vector<profile_record> rows,
      layer<P> const * target, bool coded) {
    static_assert(!temporary_index_encoding<profile_index<P>>);
    static_assert(!bound_finish<index_builder<P, mapped_native<P>>>);
    auto source = profile_blob<P>::build(rows);
    auto native = files.native(source.native());
    auto native_bytes = native.owner->view().bytes();
    auto order = merged(rows, target ? std::span<occurrence const>(target->order) : std::span<occurrence const>{}, P::group_size);
    index_builder<P, mapped_native<P>> original(native.owner);
    rejects([&] { original.finish_index(0); });
    auto builder = std::move(original);
    require(!original.needs_input() && !original.done(), "moved mapped builder state");
    rejects([&] { original.step(1); });
    rejects([&] { original.close_input(); });
    rejects([&] { original.finish_index(0); });
    rejects([&] { original.push(bit_view{}, 0); });
    auto mapped_target = target ? target->mapped : typename mapped_blob<P>::pair_type{};
    auto target_order = target ? std::span<occurrence const>(target->order) : std::span<occurrence const>{};
    drain(builder, mapped_target, target_order, order, coded);
    auto count = target ? target->order.size() : 0;
    rejects([&] { builder.finish_index(count + P::group_size); });
    require(builder.done() && !builder.finished(), "wrong sample count permits retry");
    auto index = builder.finish_index(count);
    require(builder.finished(), "mapped index finalization");
    rejects([&] { builder.step(1); });
    rejects([&] { builder.finish_index(count); });
    index_oracle<P>(index, rows, order);

    index_builder<P> reference_builder(source);
    auto owning_target = target ? target->owning : std::shared_ptr<profile_blob<P> const>{};
    drain(reference_builder, owning_target, target_order, order, !coded);
    auto reference = std::make_shared<profile_blob<P> const>(reference_builder.finish(owning_target));
    auto target_id = target ? std::optional(target->mapped->identity()) : std::nullopt;
    auto actual_wire = encode_index_sections(index, native.identity, target_id).materialize();
    auto expected_wire = encode_index_sections(*reference, native.identity, target_id).materialize();
    require(actual_wire == expected_wire, "exact mapped/owning index file oracle");
    if (!target) {
      auto terminal = profile_index<P>::native_only(rows.size());
      require(encode_index_sections(terminal, native.identity).materialize() == expected_wire,
        "count-only terminal encoding matches traversing builder");
    }
    if (index.borrowed().size()) rejects([&] { (void)encode_index_sections(index, native.identity); });
    auto mapped = files.bind(index, native, mapped_target);
    require(mapped->native_object() == native.owner && mapped->native().bytes().data() == native_bytes.data() &&
      same_range(mapped->native().bytes(), source.native().bytes()), "index-only result preserves exact native mapping");
    mapped->scan();
    return {std::move(rows), std::move(order), std::move(reference), std::move(mapped)};
  }

  template <class P> void matrix() {
    storage<P> files;
    for (auto count : {std::uint64_t{0}, P::group_size - 1, P::group_size,
                       P::group_size + 1, 2 * P::group_size + 1}) {
      std::vector<profile_record> rows;
      for (std::uint64_t i = 0; i < count; ++i)
        rows.push_back({bit_string::from_bytes("terminal/" + std::string(1, char(i))), value<P>(unsigned(i))});
      auto source = profile_blob<P>::build(rows);
      auto order = merged(rows, {}, P::group_size);
      index_builder<P> builder(source);
      drain(builder, std::shared_ptr<profile_blob<P> const>{}, {}, order, false);
      auto traversed = builder.finish();
      auto terminal = profile_index<P>::native_only(count);
      require(terminal.native_size() == count && terminal.virtual_size() == count && terminal.borrowed().size() == 0,
        "terminal directory boundary counts");
      require(encode_index_sections(terminal, id(1000)).materialize() ==
        encode_index_sections(traversed, id(1000)).materialize(), "terminal boundary exact wire oracle");
    }
    auto all = keys<P>();
    std::vector<layer<P>> layers;
    for (unsigned depth = 0; depth < 3; ++depth) {
      std::vector<profile_record> rows;
      for (std::size_t i = 0; i < all.size(); ++i)
        if (depth == 0 || (i + depth) % (depth + 1) == 0) rows.push_back({all[i], value<P>(unsigned(i + 19 * depth))});
      layers.push_back(build_layer<P>(files, std::move(rows), layers.empty() ? nullptr : &layers.back(), depth & 1));
    }
    while (layers.back().order.size() > P::group_size)
      layers.push_back(build_layer<P>(files, {}, &layers.back(), layers.size() & 1));
    auto root = mapped_query_root<P>::adopt_prepared(layers.back().mapped);
    auto owning = query_root<P>::build(layers.back().owning);
    require(root.head() == layers.back().mapped && owning.head() == layers.back().owning, "already bounded root identity");
    all.push_back(bit_string::from_bytes("missing/key"));
    all.push_back(bit_string::from_bytes("long/shared/prefix/"));
    for (auto const & query : all) {
      struct expected { typename mapped_blob<P>::pair_type source; std::uint64_t ordinal; std::string value; };
      std::vector<expected> matches;
      auto key = bits(query.view());
      for (auto i = layers.size(); i-- > 0;)
        for (std::size_t n = 0; n < layers[i].rows.size(); ++n)
          if (bits(layers[i].rows[n].key.view()) == key)
            matches.push_back({layers[i].mapped, n, bits(layers[i].rows[n].value.view())});
      auto cursor = root.cursor(query.view());
      std::size_t found = 0;
      while (!cursor.done()) {
        require(cursor.step(1) <= 1, "mapped complete-query budget");
        if (!cursor.has_match()) continue;
        auto match = cursor.take_match();
        require(found < matches.size() && match.source == matches[found].source &&
          match.ordinal == matches[found].ordinal && bits(match.value.view()) == matches[found].value,
          "mapped builder complete query differs from original rows");
        ++found;
      }
      require(found == matches.size(), "mapped builder missing native query result");
    }
    rejects([&] { index_builder<P, mapped_native<P>> invalid(std::shared_ptr<mapped_native<P> const>{}); });
    rejects([&] { sample_cursor<P, mapped_blob<P>> invalid(typename mapped_blob<P>::pair_type{}); });
    sample_cursor<P, mapped_blob<P>> before(layers.back().mapped);
    auto moved = std::move(before);
    require(before.done() && !before.target(), "moved sampler drops its target");
    rejects([&] { before.peek(); });
    rejects([&] { before.advance(); });
    auto sample = moved.peek();
    require(bits(sample.key) == layers.back().order[0].key, "moved sampler retains current context");
  }

  void lifetime() {
    using P = storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 3>;
    storage<P> files;
    std::vector<profile_record> target_rows{{bit_string::from_bytes("a"), bit_string::from_bytes("old-a")},
                                          {bit_string::from_bytes("z"), bit_string::from_bytes("old-z")}};
    auto target = build_layer<P>(files, target_rows, nullptr, false);
    auto source_array = profile_array<P>::build(std::array<profile_record, 1>{{
      {bit_string::from_bytes("a"), bit_string::from_bytes("new-a")}}});
    auto native = files.native(source_array);
    std::weak_ptr<mapped_native<P> const> native_weak = native.owner;
    std::weak_ptr<mapped_blob<P> const> target_weak = target.mapped;
    auto pointer = native.owner->view().bytes().data();
    auto builder = std::make_unique<index_builder<P, mapped_native<P>>>(native.owner);
    auto sampler = std::make_unique<sample_cursor<P, mapped_blob<P>>>(target.mapped);
#if defined(__unix__) || defined(__APPLE__)
    std::filesystem::remove(native.path);
    std::filesystem::remove(files.directory.path / object_path(target.mapped->identity().native, file_kind::native_blob));
    std::filesystem::remove(files.directory.path / object_path(target.mapped->identity().index, file_kind::fractional_index));
#endif
    native.owner.reset(); target.mapped.reset();
    require(!native_weak.expired() && !target_weak.expired(), "active builder/sampler pins survive reset and unlink");
    while (!builder->done()) {
      if (builder->needs_input()) {
        if (!sampler->done()) { auto sample = sampler->peek(); builder->push(sample.key, sample.target_ordinal); sampler->advance(); }
        else builder->close_input();
      }
      builder->step(1);
      if (builder->has_output()) (void)builder->take_output();
    }
    auto artifact = builder->finish_index(sampler->target()->virtual_size());
    native.owner = native_weak.lock();
    auto result = files.bind(artifact, native, sampler->target());
    builder.reset(); sampler.reset(); native.owner.reset();
    require(result->native().bytes().data() == pointer, "unlinked native mapping is reused");
    result->scan();
    auto root = mapped_query_root<P>::adopt_prepared(result);
    for (std::string query : {"", "a", "b", "z", "zz"}) {
      auto key = bit_string::from_bytes(query);
      auto cursor = root.cursor(key.view());
      std::vector<std::string> values;
      while (!cursor.done()) {
        cursor.step(1);
        if (cursor.has_match()) { auto match = cursor.take_match(); values.push_back(bits(match.value.view())); }
      }
      std::vector<std::string> expected;
      if (query == "a") expected = {bits(source_array.view().encoded_at(0).value), bits(target_rows[0].value.view())};
      if (query == "z") expected = {bits(target_rows[1].value.view())};
      require(values == expected, "unlinked complete query order and values");
    }
  }

  void untouched_native_values() {
#if defined(__unix__) || defined(__APPLE__)
    using P = storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 7, exponential_golomb<0>, 16>;
    auto page_query = ::sysconf(_SC_PAGESIZE);
    require(page_query > 0, "page size");
    auto page = static_cast<std::size_t>(page_query);
    storage<P> files;
    std::vector<profile_record> rows;
    for (unsigned i = 0; i < 9; ++i)
      rows.push_back({bit_string::from_bytes("key/" + std::to_string(i)), bit_string::from_bytes(std::string(5 * page, 'v'))});
    auto source = profile_array<P>::build(rows);
    auto native = files.native(source);
    auto view = native.owner->view();
    auto protect = [&](std::uintptr_t address, auto && operation) {
      auto pointer = reinterpret_cast<void *>(address);
      require(::mprotect(pointer, page, PROT_NONE) == 0, "protect mapped native page");
      try { operation(); } catch (...) { (void)::mprotect(pointer, page, PROT_READ); throw; }
      require(::mprotect(pointer, page, PROT_READ) == 0, "restore mapped native page");
    };
    auto first_page = reinterpret_cast<std::uintptr_t>(view.bytes().data()) / page * page;
    protect(first_page, [&] {
      auto artifact = profile_index<P>::native_only(native.owner->size());
      require(artifact.native_size() == rows.size() && artifact.borrowed().size() == 0, "terminal construction only needs cached count");
      (void)encode_index_sections(artifact, native.identity).materialize();
    });
    auto first_value = view.encoded_at(0).value;
    auto value_begin = reinterpret_cast<std::uintptr_t>(first_value.storage().data()) + first_value.offset() / 8;
    auto interior = (value_begin / page + 1) * page;
    require(interior + page < value_begin + first_value.size() / 8, "guard lies strictly inside native value");
    protect(interior, [&] {
      index_builder<P, mapped_native<P>> builder(native.owner);
      builder.close_input();
      while (!builder.done()) {
        builder.step(5);
        if (builder.has_output()) (void)builder.take_output();
      }
      auto artifact = builder.finish_index(0);
      require(artifact.native_size() == rows.size(), "mapped indexing skips native value pages");
      (void)encode_index_sections(artifact, native.identity).materialize();
    });
    native.owner->scan();
#endif
  }

  void empty_terminal() {
    using P = storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<fixed_values<0>>>>, 3, golomb<3>, 1>;
    storage<P> files;
    auto empty = build_layer<P>(files, {}, nullptr, true);
    auto root = mapped_query_root<P>::adopt_prepared(empty.mapped);
    require(root.cursor(bit_view{}).done(), "explicit empty mapped root");
    sample_cursor<P, mapped_blob<P>> sampler(empty.mapped);
    require(sampler.done(), "empty terminal sampler");
    rejects([&] { sampler.peek(); });
    rejects([&] { sampler.advance(); });
    auto input = files.native(profile_array<P>::build({}));
    auto terminal = profile_index<P>::native_only(0);
    auto bad = encode_index_sections(terminal, input.identity, blob_identity{id(999), id(998)});
    auto object = files.fresh();
    auto receipt = bad.seal(files.directory.path, object, files.attempt());
    auto index = std::make_shared<mapped_index<P> const>(mapped_index<P>::open(receipt.path));
    rejects([&] { mapped_blob<P>::bind({input.identity, object}, input.owner, index, empty.mapped); });
  }
}

int main() try {
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 3, exponential_golomb<0>, 16>>();
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<fixed_values<0>>>>, 7, exponential_golomb<0>, 15>>();
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<fixed_values<8>>>>, 15, exponential_golomb<0>, 16>>();
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 31, exponential_golomb<0>, 15>>();
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 3, golomb<3>, 16>>();
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<fixed_values<0>>>>, 7, exponential_golomb<2>, 15>>();
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<fixed_values<13>>>>, 15, golomb<5>, 16>>();
  matrix<storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 31, exponential_golomb<0>, 15>>();
  lifetime();
  untouched_native_values();
  empty_terminal();
  std::cout << "Mapped native index construction checks passed\n";
  return 0;
} catch (std::exception const & error) {
  std::cerr << error.what() << '\n';
  return 1;
}
