/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Exercises durable sort-owned records, mixed arrows and hidden redundant artifacts.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <everett/connection.h>
#include <everett/typed_scan.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <sys/mman.h>
#include <unistd.h>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using family = sort_runtime_family<>;
  using core = typed_engine<string_policy, wrapping_fingerprint_algebra, 256, family>;
  struct temporary {
    std::filesystem::path root;
    temporary() {
      auto pattern = (std::filesystem::temp_directory_path() / "everett-sort-store-XXXXXX").string();
      if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
      root = pattern;
    }
    ~temporary() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  };
  template <class F> void rejects(F && action) {
    bool caught = false; try { action(); } catch (std::exception const &) { caught = true; }
    assert(caught);
  }
  std::size_t files(std::filesystem::path const & root) {
    std::size_t count = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root))
      if (entry.path().extension() == ".kv" || entry.path().extension() == ".index") ++count;
    return count;
  }
  template <class S = strings, class World, class Map> void verify(World const & snapshot, Map const & values) {
    auto cursor = everett::scan<S>(snapshot); auto expected = values.begin();
    while (auto row = cursor.next()) {
      assert(expected != values.end() && row->key == expected->first && row->value == expected->second);
      assert(snapshot.template get<S>(row->key) == row->value); ++expected;
    }
    assert(expected == values.end());
  }
  void check_files(std::filesystem::path const & root) {
    unsigned count = 0;
    for (auto const & entry : std::filesystem::recursive_directory_iterator(root)) if (entry.path().extension() == ".kv") {
      auto native = file<string_policy>::open(entry.path()); auto slice = native.body(); auto body = slice.bytes();
      assert(body[0] == std::byte{'K'} && body[1] == std::byte{'V'} && body[2] == std::byte{'0'} && body[3] == std::byte{'3'});
      ++count;
    }
    assert(count);
  }
  void durable_strings() {
    temporary dir;
    std::map<std::string, std::string> expected;
    {
      auto live = connect<core>(dir.root, "earth-616");
      live.put("key", "first"); auto initial = live.snapshot();
      live.save("before", initial); auto count = files(dir.root);
      auto branch = live.fork("earth-617", initial); assert(files(dir.root) == count);
      branch.put("key", "branch");
      for (unsigned n = 0; n != 19; ++n) {
        auto key = n % 3 ? std::string("a\0", 2) + std::to_string(n % 7) : std::string(n % 5, '\0');
        auto value = "value-" + std::to_string(n); live.put(key, value); expected[key] = value;
      }
      live.erase("key"); assert(!live.get("key") && initial.get("key") == "first");
      verify(live.snapshot(), expected); assert(branch.get("key") == "branch");
      check_files(dir.root);
    }
    auto reopened = connect<core>(dir.root, "earth-616", {.create_if_missing = false});
    verify(reopened.snapshot(), expected);
    auto old = reopened.load("before"); assert(old && old->get("key") == "first");
    rejects([&] { (void)connect<core>(dir.root, "earth-616", {.schema_id = "another-schema"}); });
    rejects([&] { (void)connect<typed_engine<string_policy>>(dir.root, "earth-616"); }); // Opaque native family cannot reinterpret KV03.
  }
  void protected_payload() {
    temporary dir; auto active = persistent_engine<core>::connect(dir.root, "protected");
    auto current = active.contribute(core::put("key", std::string(32768, 'v')));
    auto object = current.runtime().frontier().root.main;
    assert(object && object->pair);
    auto id = object->pair->mapped()->identity().native;
    auto path = dir.root / object_path(id, file_kind::native_blob);
    auto mapping = mapped_file::open(path); auto slice = mapping.slice(0, mapping.size());
    auto bytes = slice.bytes(); auto body = bytes.subspan(file_detail::header_bytes);
    auto offset = file_detail::header_bytes + file_detail::get(body, 64, 8), length = file_detail::get(body, 72, 8);
    auto page = std::uint64_t(::sysconf(_SC_PAGESIZE));
    auto first = ((offset + page - 1) / page) * page, last = ((offset + length) / page) * page;
    assert(last > first);
    auto address = const_cast<std::byte *>(bytes.data()) + first;
    assert(::mprotect(address, last - first, PROT_NONE) == 0);
    struct protection {
      void * address; std::size_t bytes;
      ~protection() { assert(::mprotect(address, bytes, PROT_READ) == 0); }
    } protected_pages{address, static_cast<std::size_t>(last - first)};
    auto native = mapped_sort_profile<string_policy>::from_slice(slice);
    assert(native.size() == 1 && native.view().metadata().version == 3);
    // Normal profile admission never touches the protected FC payload pages.
  }
  void hidden_stages() {
    using P = storage_policy<string_registry, 3>;
    using F = sort_runtime_family<P>;
    using E = typed_engine<P, wrapping_fingerprint_algebra, 256, F>;
    using Runtime = typename F::template runtime_type<replace_native_value>;
    using Store = sort_runtime_store<P>;
    temporary dir; auto storage = Store::create(dir.root); Runtime active;
    auto current = storage.create_session("stages", active.snapshot());
    auto a = E::put("a", "one"), b = E::put("b", "two");
    active.try_contribute(a.records()[0]); active.try_contribute(b.records()[0]);
    unsigned seen = 0, calls = 0;
    while (active.pending()) {
      assert(++calls < 10000);
      auto snapshot = active.checkpoint(); unsigned phase = 0;
      for (auto const & level : snapshot.frontier().levels) if (level.job) phase |= 1u << unsigned(level.job->stage);
      if (phase & ~seen) {
        current = storage.publish(current.head, snapshot);
        auto count = files(dir.root);
        current = storage.publish(current.head, snapshot); assert(files(dir.root) == count);
        auto reopened = Store::open(dir.root); auto saved = reopened.find("stages"); assert(saved && saved->head == current.head);
        for (auto const & object : runtime_storage_codec<F>::objects(saved->snapshot.frontier())) {
          assert(object->native->mapped() && !object->native->owned());
          if (object->pair) assert(object->pair->native_owner() == object->native);
        }
        auto resumed = Runtime::from_snapshot(saved->snapshot);
        assert(!resumed.admission_ready());
        while (resumed.pending()) resumed.advance(37);
        for (auto const & [key, value] : std::map<std::string, std::string>{{"a", "one"}, {"b", "two"}}) {
          auto encoded = F::key_transport::template encode<strings>(key);
          auto query = resumed.snapshot().cursor(encoded.view()); bool found = false;
          while (!query.done()) { query.step(1); if (query.has_match()) {
            auto match = query.take_match(); assert((typed_detail::value<P, strings>(match.value.view()) == value)); found = true; break;
          } }
          assert(found);
        }
        mapped_cola_scan<mapped_sort_cola<P>> scan;
        scan(*saved->snapshot.query_root().head()->mapped());
        for (auto const & object : runtime_storage_codec<F>::objects(saved->snapshot.frontier()))
          if (object->pair) scan(*object->pair->mapped());
        seen |= phase;
      }
      if (!active.pending()) break;
      auto price = active.next_service_cost(), credit = active.credit(); active.advance(price > credit ? price - credit : 1);
    }
    assert(seen == 15);
  }
  void deferred_payload_scan() {
    temporary dir; std::filesystem::path path; std::uint64_t offset;
    {
      auto active = persistent_engine<core>::connect(dir.root, "deferred");
      auto current = active.contribute(core::put("key", std::string(32768, 'v')));
      auto native = current.runtime().frontier().root.main->pair->mapped()->identity().native;
      path = dir.root / object_path(native, file_kind::native_blob);
      auto source = file<string_policy>::open(path); auto slice = source.body(); auto body = slice.bytes();
      offset = file_detail::header_bytes + file_detail::get(body, 64, 8) + 8192;
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    {
      std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
      file.seekg(static_cast<std::streamoff>(offset)); char value; file.read(&value, 1); assert(file);
      value ^= 1; file.seekp(static_cast<std::streamoff>(offset)); file.write(&value, 1); file.flush(); assert(file);
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);
    auto reopened = persistent_engine<core>::connect(dir.root, "deferred", {.create_if_missing = false});
    auto current = reopened.snapshot(); assert(current.live_count() == 1);
    auto root = current.runtime().query_root().head()->mapped();
    mapped_cola_scan<mapped_sort_cola<string_policy>> scan;
    rejects([&] { scan(*root); }); // Full recovery detects the payload checksum failure.
    rejects([&] { scan(*root); }); // A failed context cannot silently skip a partial scan.
  }
  struct sums {
    using encoding = bit_encoding<fixed_values<64>>;
    using key_codec = unsigned_key<32>;
    using value_codec = unsigned_value<64>;
    using state_type = std::uint64_t;
    static state_type initial(std::uint64_t) { return 0; }
    static state_type apply(std::uint64_t, state_type old, state_type delta) { return old + delta; }
    static state_type compose(std::uint64_t, state_type old, state_type delta) { return old + delta; }
    static bool present(std::uint64_t, state_type value) { return value != 0; }
    static std::uint64_t hash_key(std::uint64_t key) { return key + 19; }
    static std::uint64_t hash_value(std::uint64_t, state_type value) { return value + 23; }
  };
  struct arrows {
    using encoding = bit_encoding<>;
    using key_codec = fc_string_key<golomb<3>>;
    using value_codec = string_value<>;
    using state_type = std::string;
    static state_type initial(std::string const &) { return {}; }
    static state_type apply(std::string const &, state_type old, state_type const & delta) { return old + delta; }
    static state_type compose(std::string const &, state_type old, state_type const & delta) { return old + delta; }
    static bool present(std::string const &, state_type const & value) { return !value.empty(); }
    static std::uint64_t hash_key(std::string const & key) { return u64_table_hash{}.key(key); }
    static std::uint64_t hash_value(std::string const &, state_type const & value) { return u64_table_hash{}.key(value); }
  };
  void mixed_arrows() {
    using P = storage_policy<bin<tip<strings>, bin<tip<sums>, tip<arrows>>>, 3>;
    using E = typed_engine<P, wrapping_fingerprint_algebra, 256, sort_runtime_family<P>>;
    temporary dir;
    connection_options options{.schema_id = "application/sorts/blue"};
    std::map<std::string, std::string> text, sequence;
    std::map<std::uint64_t, std::uint64_t> counts;
    std::uint64_t signature = 0;
    for (unsigned phase = 0; phase != 3; ++phase) {
      auto live = connect<E>(dir.root, "mixed", options);
      if (phase) assert(live.snapshot().signature() == signature);
      for (unsigned n = phase * 5; n != (phase + 1) * 5; ++n) {
        auto key = std::string("a\0", 2), delta = "[" + std::to_string(n) + "]";
        auto batch = E::batch();
        batch.put<strings>(std::to_string(n % 4), delta).change<sums>(n % 5, n + 1).change<arrows>(key, delta);
        live.apply(std::move(batch).finish());
        text[std::to_string(n % 4)] = delta; counts[n % 5] += n + 1; sequence[key] += delta;
      }
      auto snapshot = live.snapshot(); signature = snapshot.signature();
      verify<strings>(snapshot, text); verify<sums>(snapshot, counts); verify<arrows>(snapshot, sequence);
      mapped_cola_scan<mapped_sort_cola<P>> scan;
      scan(*snapshot.runtime().query_root().head()->mapped());
    }
    rejects([&] { (void)connect<E>(dir.root, "mixed"); });
  }
}
int main() {
  try { durable_strings(); protected_payload(); hidden_stages(); deferred_payload_scan(); mixed_arrows(); }
  catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }
}
