/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Selects a calibrated merge backend using input headers and byte sizes only.
 *
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace everett_gpu {
  struct cutover_header {
    std::uint64_t records = 0, payload_bits = 0, file_bytes = 0;
    std::optional<std::uint64_t> common_value_bits;
    std::uint64_t terminal_key_bits = 0;
  };
  struct cutover_identity {
    std::string_view device, backend, variant;
    bool operator==(cutover_identity const &) const = default;
  };
  // Features are hardware-independent. Timing coefficients/rules are not.
  inline constexpr std::array<std::string_view, 9> cutover_feature_names{
    "records", "payload_bytes", "file_bytes", "payload_bytes_per_record",
    "record_imbalance", "byte_imbalance", "terminal_key_bytes",
    "fixed_value_bytes", "known_value_widths"};
  using cutover_features = std::array<double, cutover_feature_names.size()>;
  struct cutover_node {
    // feature==feature_count denotes a leaf. Internal nodes branch left on <=.
    std::uint32_t feature = cutover_feature_names.size(), left = 0, right = 0;
    double threshold = 0;
    bool gpu = false;
  };
  struct cutover_calibration {
    cutover_identity identity;
    cutover_features minimum{}, maximum{};
    std::span<cutover_node const> nodes;
    bool heldout_passed = false;
  };
  enum class cutover_reason {
    unsupported, uncalibrated, outside_envelope, cpu_region, gpu_region
  };
  struct cutover_decision {
    bool gpu = false;
    cutover_reason reason = cutover_reason::uncalibrated;
  };

  inline std::optional<cutover_features> header_features(cutover_header const & a,
      cutover_header const & b) {
    // These are this experimental default-string GPU profile's hard bounds,
    // independent of empirical calibration. Empty inputs use the CPU/adopt path.
    auto eligible = [](cutover_header const & h) {
      return h.records && h.records < (1u << 24) && h.payload_bits && h.payload_bits < (1u << 31) &&
        h.file_bytes >= (h.payload_bits + 7) / 8 && h.file_bytes < (1ull << 32) &&
        h.terminal_key_bits && h.terminal_key_bits <= (1u << 30) + 1 &&
        ((h.terminal_key_bits - 1) & 7) == 0 &&
        (!h.common_value_bits || (*h.common_value_bits && *h.common_value_bits <= (1u << 30)));
    };
    if (!eligible(a) || !eligible(b) || a.records + b.records >= (1u << 24)) return std::nullopt;
    auto n = double(a.records + b.records);
    auto bytes_a = double((a.payload_bits + 7) / 8), bytes_b = double((b.payload_bits + 7) / 8);
    auto fixed = std::max(a.common_value_bits.value_or(0), b.common_value_bits.value_or(0));
    return cutover_features{n, bytes_a + bytes_b, double(a.file_bytes + b.file_bytes), (bytes_a + bytes_b) / n,
      double(std::max(a.records, b.records)) / double(std::min(a.records, b.records)),
      std::max(bytes_a, bytes_b) / std::min(bytes_a, bytes_b),
      double(std::max(a.terminal_key_bits, b.terminal_key_bits) - 1) / 8,
      double(fixed) / 8, double(a.common_value_bits.has_value()) + double(b.common_value_bits.has_value())};
  }

  // The caller establishes the supported physical sort/profile and immutable
  // input ownership. This only chooses execution; it does not replace parsing,
  // ordering validation, memory bounds, or output checks in either backend.
  inline cutover_decision choose_merge(cutover_header const & a, cutover_header const & b,
      cutover_identity execution, cutover_calibration const & calibration) {
    auto features = header_features(a, b);
    if (!features) return {false, cutover_reason::unsupported};
    if (!calibration.heldout_passed || execution.device.empty() || execution.backend.empty() || execution.variant.empty() ||
        !(execution == calibration.identity) || calibration.nodes.empty())
      return {false, cutover_reason::uncalibrated};
    for (std::size_t i = 0; i != features->size(); ++i)
      if (!((*features)[i] >= calibration.minimum[i] && (*features)[i] <= calibration.maximum[i]))
        return {false, cutover_reason::outside_envelope};
    std::uint32_t at = 0;
    // A malformed calibration graph is a CPU fallback, including cycles. The
    // checked generator emits at most depth3, but this guard is independent.
    for (std::size_t visited = 0; visited != calibration.nodes.size(); ++visited) {
      if (at >= calibration.nodes.size()) break;
      auto const & node = calibration.nodes[at];
      if (node.feature == cutover_feature_names.size())
        return {node.gpu, node.gpu ? cutover_reason::gpu_region : cutover_reason::cpu_region};
      if (node.feature > cutover_feature_names.size() || !(node.threshold == node.threshold)) break;
      at = (*features)[node.feature] <= node.threshold ? node.left : node.right;
    }
    return {false, cutover_reason::uncalibrated};
  }
}
