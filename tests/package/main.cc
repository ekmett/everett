#include <everett/blob.h>
#include <everett/durability.h>
#include <everett/fingerprint.h>
#include <everett/front.h>
#include <everett/mapped_file.h>
#include <everett/pins.h>
#include <everett/rank.h>
#include <everett/rank_groups.h>
#include <everett/rank15.h>
#include <everett/select15.h>
#include <everett/select_groups.h>
#include <everett/world.h>

#include <array>
#include <cstdint>

int main() {
  std::array<std::uint8_t, 2> classes{7, 1};
  auto ranks = everett::rank15_index::build(classes, 16);
  std::array<std::uint64_t, 3> offsets{0, 33, 40};
  auto starts = everett::select15_index::build(offsets, 16);
  if (ranks.view().rank(1) != 7 || ranks.view().rank(2) != 8) return 1;
  if (starts.view().offset(1, 8) != 153 || starts.view().offset(2, 8) != 168) return 2;
  return 0;
}

/**
 * \file
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
 * \endlicense
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Tests Everett's standalone package consumption.
 */
