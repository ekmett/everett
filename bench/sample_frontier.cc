/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/index_pipeline.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace {
  using namespace diet;
  using clock_type = std::chrono::steady_clock;
  void require(bool ok, char const * message) { if (!ok) throw std::runtime_error(message); }
  double elapsed(clock_type::time_point start) {
    return std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
  }
  struct digest {
    std::uint64_t value = 0xcbf29ce484222325ull;
    void byte(unsigned n) { value = (value ^ n) * 0x100000001b3ull; }
    void integer(std::uint64_t n) { for (unsigned i = 0; i != 8; ++i) byte(unsigned(n >> (i << 3)) & 255); }
    template <class R> void words(R const & values) { integer(values.size()); for (auto v : values) integer(v); }
    template <class R> void bytes(R const & values) {
      integer(values.size()); for (auto v : values) byte(std::to_integer<unsigned>(v));
    }
  };
  template <class P> bit_string key_for(std::uint64_t id, unsigned prefix) {
    std::string bytes(prefix, 'p');
    for (unsigned i = 8; i; --i) bytes.push_back(char(id >> ((i - 1) << 3)));
    auto key = bit_string::from_bytes(bytes);
    if constexpr (P::unit == profile_unit::bit) {
      key.bytes.push_back(id & 1 ? std::byte{0xa0} : std::byte{0x40}); key.bit_size += 3;
    }
    return key;
  }
  bool bit(bit_string const & key, std::uint64_t i) {
    return (std::to_integer<unsigned>(key.bytes[i >> 3]) >> (7 - (i & 7))) & 1;
  }
  void equal_key(bit_view actual, bit_string const & expected) {
    require(actual.size() == expected.bit_size, "sample key extent differs");
    // Cursor scratch is canonical, byte-aligned and fully reconstructed.
    require(actual.offset() == 0 && actual.storage().size() == expected.bytes.size(), "sample key storage differs");
    require(std::equal(actual.storage().begin(), actual.storage().end(), expected.bytes.begin()), "sample key bytes differ");
  }
  std::uint64_t tail(bit_view key) {
    return profile_detail::load_bits(key, key.size() - 64, 64);
  }
  std::uint64_t oracle_tail(bit_string const & key) {
    std::uint64_t n = 0;
    for (auto i = key.bit_size - 64; i != key.bit_size; ++i) n = (n << 1) | bit(key, i);
    return n;
  }
  struct occurrence { std::uint64_t id, ordinal; stream_role role; };
  std::vector<occurrence> merge_order(std::span<std::uint64_t const> native,
                                     std::span<std::uint64_t const> borrowed) {
    std::vector<occurrence> order;
    for (std::size_t i = 0; i != native.size(); ++i) order.push_back({native[i], i, stream_role::native});
    for (std::size_t i = 0; i != borrowed.size(); ++i) order.push_back({borrowed[i], i, stream_role::borrowed});
    // Stable equal IDs retain native-first ties and borrowed occurrence order.
    std::stable_sort(order.begin(), order.end(), [](auto a, auto b) { return a.id < b.id; });
    return order;
  }
  template <class P> std::vector<std::uint64_t> sample_ids(std::vector<occurrence> const & order) {
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < order.size(); i += P::group_size) ids.push_back(order[i].id);
    return ids;
  }
  template <class P, stream_role R> void equal_array(profile_array<P, R> const & a, profile_array<P, R> const & b) {
    require(std::equal(a.bytes().begin(), a.bytes().end(), b.bytes().begin(), b.bytes().end()), "pipeline payload differs from batch");
    auto const & x = a.metadata(); auto const & y = b.metadata();
    require(x.record_count == y.record_count && x.extent == y.extent && x.terminal_key_units == y.terminal_key_units &&
      x.common_value_width == y.common_value_width, "pipeline metadata differs from batch");
    auto const & e = a.group_offsets(); auto const & f = b.group_offsets();
    require(e.low == f.low && e.high == f.high && e.sparse == f.sparse && e.entry_count == f.entry_count &&
      e.universe == f.universe && e.low_width == f.low_width && e.samples.size() == f.samples.size(), "pipeline EF differs");
    for (std::size_t i = 0; i != e.samples.size(); ++i)
      require(e.samples[i].first == f.samples[i].first && e.samples[i].sparse == f.samples[i].sparse, "pipeline EF samples differ");
  }
  template <class P, stream_role R> void hash_array(digest & d, profile_array<P, R> const & a) {
    auto const & m = a.metadata(); auto const & e = a.group_offsets();
    d.integer(m.record_count); d.integer(m.extent); d.integer(m.terminal_key_units);
    d.integer(m.common_value_width.has_value()); d.integer(m.common_value_width.value_or(0));
    d.bytes(a.bytes()); d.words(e.low); d.words(e.high); d.words(e.sparse);
    d.integer(e.entry_count); d.integer(e.universe); d.integer(e.low_width); d.integer(e.samples.size());
    for (auto s : e.samples) { d.integer(s.first); d.integer(s.sparse); }
  }
  template <class P> struct fixture {
    using blob = profile_blob<P>; using pair = std::shared_ptr<blob const>;
    pair source;
    std::vector<pair> stages;
    std::vector<blob> expected;
    std::vector<std::vector<std::uint64_t>> expected_borrowed;
    std::vector<std::uint64_t> consumed, emitted;
    std::vector<occurrence> order;
    std::uint64_t native_count = 0, borrowed_count = 0, comparisons = 0, sample_checksum = 0;
    unsigned prefix;
    std::vector<profile_record> records(std::span<std::uint64_t const> ids) {
      std::vector<profile_record> result;
      for (auto id : ids) result.push_back({key_for<P>(id, prefix), bit_string::from_bytes(std::to_string(id))});
      return result;
    }
    std::vector<bit_string> keys(std::span<std::uint64_t const> ids) {
      std::vector<bit_string> result;
      for (auto id : ids) result.push_back(key_for<P>(id, prefix));
      return result;
    }
    fixture(unsigned count, unsigned prefix_bytes, bool duplicates) : prefix(prefix_bytes) {
      std::vector<std::uint64_t> native, borrowed;
      for (unsigned i = 0; i != count; ++i) {
        auto id = std::uint64_t(i) * 4 + 1; native.push_back(id);
        if (duplicates) {
          auto repeats = i % 32 == 0 ? 19 : 2;
          for (int n = 0; n != repeats; ++n) borrowed.push_back(id);
          borrowed.push_back(id + 1);
        } else { borrowed.push_back(id - 1); borrowed.push_back(id + 1); }
      }
      native_count = native.size(); borrowed_count = borrowed.size(); order = merge_order(native, borrowed);
      source = std::make_shared<blob const>(blob::build(records(native), keys(borrowed)));
      std::size_t ni = 0, bi = 0;
      for (auto const & o : order) {
        if (ni != native.size() && bi != borrowed.size()) ++comparisons;
        if (o.role == stream_role::native) ++ni; else ++bi;
      }
      for (std::size_t i = 0; i != borrowed.size(); ++i)
        require(source->view().false_borrow(i) == std::binary_search(native.begin(), native.end(), borrowed[i]),
                "false borrow differs from integer membership");
      digest samples;
      for (std::size_t i = 0; i < order.size(); i += P::group_size) {
        auto const & o = order[i]; auto key = key_for<P>(o.id, prefix);
        samples.integer(i); samples.integer(o.ordinal); samples.integer(unsigned(o.role));
        samples.integer(key.bit_size); samples.integer(oracle_tail(key));
      }
      sample_checksum = samples.value;
      auto incoming = sample_ids<P>(order);
      for (unsigned level = 0; level != 3; ++level) {
        std::vector<std::uint64_t> ids;
        auto stride = std::uint64_t{16} << (level << 1);
        for (unsigned i = 0; i != (count >> ((level + 1) << 1)); ++i) ids.push_back(i * stride + 1);
        auto stage = std::make_shared<blob const>(blob::build(records(ids)));
        expected.push_back(stage->reindex(keys(incoming)));
        expected_borrowed.push_back(incoming);
        stages.push_back(std::move(stage));
        auto next = merge_order(ids, incoming); consumed.push_back(next.size());
        incoming = sample_ids<P>(next); emitted.push_back(incoming.size());
      }
    }
    void verify_work(sampling_work const & work) const {
      require(work.native_entries == native_count && work.borrowed_entries == borrowed_count &&
        work.decoded_entries == order.size() && work.key_comparisons == comparisons, "sampler counters differ from integer oracle");
    }
    void verify_samples() const {
      sample_cursor<P> cursor(source);
      std::size_t i = 0;
      while (!cursor.done()) {
        auto sample = cursor.peek(); auto const & o = order[i];
        require(sample.target_ordinal == i && sample.source_ordinal == o.ordinal && sample.source_role == o.role,
                "sample tagged occurrence differs");
        equal_key(sample.key, key_for<P>(o.id, prefix));
        cursor.advance(); i += std::min<std::size_t>(P::group_size, order.size() - i);
      }
      require(i == order.size(), "sample traversal ended early"); verify_work(cursor.counters());
    }
    std::uint64_t verify_pipeline(pair current, index_pipeline<P> const & pipeline) const {
      verify_work(pipeline.source_work());
      require(std::equal(consumed.begin(), consumed.end(), pipeline.consumed_entries().begin(), pipeline.consumed_entries().end()) &&
        std::equal(emitted.begin(), emitted.end(), pipeline.emitted_samples().begin(), pipeline.emitted_samples().end()), "pipeline counters differ");
      digest d;
      d.words(pipeline.consumed_entries()); d.words(pipeline.emitted_samples());
      d.words(pipeline.emitted_suffix_units()); d.integer(pipeline.source_suffix_units());
      for (std::size_t i = stages.size(); i; --i) {
        auto const & e = expected[i - 1];
        require(current && &current->native() == &stages[i - 1]->native(), "pipeline lost shared native allocation");
        equal_array(current->native(), e.native()); equal_array(current->borrowed(), e.borrowed());
        require(current->interleave().classes == e.interleave().classes && current->interleave().checkpoints == e.interleave().checkpoints,
                "pipeline ranks differ from batch");
        require(std::equal(current->cut_lcps().begin(), current->cut_lcps().end(), e.cut_lcps().begin(), e.cut_lcps().end()) &&
          std::equal(current->false_borrow_bits().begin(), current->false_borrow_bits().end(), e.false_borrow_bits().begin(), e.false_borrow_bits().end()),
          "pipeline cut/false-borrow metadata differs from batch");
        auto cursor = current->borrowed().view().cursor();
        for (auto id : expected_borrowed[i - 1]) {
          require(!cursor.done(), "pipeline borrowed stream ended early"); equal_key(cursor.peek().key.prefix, key_for<P>(id, prefix)); cursor.advance();
        }
        require(cursor.done(), "pipeline borrowed stream has excess entries");
        hash_array(d, current->native()); hash_array(d, current->borrowed());
        d.words(current->interleave().classes); d.words(current->interleave().checkpoints);
        d.words(current->cut_lcps()); d.bytes(current->false_borrow_bits());
        current = current->target();
      }
      require(current == source, "pipeline lost exact source pin"); return d.value;
    }
  };
  template <class P> void run(char const * profile, unsigned count, unsigned prefix, unsigned rounds, bool duplicates) {
    fixture<P> input(count, prefix, duplicates);
    input.verify_samples();
    index_pipeline<P> warm(input.source, input.stages);
    while (!warm.done()) warm.step(4096);
    auto warm_result = warm.finish(); auto expected_wire = input.verify_pipeline(warm_result, warm);
    for (unsigned round = 0; round != rounds; ++round) {
      auto start = clock_type::now(); sample_cursor<P> cursor(input.source); digest checksum;
      while (!cursor.done()) {
        auto s = cursor.peek(); checksum.integer(s.target_ordinal); checksum.integer(s.source_ordinal);
        checksum.integer(unsigned(s.source_role)); checksum.integer(s.key.size()); checksum.integer(tail(s.key)); cursor.advance();
      }
      auto sample_ns = elapsed(start);
      require(checksum.value == input.sample_checksum, "timed sample checksum differs"); input.verify_work(cursor.counters());
      std::cout << profile << ',' << (duplicates ? "duplicates" : "interleaved") << ',' << count << ',' << prefix << ',' << round
        << ",sample," << sample_ns << ',' << sample_ns / input.order.size() << ',' << input.order.size() << ','
        << input.comparisons << ',' << checksum.value << '\n';
      start = clock_type::now(); index_pipeline<P> pipeline(input.source, input.stages);
      std::uint64_t quanta = 0; while (!pipeline.done()) quanta += pipeline.step(4096);
      auto result = pipeline.finish(); auto pipeline_ns = elapsed(start);
      auto wire = input.verify_pipeline(result, pipeline); require(wire == expected_wire, "timed pipeline differs from warm build");
      std::cout << profile << ',' << (duplicates ? "duplicates" : "interleaved") << ',' << count << ',' << prefix << ',' << round
        << ",pipeline," << pipeline_ns << ',' << pipeline_ns / input.order.size() << ',' << input.order.size() << ','
        << quanta << ',' << wire << '\n';
    }
    input.verify_samples();
  }
}
int main(int argc, char ** argv) try {
  auto count = argc > 1 ? unsigned(std::stoul(argv[1])) : 4096u;
  auto prefix = argc > 2 ? unsigned(std::stoul(argv[2])) : 64u;
  auto rounds = argc > 3 ? unsigned(std::stoul(argv[3])) : 3u;
  require(count >= 64 && count <= 65536 && prefix <= 4096 && rounds && rounds <= 100, "invalid benchmark dimensions");
#if defined(__APPLE__)
  require(pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0) == 0, "benchmark QoS request failed");
#endif
  std::cout << std::fixed << std::setprecision(3)
    << "profile,fixture,records,prefix_bytes,round,operation,total_ns,record_ns,source_entries,work,checksum\n";
  for (bool duplicates : {false, true}) {
    run<storage_policy<profile_unit::byte>>("byte", count, prefix, rounds, duplicates);
    run<storage_policy<profile_unit::bit>>("bit", count, prefix, rounds, duplicates);
  }
} catch (std::exception const & error) { std::cerr << error.what() << '\n'; return 1; }

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Measures tagged sample traversal and complete index pipelines with independent integer-key oracles.
 */
