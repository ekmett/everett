/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks native module imports and architecture-independent file bytes.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

import everett;
#if defined(EVERETT_PACKAGE_NEON)
import everett.neon;
using policy = everett::neon_policy<>;
#elif defined(EVERETT_PACKAGE_AVX2)
import everett.avx2;
using policy = everett::avx2_policy<>;
#elif defined(EVERETT_PACKAGE_AVX512)
import everett.avx512;
using policy = everett::avx512_policy<>;
#endif

using arch = policy::architecture;
using store = everett::multiverse<policy>;
static_assert(std::same_as<everett::storage_policy<>::architecture, simd::scalar>);
static_assert(std::same_as<store::policy_type, everett::backend_policy<arch>>);
static_assert(policy::unit == everett::storage_policy<>::unit);
static_assert(policy::value_width == everett::storage_policy<>::value_width);

int native_identity(everett::rank15_view const & ranks) {
  if (ranks.rank<arch>(1) != 7) return 1;
  std::uint64_t prefix = 0;
  for (std::uint64_t group = 0; group < ranks.group_count(); ++group) {
    if (ranks.rank<arch>(group) != prefix) return 1;
    prefix += (group + 7) & 15;
  }
  for (unsigned width : {8u, 16u}) {
    std::array<std::uint64_t, 64> offsets{};
    for (std::uint64_t i = 0; i < offsets.size(); ++i)
      offsets[i] = ((i + 1) << width) + (i & 127);
    auto ef = everett::elias_fano::build<arch>(offsets);
    auto scalar_ef = everett::elias_fano::build(offsets);
    if (ef.low_width != width || ef.low != scalar_ef.low || ef.high != scalar_ef.high) return 2;
    for (std::uint64_t i = 0; i < offsets.size(); ++i)
      if (ef.view().select<arch>(i) != offsets[i]) return 2;
  }
  std::vector<everett::profile_record> records;
  for (unsigned i = 0; i < 32; ++i) {
    // Long shared prefixes cross vector boundaries; encoded suffixes and
    // subsequent records do not promise native alignment.
    auto key = std::string(131, 'a') + char('A' + i);
    auto value = std::string(i + 1, 'x');
    records.push_back({everett::bit_string::from_bytes(key), everett::bit_string::from_bytes(value)});
  }
  auto native = store::native_array::build(records);
  auto scalar = everett::profile_array<everett::storage_policy<>>::build(records);
  if (everett::encode_native_sections(native).materialize() !=
      everett::encode_native_sections(scalar).materialize()) return 3;
  for (std::uint64_t i = 0; i < records.size(); ++i) {
    auto key = native.view().reconstruct_at(i);
    if (everett::compare_bits(key.prefix.view(), records[i].key.view())) return 4;
  }
  return 0;
}
