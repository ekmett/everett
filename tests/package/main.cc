/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's standalone package consumption.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/crc32c.h>
#include <everett/durability.h>
#include <everett/fingerprint.h>
#include <everett/file.h>
#include <everett/fixed_search.h>
#include <everett/index_pipeline.h>
#include <everett/mapped_file.h>
#include <everett/mapped_blob.h>
#include <everett/multiverse.h>
#include <everett/native_merge.h>
#include <everett/native_writer.h>
#include <everett/object_path.h>
#include <everett/object_writer.h>
#include <everett/pins.h>
#include <everett/policy.h>
#include <everett/profile.h>
#include <everett/profile_blob.h>
#include <everett/query.h>
#include <everett/rank.h>
#include <everett/rank_groups.h>
#include <everett/rank15.h>
#include <everett/elias_fano.h>
#include <everett/sections.h>
#include <everett/word_view.h>
#include <everett/world.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

using policy = everett::storage_policy<everett::tip<everett::encoded_sort<everett::bit_encoding<everett::fixed_values<3>>>>, 7>;
using store = everett::multiverse<policy>;
static_assert(std::is_same_v<store::sort, everett::sort<policy>>);
static_assert(std::is_same_v<store::blob::policy_type, policy>);
static_assert(std::is_same_v<store::query_root, everett::query_root<policy>>);
static_assert(std::is_same_v<store::query_cursor, everett::query_cursor<policy>>);
static_assert(std::is_same_v<store::query_context, everett::profile_query_context<policy>>);
static_assert(std::is_same_v<store::object_writer::policy_type, policy>);
static_assert(std::is_same_v<store::mapped_blob::policy_type, policy>);
static_assert(std::is_same_v<store::mapped_query_root, everett::query_root<policy, store::mapped_blob>>);
static_assert(std::is_same_v<store::native_writer::policy_type, policy>);
static_assert(std::is_same_v<store::native_merge_builder<>::policy_type, policy>);

std::uint32_t crc32c_from_other_translation_unit(std::span<std::byte const> bytes);

int main() {
  std::array<std::uint32_t, 3> fixed_keys{0, 0, 0};
  everett::fixed_key_view<1> fixed(std::as_bytes(std::span(fixed_keys)));
  if (fixed.lower_bound({0}) != 0 || fixed.upper_bound({0}) != 3) return 30;
  std::array<std::uint8_t, 2> classes{7, 1};
  auto ranks = everett::rank15_index::build(classes, 16);
  std::array<std::uint64_t, 3> offsets{0, 33, 40};
  auto starts = everett::elias_fano::build(offsets);
  if (ranks.view().rank(1) != 7 || ranks.view().count() != 8) return 1;
  if (starts.view().size() != 3 || starts.view().select(1) != 33 || starts.view().select(2) != 40) return 2;
  std::array<everett::profile_record, 1> records{{
    {everett::bit_string::from_bits("101"), everett::bit_string::from_bits("110")}
  }};
  auto blob = store::blob::build(records);
  auto decoded = blob.native().view().reconstruct_at(0);
  if (everett::compare_bits(decoded.prefix.view(), records[0].key.view()) ||
      everett::compare_bits(decoded.value.view(), records[0].value.view())) return 3;
  auto target = std::make_shared<store::blob const>(std::move(blob));
  everett::index_pipeline<policy> pipeline(target, {target});
  while (!pipeline.done()) pipeline.step(4);
  auto head = pipeline.finish();
  if (head->target() != target || head->borrowed().size() != 1 ||
      !head->false_borrow(0) ||
      std::addressof(head->native()) != std::addressof(target->native())) return 4;
  auto check = std::as_bytes(std::span("123456789", std::size_t{9}));
  if (everett::crc32c(check) != 0xe3069283u || crc32c_from_other_translation_unit(check) != 0xe3069283u)
    return 5;
  if (everett::crc32c({}) != 0 || crc32c_from_other_translation_unit({}) != 0) return 6;
  auto root = store::query_root::build(head);
  if (root.head() != head) return 7;
  auto query = root.cursor(records[0].key.view());
  std::size_t matches = 0;
  while (!query.done()) {
    query.step(1);
    if (!query.has_match()) continue;
    auto match = query.take_match();
    if (matches >= 2 || match.source != (matches ? target : head) || match.ordinal ||
        everett::compare_bits(match.value.view(), records[0].value.view())) return 8;
    ++matches;
  }
  if (matches != 2) return 9;
  if (everett::crc32c(check.subspan(4), everett::crc32c(check.first(4))) != 0xe3069283u) return 10;
  everett::file_header<policy> envelope{everett::file_kind::native_blob, 0, 0, 3};
  auto encoded_header = everett::encode_file_header(envelope, 0);
  if (everett::decode_file_header<policy>(encoded_header) != envelope) return 11;
  auto sections = everett::encode_native_sections(target->native());
  auto serialized = sections.materialize();
  if (everett::validate_file<policy>(serialized) != sections.header()) return 12;
  store::native_writer writer;
  writer.append(records[0]);
  auto native = std::make_shared<store::native_array const>(writer.finish());
  store::native_merge_builder<> merge(native, native);
  merge.step(1);
  auto merged = store::blob::adopt_native(merge.finish());
  if (merged.native().size() != 1 || merged.borrowed().size() != 0) return 13;
  return 0;
}
