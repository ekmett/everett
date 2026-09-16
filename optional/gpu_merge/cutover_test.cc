/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks header-only calibrated selection and conservative fallbacks.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include "cutover.h"
#include <limits>
#include <stdexcept>

namespace {
  void check(bool value) { if (!value) throw std::runtime_error("cutover selector regression"); }
}
int main() {
  using namespace everett_gpu;
  cutover_header small{512, 8192, 1200, 8, 65};
  auto larger = small; larger.records *= 8; larger.payload_bits *= 8; larger.file_bytes *= 8;
  auto low = *header_features(small, small), high = *header_features(larger, larger);
  std::array<cutover_node, 3> nodes{{{0, 1, 2, 2048, false}, {}, {9, 0, 0, 0, true}}};
  cutover_calibration calibration{{"test device", "test backend", "checked variant"}, low, high, nodes, true};
  check(!choose_merge(small, small, calibration.identity, calibration).gpu);
  check(choose_merge(larger, larger, calibration.identity, calibration).gpu);
  // Same row count but unmeasured large payload/file sizes must not inherit the
  // small-value calibration. Every feature comes from headers or stat sizes.
  auto big_values = larger; big_values.payload_bits *= 8; big_values.file_bytes *= 8;
  check(choose_merge(big_values, big_values, calibration.identity, calibration).reason == cutover_reason::outside_envelope);
  auto other = calibration.identity; other.backend = "different backend";
  check(choose_merge(larger, larger, other, calibration).reason == cutover_reason::uncalibrated);
  other = calibration.identity; other.variant = "unchecked variant";
  check(!choose_merge(larger, larger, other, calibration).gpu);
  other = calibration.identity; other.device = "different GPU";
  check(!choose_merge(larger, larger, other, calibration).gpu);
  calibration.heldout_passed = false;
  check(!choose_merge(larger, larger, calibration.identity, calibration).gpu);
  calibration.heldout_passed = true;
  nodes[0].left = 0; nodes[0].right = 0;
  check(!choose_merge(larger, larger, calibration.identity, calibration).gpu);
  nodes[0].feature = 100;
  check(!choose_merge(larger, larger, calibration.identity, calibration).gpu);
  auto bad = small; bad.records = 0; check(!header_features(bad, small));
  bad = small; bad.terminal_key_bits = 64; check(!header_features(bad, small));
  bad = small; bad.common_value_bits = 0; check(!header_features(bad, small));
  bad = small; bad.file_bytes = 1; check(!header_features(bad, small));
  bad = small; bad.payload_bits = std::numeric_limits<std::uint64_t>::max(); check(!header_features(bad, small));
  check(header_features(larger, small)->at(4) == 8);
}
