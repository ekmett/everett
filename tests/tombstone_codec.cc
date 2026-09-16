/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks explicit FC retention bounds and semantic tombstone preservation.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/sort_runtime.h>
#include <everett/sort_profile_adaptive.h>
#include <everett/native_file_writer.h>
#include <everett/sort_profile_file_merge.h>
#include <fstream>
#include <iostream>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using P = string_policy;
  using array = sort_profile_array<P>;
  void check(bool v, char const * why) { if (!v) throw std::runtime_error(why); }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto p = (std::filesystem::temp_directory_path() / "everett-tombstone-codec-XXXXXX").string();
      if (!::mkdtemp(p.data())) throw std::runtime_error("mkdtemp");
      root = p;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  object_attempt_id attempt(unsigned n) { return object_attempt_id(id(n).hex()); }
  std::vector<std::byte> file_bytes(std::filesystem::path const & path) {
    std::vector<std::byte> result(std::filesystem::file_size(path));
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char *>(result.data()), std::streamsize(result.size()));
    if (!in) throw std::runtime_error("read canonical file");
    return result;
  }
  profile_record row(std::string const & key, std::optional<std::string> const & value,
      std::optional<std::uint64_t> cap = {}) {
    bit_string encoded; sort_bit_writer out(encoded);
    sort_codec<strings>::value_codec::write(out, value);
    return {sort_profile_query<P, strings>(key), std::move(encoded), cap};
  }
  array build(std::vector<profile_record> const & records) {
    sort_profile_writer<P> writer;
    sort_runtime_detail::write_sorted_native<P, registry_selector<string_registry>>(writer, records);
    return writer.finish();
  }
  auto bytes(array const & source) { return encoded_sort_sections<P>::from(source).materialize(); }
  template <class View> void content(View view, std::vector<profile_record> const & expected) {
    if constexpr (requires { view.scan(); }) view.scan();
    auto cursor = view.cursor();
    for (auto const & record : expected) {
      check(!cursor.done(), "missing row");
      auto item = cursor.peek();
      check(compare_bits(item.key.prefix, record.key.view()) == 0 &&
        compare_bits(item.value, record.value.view()) == 0, "retention changed logical content");
      cursor.advance();
    }
    check(cursor.done(), "extra row");
  }
  struct tombstones : replace_native_value {
    bit_view operator()(bit_view, bit_view newer) const { return newer; }
    unsigned * unary_calls = nullptr;
    bool is_tombstone(bit_view value) const {
      if (unary_calls) ++*unary_calls;
      return !value.at(0);
    }
    bool is_tombstone(bit_view, bit_view) const { throw std::runtime_error("unary fast path bypassed"); }
  };
  template <class Compose> array merge(array const & older, array const & newer, Compose compose,
      std::uint64_t * materialized = nullptr) {
    sort_profile_merge_builder<P, array, Compose> builder(std::make_shared<array const>(older),
      std::make_shared<array const>(newer), compose);
    while (!builder.done()) builder.step(1);
    if (materialized) *materialized = builder.materialized_keys();
    return builder.finish();
  }
  void direct_writers() {
    temporary files;
    std::vector<profile_record> records;
    for (unsigned i = 0; i != 32; ++i) {
      char key[48]; std::snprintf(key, sizeof key, "shared-long-prefix/key-%03u", i);
      records.push_back(row(key, i == 15 || i == 16 ? std::nullopt : std::optional<std::string>("value")));
    }
    records[15].retained_limit_bits = 9;
    records[16].retained_limit_bits = 0;
    auto expected = build(records); content(expected.view(), records);
    check(expected.view().encoded_at(15).retained == 9, "W boundary cap ignored");
    check(expected.view().encoded_at(16).retained == 1, "selector floor cap ignored");
    check(expected.view().encoded_at(0).retained == 0, "first logical key not self contained");
    sort_profile_writer<P> typed;
    for (unsigned i = 0; i != records.size(); ++i) {
      char key[48]; std::snprintf(key, sizeof key, "shared-long-prefix/key-%03u", i);
      typed.append<strings>(key, i == 15 || i == 16 ? std::nullopt : std::optional<std::string>("value"), records[i].retained_limit_bits);
    }
    check(bytes(typed.finish()) == bytes(expected), "typed writer changed capped encoding");
    sort_profile_file_writer<P> writer(files.root, id(1), attempt(1));
    for (auto const & record : records) writer.append_encoded(record);
    auto receipt = writer.finish(); auto mapped = mapped_sort_profile<P>::open(receipt.path);
    content(mapped.view(), records);
    check(file_bytes(receipt.path) == bytes(expected), "streamed cap bytes differ");
    auto factory = [&] { return std::make_unique<object_stream<P>>(files.root, id(2), attempt(2), file_kind::native_blob, 192); };
    using output = sort_profile_adaptive_writer<P, registry_selector<string_registry>, posix_object_ops, decltype(factory)>;
    output adaptive(factory, output_budget(1 << 20), 128 << 10, true);
    sort_runtime_detail::write_sorted_native<P, registry_selector<string_registry>>(adaptive, records);
    auto result = adaptive.finish();
    check(std::holds_alternative<std::shared_ptr<array const>>(result), "small adaptive output spilled");
    check(bytes(*std::get<std::shared_ptr<array const>>(result)) == bytes(expected), "adaptive cap bytes differ");
    output spilled(factory, output_budget(0), 0, true);
    sort_runtime_detail::write_sorted_native<P, registry_selector<string_registry>>(spilled, records);
    auto sealed = spilled.finish();
    check(std::holds_alternative<object_seal_receipt>(sealed), "zero allowance did not stream");
    check(file_bytes(std::get<object_seal_receipt>(sealed).path) == bytes(expected), "adaptive spill cap bytes differ");
  }
  void direct_merges() {
    auto old_records = std::vector{row("a0", "old"), row("ab9", std::nullopt)};
    auto old = build(old_records), inserted = build({row("ab8", "new")});
    auto stored = old.view().encoded_at(1).retained;
    auto ordinary = merge(old, inserted, replace_native_value{});
    check(ordinary.view().encoded_at(2).retained > stored, "fixture does not raise natural retention");
    tombstones semantic; unsigned calls = 0; semantic.unary_calls = &calls;
    std::uint64_t materialized = 99;
    auto capped = merge(old, inserted, std::ref(semantic), &materialized);
    check(capped.view().encoded_at(2).retained == stored, "natural-small tombstone lost its cap");
    check(calls == 3 && materialized == 0, "unary tombstone path materialized keys");
    content(capped.view(), {row("a0", "old"), row("ab8", "new"), row("ab9", std::nullopt)});
    temporary files;
    sort_profile_file_merge<P, array, std::reference_wrapper<tombstones>> streamed(files.root,
      id(3), attempt(3), std::make_shared<array const>(old), std::make_shared<array const>(inserted), std::ref(semantic));
    while (!streamed.done()) streamed.step(1);
    auto receipt = streamed.finish();
    check(file_bytes(receipt.path) == bytes(capped), "streamed merge cap bytes differ");
    auto same = merge(build({row("ab8", "live"), row("ab9", "live")}),
      build({row("ab9", std::nullopt)}), std::ref(semantic), &materialized);
    check(materialized == 0, "binary matched composition materialized key");
    check(same.view().encoded_at(1).retained == 1, "same-key source minimum lost");
    auto revived = merge(same, build({row("ab9", "revived")}), std::ref(semantic));
    check(revived.view().encoded_at(1).retained > 1, "live replacement kept tombstone cap");
    content(revived.view(), {row("ab8", "live"), row("ab9", "revived")});
    // Repeated merges must keep the original cap after neighboring insertions.
    auto again = merge(capped, build({row("ab85", "later")}), std::ref(semantic));
    check(again.view().encoded_at(3).retained == stored, "second merge raised tombstone cap");
    struct dependent : replace_native_value {
      bool is_tombstone(bit_view key, bit_view value) const {
        check(key.size() > 1, "key-dependent callback omitted key"); return !value.at(0);
      }
    };
    auto generic = merge(old, inserted, dependent{}, &materialized);
    check(generic.view().encoded_at(2).retained == stored && materialized == 3,
      "key-dependent predicate fallback failed");
  }
  template <class Policy> void opaque() {
    auto make = [](std::string const & key, unsigned value, std::optional<std::uint64_t> cap = {}) {
      std::string v(1, char(value));
      return profile_record{bit_string::from_bytes(key), bit_string::from_bytes(v), cap};
    };
    std::vector records{make("shared-a", 1), make("shared-b", 0, 13), make("shared-c", 1)};
    auto expected = profile_array<Policy>::build(records); content(expected.view(), records);
    auto expected_retained = (std::uint64_t{13} >> Policy::unit_shift) << Policy::unit_shift;
    check(expected.view().encoded_at(1).retained * Policy::bits_per_unit == expected_retained, "opaque unit cap");
    profile_native_writer<Policy> writer(8 >> Policy::unit_shift);
    for (auto const & record : records) writer.append(record);
    auto written = writer.finish();
    check(written.bytes().size() == expected.bytes().size() &&
      std::equal(written.bytes().begin(), written.bytes().end(), expected.bytes().begin()), "opaque writer cap bytes");
    struct semantic : replace_native_value {
      bit_view operator()(bit_view, bit_view newer) const { return newer; }
      bool is_tombstone(bit_view v) const { return profile_detail::load_bits(v, 0, 8) == 0; }
    };
    using native = profile_array<Policy>;
    auto older = std::make_shared<native const>(native::build(std::vector{make("ab8", 1), make("ab9", 1)}));
    auto newer = std::make_shared<native const>(native::build(std::vector{make("ab9", 0)}));
    semantic compose;
    static_assert(native_merge_builder<Policy, native, std::reference_wrapper<semantic>>::encoded_keys);
    native_merge_builder<Policy, native, std::reference_wrapper<semantic>> merger(older, newer, std::ref(compose));
    while (!merger.done()) merger.step(1);
    auto output = merger.finish();
    check(output.view().encoded_at(1).retained == 0, "opaque merge did not expand inherited literal");
    content(output.view(), std::vector{make("ab8", 1), make("ab9", 0)});
    struct keyed : semantic {
      bit_view operator()(bit_view, bit_view, bit_view newer) const { return newer; }
    };
    static_assert(!native_merge_builder<Policy, native, keyed>::encoded_keys);
    native_merge_builder<Policy, native, keyed> keyed_merge(older, newer, keyed{});
    keyed_merge.step(100); auto keyed_output = keyed_merge.finish();
    check(keyed_output.view().encoded_at(1).retained == 0, "decoded merge lost source cap");
  }
}
int main() {
  try {
    direct_writers(); direct_merges();
    opaque<storage_policy<tip<encoded_sort<bit_encoding<>>>>>();
    opaque<storage_policy<tip<encoded_sort<byte_encoding<>>>>>();
  } catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
}
