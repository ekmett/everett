/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Diet's standalone package consumption.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <diet/crc32c.h>
#include <diet/durability.h>
#include <diet/fingerprint.h>
#include <diet/file.h>
#include <diet/index_pipeline.h>
#include <diet/mapped_file.h>
#include <diet/mapped_blob.h>
#include <diet/fridge.h>
#include <diet/native_merge.h>
#include <diet/native_writer.h>
#include <diet/object_path.h>
#include <diet/object_writer.h>
#include <diet/pins.h>
#include <diet/policy.h>
#include <diet/profile.h>
#include <diet/profile_blob.h>
#include <diet/query.h>
#include <diet/rank.h>
#include <diet/rank_groups.h>
#include <diet/rank15.h>
#include <diet/elias_fano.h>
#include <diet/sections.h>
#include <diet/word_view.h>
#include <diet/cola.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

using policy = diet::storage_policy<diet::profile_unit::bit, diet::fixed_values<3>, 7>;
using store = diet::fridge<policy>;
static_assert(std::is_same_v<store::sort, diet::sort<policy>>);
static_assert(std::is_same_v<store::blob::policy_type, policy>);
static_assert(std::is_same_v<store::query_root, diet::query_root<policy>>);
static_assert(std::is_same_v<store::query_cursor, diet::query_cursor<policy>>);
static_assert(std::is_same_v<store::query_context, diet::profile_query_context<policy>>);
static_assert(std::is_same_v<store::object_writer::policy_type, policy>);
static_assert(std::is_same_v<store::mapped_blob::policy_type, policy>);
static_assert(std::is_same_v<store::mapped_query_root, diet::query_root<policy, store::mapped_blob>>);
static_assert(std::is_same_v<store::native_writer::policy_type, policy>);
static_assert(std::is_same_v<store::native_merge_builder<>::policy_type, policy>);

std::uint32_t crc32c_from_other_translation_unit(std::span<std::byte const> bytes);

int main() {
  std::array<std::uint8_t, 2> classes{7, 1};
  auto ranks = diet::rank15_index::build(classes, 16);
  std::array<std::uint64_t, 3> offsets{0, 33, 40};
  auto starts = diet::elias_fano::build(offsets);
  if (ranks.view().rank(1) != 7 || ranks.view().count() != 8) return 1;
  if (starts.view().size() != 3 || starts.view().select(1) != 33 || starts.view().select(2) != 40) return 2;
  std::array<diet::profile_record, 1> records{{
    {diet::bit_string::from_bits("101"), diet::bit_string::from_bits("110")}
  }};
  auto blob = store::blob::build(records);
  auto decoded = blob.native().view().reconstruct_at(0);
  if (diet::compare_bits(decoded.prefix.view(), records[0].key.view()) ||
      diet::compare_bits(decoded.value.view(), records[0].value.view())) return 3;
  auto target = std::make_shared<store::blob const>(std::move(blob));
  diet::index_pipeline<policy> pipeline(target, {target});
  while (!pipeline.done()) pipeline.step(4);
  auto head = pipeline.finish();
  if (head->target() != target || head->borrowed().size() != 1 ||
      !head->false_borrow(0) ||
      std::addressof(head->native()) != std::addressof(target->native())) return 4;
  auto check = std::as_bytes(std::span("123456789", std::size_t{9}));
  if (diet::crc32c(check) != 0xe3069283u || crc32c_from_other_translation_unit(check) != 0xe3069283u)
    return 5;
  if (diet::crc32c({}) != 0 || crc32c_from_other_translation_unit({}) != 0) return 6;
  auto root = store::query_root::build(head);
  if (root.head() != head) return 7;
  auto query = root.cursor(records[0].key.view());
  std::size_t matches = 0;
  while (!query.done()) {
    query.step(1);
    if (!query.has_match()) continue;
    auto match = query.take_match();
    if (matches >= 2 || match.source != (matches ? target : head) || match.ordinal ||
        diet::compare_bits(match.value.view(), records[0].value.view())) return 8;
    ++matches;
  }
  if (matches != 2) return 9;
  if (diet::crc32c(check.subspan(4), diet::crc32c(check.first(4))) != 0xe3069283u) return 10;
  diet::file_header<policy> envelope{diet::file_kind::native_blob, 0, 0, 3};
  auto encoded_header = diet::encode_file_header(envelope, 0);
  if (diet::decode_file_header<policy>(encoded_header) != envelope) return 11;
  auto sections = diet::encode_native_sections(target->native());
  auto serialized = sections.materialize();
  if (diet::validate_file<policy>(serialized) != sections.header()) return 12;
  store::native_writer writer;
  writer.append(records[0]);
  auto native = std::make_shared<store::native_array const>(writer.finish());
  store::native_merge_builder<> merge(native, native);
  merge.step(1);
  auto merged = store::blob::adopt_native(merge.finish());
  if (merged.native().size() != 1 || merged.borrowed().size() != 0) return 13;
  return 0;
}
