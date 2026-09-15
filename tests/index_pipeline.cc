/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks bounded index pipelines against independently sampled batch indexes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/index_pipeline.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  void require(bool condition, char const * message) {
    if (!condition) throw std::runtime_error(message);
  }
  template <class F> void rejects(F && action) {
    bool rejected = false;
    try { action(); } catch (std::exception const &) { rejected = true; }
    require(rejected, "invalid pipeline operation accepted");
  }
  template <class P> diet::bit_string key(unsigned n) {
    auto text = std::string(80, 'p') + std::to_string(10000 + n);
    auto result = diet::bit_string::from_bytes(text);
    if constexpr (P::unit == diet::profile_unit::bit)
      diet::profile_detail::append_bit(result, (n & 1) != 0);
    return result;
  }
  template <class P>
  std::shared_ptr<diet::profile_blob<P> const> make_pair(unsigned first, unsigned count,
                                                         unsigned stride, bool borrowed) {
    std::vector<diet::profile_record> records;
    for (unsigned i = 0; i != count; ++i)
      records.push_back({key<P>(first + i * stride), diet::bit_string::from_bytes(std::string(i % 7, 'v'))});
    std::vector<diet::bit_string> samples;
    if (borrowed) {
      for (unsigned i = 0; i != count; ++i) {
        samples.push_back(key<P>(first + i * stride));
        if (i % 3 == 0) samples.push_back(samples.back());
      }
    }
    return std::make_shared<diet::profile_blob<P> const>(diet::profile_blob<P>::build(records, samples));
  }
  template <class P> std::vector<diet::bit_string> oracle_samples(diet::profile_blob<P> const & pair) {
    std::vector<diet::bit_string> keys;
    auto append = [&](auto item) {
      keys.push_back(diet::bit_string::copy(item.key.prefix));
      return true;
    };
    pair.native().view().visit_all(append);
    pair.borrowed().view().visit_all(append);
    std::stable_sort(keys.begin(), keys.end(), [](auto const & a, auto const & b) {
      return diet::compare_bits(a.view(), b.view()) < 0;
    });
    std::vector<diet::bit_string> samples;
    for (std::size_t i = 0; i < keys.size(); i += P::group_size) samples.push_back(keys[i]);
    return samples;
  }
  template <class P> void equal_pair(diet::profile_blob<P> const & a,
                                     diet::profile_blob<P> const & b) {
    require(a.virtual_size() == b.virtual_size(), "pipeline virtual size");
    require(std::ranges::equal(a.borrowed().bytes(), b.borrowed().bytes()), "pipeline borrowed encoding");
    require(std::ranges::equal(a.false_borrow_bits(), b.false_borrow_bits()), "pipeline false borrows");
    require(a.interleave().classes == b.interleave().classes &&
            a.interleave().checkpoints == b.interleave().checkpoints &&
            a.interleave().view().count() == b.interleave().view().count(), "pipeline rank directory");
    auto const & x = a.borrowed().group_offsets();
    auto const & y = b.borrowed().group_offsets();
    require(x.low == y.low && x.high == y.high && x.sparse == y.sparse &&
            x.entry_count == y.entry_count && x.universe == y.universe &&
            x.low_width == y.low_width && x.samples.size() == y.samples.size(), "pipeline offset encoding");
    for (std::size_t i = 0; i != x.samples.size(); ++i)
      require(x.samples[i].first == y.samples[i].first && x.samples[i].sparse == y.samples[i].sparse,
              "pipeline select samples");
  }
  template <class P> void exercise() {
    using blob = diet::profile_blob<P>;
    using pair = std::shared_ptr<blob const>;
    for (auto empty : {false, true}) {
      auto target = make_pair<P>(0, empty ? 0 : 73, 4, true);
      std::vector<pair> native{
        make_pair<P>(1, empty ? 0 : 39, 4, true),
        make_pair<P>(2, empty ? 0 : 21, 4, false),
        make_pair<P>(3, 0, 1, false),
        make_pair<P>(3, empty ? 0 : 8, 4, false)
      };
      std::vector<pair> expected;
      auto next = target;
      for (auto const & source : native) {
        auto samples = oracle_samples(*next);
        next = std::make_shared<blob const>(source->reindex(samples));
        expected.push_back(next);
      }
      for (std::uint64_t budget : {1, 2, 7, 64}) {
        diet::index_pipeline<P> pipeline(target, native);
        require(pipeline.step(0) == 0, "zero pipeline budget");
        rejects([&] { pipeline.finish(); });
        std::uint64_t calls = 0;
        while (!pipeline.done()) {
          auto work = pipeline.step(budget);
          require(work && work <= budget, "pipeline progress budget");
          require(++calls < 100000, "pipeline stalled");
          if (calls % 5 == 0) {
            auto saved = std::move(pipeline);
            pipeline = std::move(saved);
          }
        }
        auto head = pipeline.finish();
        require(head == pipeline.finish() && pipeline.finished(), "stable pipeline finish");
        rejects([&] { pipeline.step(1); });
        auto current = head;
        for (std::size_t end = native.size(); end; --end) {
          auto i = end - 1;
          equal_pair(*current, *expected[i]);
          require(std::addressof(current->native()) == std::addressof(native[i]->native()),
                  "pipeline changed native allocation");
          require(pipeline.consumed_entries()[i] == current->virtual_size(), "pipeline entry accounting");
          require(pipeline.emitted_samples()[i] == current->group_count(), "pipeline sample accounting");
          current = current->target();
        }
        require(current == target, "pipeline lost exact target pin");
        require(pipeline.source_work().consumed_entries() == target->virtual_size() &&
                pipeline.source_work().decoded_entries == target->virtual_size(), "source was rescanned");
        auto samples = oracle_samples(*target);
        std::uint64_t literal_units = 0;
        for (auto const & sample : samples) literal_units += sample.bit_size / P::bits_per_unit;
        require(pipeline.source_suffix_units() <= literal_units, "sample suffix accounting");
        if (samples.size() > 1)
          require(pipeline.source_suffix_units() < literal_units / 2, "shared prefixes crossed handoff repeatedly");
      }
      {
        diet::index_pipeline<P> identity(target, {});
        require(identity.done() && identity.finish() == target, "empty pipeline identity");
      }
      rejects([&] { diet::index_pipeline<P> invalid({}, native); });
      rejects([&] { diet::index_pipeline<P> invalid(target, {pair{}}); });
      std::weak_ptr<blob const> retained = target;
      pair head;
      {
        diet::index_pipeline<P> pipeline(target, native);
        target.reset();
        native.clear();
        expected.clear();
        next.reset();
        while (!pipeline.done()) pipeline.step(3);
        head = pipeline.finish();
      }
      require(!retained.expired(), "completed chain did not retain target");
      head.reset();
      require(retained.expired(), "pipeline leaked the target pin");
    }
  }
}

int main() {
  try {
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 3>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 7>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 15>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>, 31>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 3>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 7>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 15>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 31>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 3, diet::golomb<3>>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 7, diet::golomb<5>>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 15, diet::exponential_golomb<2>>>();
    exercise<diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>, 31, diet::exponential_golomb<63>>>();
    std::cout << "Streaming index pipeline checks passed\n";
  } catch (std::exception const & error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
