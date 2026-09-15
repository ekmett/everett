/**
 * \file
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
#include <everett/index_pipeline.h>
#include <everett/mapped_file.h>
#include <everett/multiverse.h>
#include <everett/object_path.h>
#include <everett/pins.h>
#include <everett/policy.h>
#include <everett/profile.h>
#include <everett/profile_blob.h>
#include <everett/query.h>
#include <everett/rank.h>
#include <everett/rank_groups.h>
#include <everett/rank15.h>
#include <everett/select15.h>
#include <everett/select_groups.h>
#include <everett/world.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

using policy = everett::storage_policy<everett::profile_unit::bit, everett::fixed_values<3>, 7>;
using store = everett::multiverse<policy>;
static_assert(std::is_same_v<store::sort, everett::sort<policy>>);
static_assert(std::is_same_v<store::blob::policy_type, policy>);
static_assert(std::is_same_v<store::query_root, everett::query_root<policy>>);
static_assert(std::is_same_v<store::query_cursor, everett::query_cursor<policy>>);
static_assert(std::is_same_v<store::query_context, everett::profile_query_context<policy>>);

std::uint32_t crc32c_from_other_translation_unit(std::span<std::byte const> bytes);

int main() {
  std::array<std::uint8_t, 2> classes{7, 1};
  auto ranks = everett::rank15_index::build(classes, 16);
  std::array<std::uint64_t, 3> offsets{0, 33, 40};
  auto starts = everett::select15_index::build(offsets, 16);
  if (ranks.view().rank(1) != 7 || ranks.view().rank(2) != 8) return 1;
  if (starts.view().offset(1, 8) != 153 || starts.view().offset(2, 8) != 168) return 2;
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
  return 0;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's standalone package consumption.
 */
