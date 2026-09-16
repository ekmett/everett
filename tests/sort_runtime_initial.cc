/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks direct sorted native construction across owning and adaptive storage.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/sort_runtime_context.h>

#include <iostream>

namespace {
  using namespace diet;
  using strings = unsorted<std::optional<std::string>>;
  using P = string_policy;
  using storage = sort_file_runtime_storage<P>;
  using owned_storage = sort_runtime_storage<P>;
  object_id id(unsigned n = 1) {
    char text[33]; std::snprintf(text, sizeof text, "%032x", n); return object_id(text);
  }
  void check(bool value, char const * why) { if (!value) throw std::runtime_error(why); }
  template <class F> void rejects(F && action) {
    try { action(); } catch (std::exception const &) { return; }
    throw std::runtime_error("expected sorted native rejection");
  }
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-sorted-native-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  std::size_t files(std::filesystem::path const & root) {
    std::size_t count = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++count;
    return count;
  }
  template <class Policy, class Selector, class S, class K, class V>
  profile_record record(K const & key, V const & value) {
    bit_string encoded;
    sort_bit_writer out(encoded);
    sort_codec<S>::value_codec::write(out, value);
    return {sort_key_transport<Policy, Selector>::template encode<S>(key), std::move(encoded)};
  }
  profile_record string_record(std::string const & key, std::optional<std::string> const & value) {
    return record<P, registry_selector<string_registry>, strings>(key, value);
  }
  template <class Policy, class Selector, class Native>
  void verify(std::shared_ptr<Native const> const & actual,
      sort_profile_array<Policy, Selector> const & expected, std::span<profile_record const> records) {
    check(actual->size() == records.size(), "sorted native cardinality");
    actual->view().scan();
    sort_profile_cursor cursor(actual->view());
    for (auto const & row : records) {
      check(!cursor.done(), "sorted native omitted record");
      auto item = cursor.peek();
      check(compare_bits(item.key.prefix, row.key.view()) == 0 &&
        compare_bits(item.value, row.value.view()) == 0, "sorted native changed key or value");
      cursor.advance();
    }
    check(cursor.done(), "sorted native added record");
    auto encoded = encoded_sort_sections<Policy>::from(expected);
    std::vector<std::byte> wanted;
    for (auto chunk : encoded.chunks()) wanted.insert(wanted.end(), chunk.begin(), chunk.end());
    if (auto owned = actual->owned()) {
      auto output = encoded_sort_sections<Policy>::from(*owned);
      std::vector<std::byte> bytes;
      for (auto chunk : output.chunks()) bytes.insert(bytes.end(), chunk.begin(), chunk.end());
      check(bytes == wanted, "owning sorted native changed canonical encoding");
    } else {
      check(bool(actual->sealed()), "streamed sorted native lacks acknowledged seal");
      auto source = file<Policy>::open(actual->sealed()->receipt.path);
      check(std::ranges::equal(source.body().bytes(), wanted), "streamed sorted native changed canonical encoding");
      actual->mapped()->scan();
    }
  }

  void strings_and_adaptation() {
    temporary dir; auto catalog = sqlite_catalog<P>::create_taps(dir.root, id());
    auto disk = storage::open_for_schema(dir.root, "initial-strings/1");
    disk.check_schema("initial-strings/1");
    std::vector<profile_record> records;
    sort_profile_writer<P> reference;
    for (unsigned n = 0; n != 65; ++n) {
      auto key = std::string(600, 'p') + char(n);
      std::optional<std::string> value = n % 7 ? std::optional(std::string(n % 19, char(n))) : std::nullopt;
      records.push_back(string_record(key, value)); reference.append<strings>(key, value);
    }
    auto expected = reference.finish();
    auto owned = owned_storage::sorted_native(records);
    auto adaptive = disk.sorted_native(records);
    check(owned->owned() && adaptive->owned() && !files(dir.root) && !disk.context()->sealed_outputs(),
      "small sorted batch created singleton files");
    verify(owned, expected, records); verify(adaptive, expected, records);
    check(disk.context()->retained_output_bytes() == adaptive->owned()->retained_bytes() + sizeof(output_budget::lease),
      "sorted retained output escaped allowance accounting");
    auto context = disk.context();
    std::weak_ptr<storage::context_type> lifetime = context;
    disk = storage{}; context.reset();
    check(lifetime.expired(), "sorted output retained the execution context");
    verify(adaptive, expected, records);

    auto eager = storage::open(dir.root, {}, {}, {}, {}, {0, 0});
    auto streamed = eager.sorted_native(records);
    check(streamed->mapped() && files(dir.root) == 1 && eager.context()->sealed_outputs() == 1 &&
      !eager.context()->sealed_indexes(), "sorted slice did not produce exactly one native stream");
    verify(streamed, expected, records);
    catalog.verify_sealed(streamed->sealed()->receipt, file_kind::native_blob);
    auto empty = owned_storage::sorted_native({});
    sort_profile_writer<P> empty_reference;
    verify(empty, empty_reference.finish(), {});
    check(owned_storage::singleton(records.front())->size() == 1, "singleton delegation changed cardinality");

    auto large_value = std::string(180'001, 'v');
    std::array large{string_record("a", "small"), string_record("z", large_value)};
    sort_profile_writer<P> large_reference;
    large_reference.append<strings>("a", "small"); large_reference.append<strings>("z", large_value);
    auto larger = storage::open(dir.root);
    auto spilled = larger.sorted_native(large);
    check(spilled->mapped() && larger.context()->sealed_outputs() == 1 && files(dir.root) == 2 &&
      !larger.context()->retained_output_bytes(), "large sorted slice did not spill once");
    verify(spilled, large_reference.finish(), large);
    rejects([&] { (void)storage{}.sorted_native(records); });
  }

  struct integers {
    using encoding = bit_encoding<fixed_values<11>>;
    using key_codec = unsigned_key<13>;
    using value_codec = unsigned_value<11>;
  };
  struct bits {
    using encoding = bit_encoding<fixed_values<3>>;
    using key_codec = fc_bit_key<golomb<2>>;
    using value_codec = unsigned_value<3>;
  };
  using registry = bin<tip<strings>, bin<tip<integers>, tip<bits>>>;
  using mixed_policy = storage_policy<registry, 7, exponential_golomb<1>, 3>;
  struct mixed_selector {
    template <class Input, class F> static decltype(auto) select(Input & in, F && fn) {
      if (in.read_bits(2) != 2) throw std::invalid_argument("custom selector prefix");
      return dispatch_sort<registry>(in, std::forward<F>(fn));
    }
    template <class S, class Output> static void write(Output & out) {
      out.write_bits(2, 2); write_sort_code<registry, S>(out);
    }
  };
  void mixed_sorts() {
    using mixed_storage = sort_runtime_storage<mixed_policy, mixed_selector>;
    using mixed_file = sort_file_runtime_storage<mixed_policy, mixed_selector>;
    std::vector<profile_record> records;
    sort_profile_writer<mixed_policy, mixed_selector> reference;
    auto add = [&]<class S>(auto const & key, auto const & value) {
      records.push_back(record<mixed_policy, mixed_selector, S>(key, value));
      reference.template append<S>(key, value);
    };
    add.template operator()<strings>(std::string("a\0", 2), std::optional<std::string>("first"));
    add.template operator()<strings>(std::string("aa"), std::optional<std::string>{});
    for (unsigned n : {0, 7, 8191}) add.template operator()<integers>(std::uint64_t(n), std::uint64_t(n & 2047));
    for (unsigned n = 0; n != 12; ++n) {
      bit_string key; sort_bit_writer out(key); out.write_bits(0, n);
      add.template operator()<bits>(key, std::uint64_t(n & 7));
    }
    auto expected = reference.finish();
    auto owned = mixed_storage::sorted_native(records);
    verify(owned, expected, records);
    temporary dir; auto catalog = sqlite_catalog<mixed_policy>::create_taps(dir.root, id());
    auto disk = mixed_file::open(dir.root, {}, {}, {}, {}, {0, 0});
    auto streamed = disk.sorted_native(records);
    verify(streamed, expected, records);
    check(files(dir.root) == 1 && streamed->view().dictionary_size() == 3 &&
      !streamed->view().metadata().common_value_width, "mixed framing or selector changed");

    auto bad = records[2];
    profile_detail::resize(bad.key, bad.key.bit_size - 1);
    rejects([&] { (void)mixed_storage::sorted_native({&bad, 1}); });
    bad = records[2]; profile_detail::resize(bad.value, 10);
    rejects([&] { (void)mixed_storage::sorted_native({&bad, 1}); });
  }

  void invalid_records() {
    std::array sorted{string_record("a", "one"), string_record("b", "two")};
    for (unsigned mode = 0; mode != 7; ++mode) {
      auto records = sorted;
      if (mode == 0) std::swap(records[0], records[1]);
      if (mode == 1) records[1] = records[0];
      if (mode == 2) records[0].key.bytes[0] |= std::byte{0x80}; // Reserved registry hole.
      if (mode == 3) profile_detail::resize(records[0].key, 8); // Partial string byte.
      if (mode == 4) records[0].value = {};
      if (mode == 5) { sort_bit_writer out(records[0].value); out.write_bits(0, 1); }
      if (mode == 6) records[0].key.bytes.back() |= std::byte{1}; // Noncanonical tail padding.
      rejects([&] { (void)owned_storage::sorted_native(records); });
      temporary dir; auto catalog = sqlite_catalog<P>::create_taps(dir.root, id());
      auto disk = storage::open(dir.root);
      rejects([&] { (void)disk.sorted_native(records); });
      check(disk.context()->failed() && !disk.context()->sealed_outputs() && !files(dir.root),
        "invalid small input published output or left a healthy context");
      rejects([&] { (void)disk.sorted_native(sorted); });
    }
  }

  struct faults { unsigned commit = 0, commits = 0; int file = 0; bool after = false; };
  struct file_ops : posix_object_ops {
    std::shared_ptr<faults> state;
    std::ptrdiff_t write(int fd, std::span<std::byte const> bytes) noexcept {
      if (state->file == 1) { state->file = 0; errno = EIO; return -1; }
      return posix_object_ops::write(fd, bytes);
    }
    int sync_file(int fd) noexcept {
      if (state->file == 2) { state->file = 0; errno = EIO; return -1; }
      return posix_object_ops::sync_file(fd);
    }
  };
  struct catalog_ops {
    std::shared_ptr<faults> state;
    int commit(sqlite3 * db) noexcept {
      bool fail = ++state->commits == state->commit;
      if (fail && !state->after) return SQLITE_IOERR_FSYNC;
      auto code = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
      return fail && code == SQLITE_OK ? SQLITE_IOERR_FSYNC : code;
    }
  };
  void failed_output() {
    using fault_storage = sort_file_runtime_storage<P, registry_selector<string_registry>, random_object_ids, catalog_ops, file_ops>;
    std::array records{string_record("a", "one"), string_record("b", "two")};
    auto old = owned_storage::sorted_native(records);
    for (unsigned mode = 0; mode != 6; ++mode) {
      temporary dir; auto catalog = sqlite_catalog<P>::create_taps(dir.root, id());
      auto state = std::make_shared<faults>();
      if (mode < 2) state->file = int(mode + 1);
      else { state->commit = (mode - 2) / 2 + 1; state->after = mode & 1; }
      auto disk = fault_storage::open(dir.root, {}, {}, catalog_ops{state}, file_ops{{}, state}, {0, 0});
      rejects([&] { (void)disk.sorted_native(records); });
      check(disk.context()->failed() && !disk.context()->sealed_outputs(), "failed seal admitted sorted output");
      rejects([&] { (void)disk.sorted_native(records); });
      old->view().scan();
      check(old->size() == 2, "failed sorted output changed its old owner");
      auto healthy = storage::open(dir.root);
      auto retry = healthy.sorted_native(records);
      check(retry->owned() && !healthy.context()->failed(), "fresh context could not retry sorted construction");
    }
  }
}
int main() {
  try { strings_and_adaptation(); mixed_sorts(); invalid_records(); failed_output(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
