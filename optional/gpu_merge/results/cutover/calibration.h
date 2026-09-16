/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Generated artifact for the checked optional GPU cutover calibration.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
// Generated empirical calibration; see calibration.json and report.md.
#pragma once
#include "cutover.h"
namespace everett_gpu {
inline constexpr std::array<cutover_node, 3> calibrated_nodes{{
  {0, 1, 2, 51200.0, false},
  {9, 0, 0, 0, false},
  {9, 0, 0, 0, true},
}};
inline const cutover_calibration calibrated_cutover{
  {"Apple M2 Max|registryID=4294968459", "metal", "c7042a4fdd4d9207e106916f9fdc97fc0b058ba7fb89a2bc9a8ab4493a5f4653"},
  {512, 8031, 8760, 10.880470275878906, 1.0, 1.0, 8.0, 0.0, 0},
  {524288, 126940002, 127016152, 516.4378877527573, 16.0, 15.99833696390509, 264.0, 514.5, 2},
  calibrated_nodes, true
};
}
