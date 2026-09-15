/**
 * \file
 * \license
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/file_index_pipeline.h>
#include <diet/index_pipeline.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>

namespace {
  using namespace diet;
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class E = std::exception, class F> void rejects(F && f) {
    bool caught = false;
    try { f(); } catch (E const &) { caught = true; }
    require(caught, "invalid file pipeline operation accepted");
  }
  object_id id(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "0123456789abcdef01234567%08x", n); return object_id(text);
  }
  object_attempt_id attempt(unsigned n) {
    char text[33]; std::snprintf(text, sizeof text, "fedcba9876543210fedcba98%08x", n); return object_attempt_id(text);
  }
  struct directory {
    std::filesystem::path path;
    directory() {
      auto pattern = (std::filesystem::temp_directory_path() / "diet-file-pipeline-XXXXXX").string();
      auto made = ::mkdtemp(pattern.data()); require(made, "file pipeline temporary directory"); path = made;
    }
    ~directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  };
  struct observed_ops : posix_object_ops {
    std::uint64_t calls = 0, writes = 0;
    bool fail_write = false;
    int open_root(std::filesystem::path const & path) { ++calls; return posix_object_ops::open_root(path); }
    std::ptrdiff_t write(int fd, std::span<std::byte const> bytes) {
      ++calls; ++writes;
      if (fail_write) { errno = EIO; return -1; }
      return posix_object_ops::write(fd, bytes);
    }
  };
  struct external_ops : observed_ops {
    external_ops() = delete;
    explicit external_ops(unsigned) {}
  };
  unsigned bit(bit_view value, std::uint64_t at) {
    at += value.offset();
    return (std::to_integer<unsigned>(value.storage()[at >> 3]) >> (7 - (at & 7))) & 1;
  }
  std::uint64_t common(bit_view a, bit_view b) {
    std::uint64_t i = 0;
    while (i != std::min(a.size(), b.size()) && bit(a, i) == bit(b, i)) ++i;
    return i;
  }
  int order(bit_view a, bit_view b) {
    auto n = common(a, b);
    if (n == std::min(a.size(), b.size())) return (a.size() > b.size()) - (a.size() < b.size());
    return bit(a, n) ? 1 : -1;
  }
  void equal(bit_view a, bit_view b) {
    require(a.size() == b.size() && common(a, b) == a.size(), "pipeline bit content mismatch");
  }
  template <class P> bit_string key(unsigned n) {
    std::string bits(35, '0');
    for (unsigned i = 16; i; --i) bits += char('0' + ((n >> (i - 1)) & 1));
    if constexpr (P::unit == profile_unit::byte) bits.append((8 - bits.size() % 8) % 8, '0');
    return bit_string::from_bits(bits);
  }
  template <class P> std::vector<profile_record> rows(unsigned count, unsigned stride, unsigned salt) {
    std::vector<profile_record> result;
    for (unsigned i = 0; i != count; ++i) {
      auto units = P::value_width.value_or((i + salt) % 7);
      std::string value(units * P::bits_per_unit, '0');
      for (std::size_t j = 0; j != value.size(); ++j) if ((j + i + salt) % 3) value[j] = '1';
      result.push_back({key<P>(i * stride), bit_string::from_bits(value)});
    }
    return result;
  }
  std::vector<bit_string> merge_keys(std::span<profile_record const> native, std::span<bit_string const> borrowed) {
    std::vector<bit_string> result;
    std::size_t n = 0, b = 0;
    while (n != native.size() || b != borrowed.size()) {
      if (b != borrowed.size() && (n == native.size() || order(borrowed[b].view(), native[n].key.view()) < 0))
        result.push_back(borrowed[b++]);
      else result.push_back(native[n++].key);
    }
    return result;
  }
  template <class P> std::vector<bit_string> samples(std::span<bit_string const> keys) {
    std::vector<bit_string> result;
    for (std::size_t i = 0; i < keys.size(); i += P::group_size) result.push_back(keys[i]);
    return result;
  }
  template <class P> std::uint64_t suffix_units(std::span<bit_string const> sample) {
    bit_view previous;
    std::uint64_t result = 0;
    for (auto const & k : sample) {
      result += (k.bit_size >> P::unit_shift) - (common(previous, k.view()) >> P::unit_shift);
      previous = k.view();
    }
    return result;
  }
  std::vector<std::byte> bytes(std::filesystem::path const & path) {
    auto file = mapped_file::open(path); auto slice = file.slice(0, file.size());
    return {slice.bytes().begin(), slice.bytes().end()};
  }
  template <class P> struct fixture {
    directory root;
    std::array<std::vector<profile_record>, 4> originals;
    std::array<std::shared_ptr<profile_blob<P> const>, 4> owning;
    std::array<std::shared_ptr<mapped_native<P> const>, 4> native;
    std::array<blob_identity, 4> ids{{{id(1), id(2)}, {id(3), id(4)}, {id(5), id(6)}, {id(7), id(8)}}};
    std::array<std::filesystem::path, 4> native_paths;
    std::array<std::vector<std::byte>, 4> native_bytes;
    std::shared_ptr<mapped_blob<P> const> target;
    std::vector<file_index_stage<P>> stages;
    explicit fixture(bool large_value = false) {
      originals[0] = rows<P>(unsigned(P::group_size * P::group_size + 3), 2, 0);
      originals[1] = rows<P>(unsigned(P::group_size + 4), unsigned(2 * P::group_size), 1);
      originals[2] = rows<P>(2, unsigned(2 * P::group_size), 2);
      if (large_value) {
        require(!P::fixed_width, "guard fixture requires variable values");
        originals[1][0].value = bit_string::from_bytes(std::string(128 * 1024, 'v'));
      }
      for (std::size_t i = 0; i != originals.size(); ++i) {
        owning[i] = std::make_shared<profile_blob<P> const>(profile_blob<P>::build(originals[i]));
        auto sealed = encode_native_sections(owning[i]->native()).seal(root.path, ids[i].native, attempt(unsigned(i + 1)));
        native_paths[i] = sealed.path;
        native_bytes[i] = bytes(sealed.path);
        native[i] = std::make_shared<mapped_native<P> const>(mapped_native<P>::open(sealed.path));
      }
      auto sealed = encode_index_sections(*owning[0], ids[0].native).seal(root.path, ids[0].index, attempt(9));
      auto index = std::make_shared<mapped_index<P> const>(mapped_index<P>::open(sealed.path));
      target = mapped_blob<P>::bind(ids[0], native[0], index);
      for (std::size_t i = 1; i != originals.size(); ++i)
        stages.push_back({native[i], ids[i], attempt(unsigned(i + 10))});
    }
    std::vector<std::shared_ptr<profile_blob<P> const>> owning_stages() const {
      return {owning[1], owning[2], owning[3]};
    }
  };
  template <class P, class Pipeline> void verify_counters(Pipeline const & pipeline, fixture<P> const & f) {
    std::vector<bit_string> augmented;
    for (auto const & row : f.originals[0]) augmented.push_back(row.key);
    auto source = samples<P>(augmented);
    require(pipeline.source_suffix_units() == suffix_units<P>(source), "source suffix accounting mismatch");
    auto const & work = pipeline.source_work();
    require(work.native_entries == augmented.size() && !work.borrowed_entries && work.consumed_entries() == augmented.size(),
      "source sampler occurrence accounting mismatch");
    for (std::size_t i = 0; i != 3; ++i) {
      auto borrowed = samples<P>(augmented);
      augmented = merge_keys(f.originals[i + 1], borrowed);
      auto output = samples<P>(augmented);
      require(pipeline.consumed_entries()[i] == augmented.size() && pipeline.emitted_samples()[i] == output.size() &&
        pipeline.emitted_suffix_units()[i] == suffix_units<P>(output), "stage accounting mismatch");
    }
    require(augmented.size() <= P::group_size, "fixture head not bounded");
  }
  template <class P> void verify_queries(std::shared_ptr<mapped_blob<P> const> head, fixture<P> const & f) {
    auto root = mapped_query_root<P>::adopt_prepared(head);
    std::vector<bit_string> queries{bit_string{}, bit_string::from_bits(std::string(P::bits_per_unit, '1'))};
    for (auto const & rows : f.originals) for (auto const & row : rows) queries.push_back(row.key);
    queries.push_back(key<P>(65535));
    for (auto const & query : queries) {
      struct match { blob_identity identity; std::uint64_t ordinal; bit_view value; };
      std::vector<match> expected;
      for (std::size_t level = f.originals.size(); level--;) {
        auto const & rows = f.originals[level];
        for (std::size_t i = 0; i != rows.size(); ++i)
          if (!order(query.view(), rows[i].key.view())) expected.push_back({f.ids[level], i, rows[i].value.view()});
      }
      auto cursor = root.cursor(query.view());
      std::size_t i = 0;
      while (!cursor.done()) {
        require(cursor.step(1) <= 1, "query catalog budget exceeded");
        if (cursor.has_match()) {
          auto actual = cursor.take_match();
          require(i != expected.size() && actual.source->identity() == expected[i].identity && actual.ordinal == expected[i].ordinal,
            "mapped pipeline query source/order/ordinal mismatch");
          equal(actual.value.view(), expected[i++].value);
        }
      }
      require(i == expected.size(), "mapped pipeline omitted a native match");
    }
  }
  template <class P> void matrix(bool protect_value = false) {
    fixture<P> f(protect_value);
    index_pipeline<P> owning(f.owning[0], f.owning_stages());
    file_index_pipeline<P> pipeline(f.root.path, f.target, f.stages);
    require(pipeline.planned_head() == f.ids.back() && pipeline.size() == 3 && !pipeline.failed() && !pipeline.finished(), "initial pipeline state");
    require(pipeline.completed_receipts().empty(), "unsealed pipeline has receipt");
    rejects<std::logic_error>([&] { pipeline.seal_next(); });
    rejects<std::out_of_range>([&] { (void)pipeline.paths(3); });
    void * protected_page = nullptr;
    std::size_t page = 0;
    if (protect_value) {
      auto n = ::sysconf(_SC_PAGESIZE); require(n > 0, "page size unavailable"); page = std::size_t(n);
      auto value = f.native[1]->view().encoded_at(0).value;
      auto first = reinterpret_cast<std::uintptr_t>(value.storage().data()) + (value.offset() >> 3);
      auto aligned = (first + page - 1) & ~std::uintptr_t(page - 1);
      require(aligned + page <= first + (value.size() >> 3), "large value lacks an interior page");
      protected_page = reinterpret_cast<void *>(aligned);
      require(::mprotect(protected_page, page, PROT_NONE) == 0, "protect native value page");
    }
    std::uint64_t turns = 0;
    while (!pipeline.done()) {
      require(!owning.done(), "owning pipeline drained before streamed pipeline");
      require(pipeline.step(0) == 0 && owning.step(0) == 0, "zero quantum made progress");
      auto budget = turns++ % 5 ? 1u : 7u;
      auto actual = pipeline.step(budget), expected = owning.step(budget);
      require(actual && actual <= budget && actual == expected, "shared pump work/backpressure changed");
      require(turns < 1000000, "pipeline failed to drain");
    }
    require(owning.done() && pipeline.step(1) == 0, "drained pipeline performed work");
    verify_counters(pipeline, f); verify_counters(owning, f);
    auto expected_head = owning.finish();
    require(owning.finish() == expected_head && owning.finished(), "owning finish identity changed");
    std::array<std::shared_ptr<profile_blob<P> const>, 3> expected;
    for (std::size_t i = 3; i--;) { expected[i] = expected_head; expected_head = expected_head->target(); }
    auto target = f.target;
    for (std::size_t i = 0; i != 3; ++i) {
      require(pipeline.seal_next(), "stage failed to seal");
      auto receipts = pipeline.completed_receipts();
      require(receipts.size() == i + 1 && receipts.back().object == f.ids[i + 1].index, "receipt order mismatch");
      auto const & receipt = receipts.back();
      auto reference = encode_index_sections(*expected[i], f.ids[i + 1].native, target->identity()).materialize();
      require(bytes(receipt.path) == reference, "streamed pipeline full .index bytes differ");
      auto index = std::make_shared<mapped_index<P> const>(mapped_index<P>::open(receipt.path));
      target = mapped_blob<P>::bind(f.ids[i + 1], f.native[i + 1], index, target);
    }
    require(pipeline.finished() && !pipeline.seal_next() && pipeline.finish().size() == 3, "final pipeline state/finish retry");
    rejects<std::logic_error>([&] { pipeline.step(0); });
    if (protect_value) require(::mprotect(protected_page, page, PROT_READ) == 0, "restore native value page");
    target->scan(); verify_queries(target, f);
    for (std::size_t i = 0; i != f.native.size(); ++i)
      require(bytes(f.native_paths[i]) == f.native_bytes[i], "file index pipeline modified native bytes");
  }
  void invalid_and_zero_stages() {
    using P = storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<fixed_values<0>>>>, 3, exponential_golomb<0>, 16>;
    fixture<P> f;
    auto reject = [&](auto target, auto stages) {
      observed_ops ops;
      rejects<std::invalid_argument>([&] { file_index_pipeline<P, observed_ops> bad(f.root.path, target, stages, ops); });
      require(ops.calls == 0, "invalid stage list opened private outputs");
    };
    reject(std::shared_ptr<mapped_blob<P> const>{}, f.stages);
    auto bad = f.stages; bad[1].source.reset(); reject(f.target, bad);
    bad = f.stages; bad[1].identity.index = bad[0].identity.index; reject(f.target, bad);
    bad = f.stages; bad[1].identity.index = f.target->identity().index; reject(f.target, bad);
    bad = f.stages; auto moved_native = std::move(bad[0].identity.native); (void)moved_native; reject(f.target, bad);
    bad = f.stages; auto moved_index = std::move(bad[0].identity.index); (void)moved_index; reject(f.target, bad);
    bad = f.stages; auto moved_attempt = std::move(bad[0].attempt); (void)moved_attempt; reject(f.target, bad);
    auto target_identity = f.target->identity();
    std::weak_ptr<mapped_blob<P> const> pin = f.target;
    {
      observed_ops ops;
      file_index_pipeline<P, observed_ops> empty(f.root.path, f.target, {}, ops);
      f.target.reset();
      require(!pin.expired() && empty.done() && !empty.finished() && !empty.size() &&
        empty.planned_head() == target_identity && empty.stage_identities().empty() && ops.calls == 0,
        "zero-stage identity/pin behavior");
      require(empty.step(0) == 0 && empty.step(1) == 0 && !empty.seal_next() && empty.finished() && empty.finish().empty(),
        "zero-stage completion behavior");
      rejects<std::logic_error>([&] { empty.step(1); });
    }
    require(pin.expired(), "zero-stage pipeline leaked target pin");
    index_pipeline<P> unchanged(f.owning[0], {});
    auto moved = std::move(unchanged);
    require(moved.done() && moved.finish() == f.owning[0], "owning extraction lost move/identity behavior");
    static_assert(!std::is_move_constructible_v<file_index_pipeline<P>>);
    static_assert(!std::is_copy_constructible_v<file_index_pipeline<P>>);
  }
  void partial_seal_failure() {
    using P = storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 7, golomb<3>, 16>;
    fixture<P> f;
    std::weak_ptr<mapped_blob<P> const> pin = f.target;
    std::array<std::weak_ptr<mapped_native<P> const>, 3> sources{f.native[1], f.native[2], f.native[3]};
    external_ops ops(1);
    {
      file_index_pipeline<P, external_ops> pipeline(f.root.path, f.target, f.stages, ops);
      f.target.reset(); f.stages.clear();
      for (std::size_t i = 1; i != f.native.size(); ++i) {
        f.native[i].reset(); std::filesystem::remove(f.native_paths[i]);
      }
      while (!pipeline.done()) require(pipeline.step(1) == 1, "external Ops pipeline stalled");
      require(!pin.expired() && std::all_of(sources.begin(), sources.end(), [](auto const & p) { return !p.expired(); }),
        "pipeline lost active source/target pins");
      require(pipeline.seal_next(), "first stage did not seal");
      auto first = pipeline.completed_receipts().front();
      auto first_wire = bytes(first.path);
      auto target = pin.lock();
      auto first_index = std::make_shared<mapped_index<P> const>(mapped_index<P>::open(first.path));
      auto first_pair = mapped_blob<P>::bind(f.ids[1], sources[0].lock(), first_index, target);
      first_pair->scan();
      ops.fail_write = true;
      rejects<object_write_error>([&] { pipeline.seal_next(); });
      require(pipeline.failed() && !pipeline.finished() && pipeline.completed_receipts().size() == 1 &&
        pipeline.completed_receipts()[0].object == first.object && bytes(first.path) == first_wire,
        "later sealing failure lost or changed an earlier receipt");
      require(std::filesystem::exists(pipeline.paths(1).private_output) &&
        std::filesystem::exists(pipeline.paths(2).private_output), "failure discarded unfinished attempts");
      auto calls = ops.calls;
      rejects<std::logic_error>([&] { pipeline.seal_next(); });
      rejects<std::logic_error>([&] { pipeline.finish(); });
      rejects<std::logic_error>([&] { pipeline.step(0); });
      require(calls == ops.calls, "failed pipeline performed more I/O");
    }
    require(pin.expired() && std::all_of(sources.begin(), sources.end(), [](auto const & p) { return p.expired(); }),
      "failed pipeline leaked retained input pins");
  }
}
#endif

int main() {
  try {
#if defined(__APPLE__) || defined(__linux__)
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 3, diet::exponential_golomb<0>, 16>>(true);
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<diet::fixed_values<0>>>>, 7, diet::exponential_golomb<0>, 15>>();
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<diet::fixed_values<8>>>>, 15, diet::exponential_golomb<0>, 16>>();
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 31, diet::exponential_golomb<0>, 15>>();
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 3, diet::golomb<3>, 16>>();
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<diet::fixed_values<0>>>>, 7, diet::exponential_golomb<2>, 15>>();
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<diet::fixed_values<13>>>>, 15, diet::golomb<5>, 16>>();
    matrix<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 31, diet::exponential_golomb<0>, 1>>();
    invalid_and_zero_stages(); partial_seal_failure();
#endif
  } catch (std::exception const & e) { std::cerr << e.what() << '\n'; return 1; }
}
