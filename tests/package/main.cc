/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/blob.h>
#include <everett/durability.h>
#include <everett/fingerprint.h>
#include <everett/file.h>
#include <everett/front.h>
#include <everett/mapped_file.h>
#include <everett/multiverse.h>
#include <everett/object_path.h>
#include <everett/pins.h>
#include <everett/policy.h>
#include <everett/profile.h>
#include <everett/profile_blob.h>
#include <everett/rank.h>
#include <everett/rank_groups.h>
#include <everett/rank15.h>
#include <everett/select15.h>
#include <everett/select_groups.h>
#include <everett/world.h>

#include <array>
#include <cstdint>
#include <type_traits>

using policy = everett::storage_policy<everett::profile_unit::bit, everett::fixed_values<3>, 7>;
using store = everett::multiverse<policy>;
static_assert(std::is_same_v<store::sort, everett::sort<policy>>);
static_assert(std::is_same_v<store::blob::policy_type, policy>);

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
  return 0;
}

/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's standalone package consumption.
 */
