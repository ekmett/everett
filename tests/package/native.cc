// SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
#include <array>
#include <concepts>
#include <cstdint>
#include <span>
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
  std::array<std::uint64_t, 4> offsets{0, 7, 300, 12000};
  auto ef = everett::elias_fano::build(offsets);
  for (std::uint64_t i = 0; i < offsets.size(); ++i)
    if (ef.view().select<arch>(i) != offsets[i]) return 2;
  std::array<everett::profile_record, 2> records{{
    {everett::bit_string::from_bytes("a"), everett::bit_string::from_bytes("x")},
    {everett::bit_string::from_bytes("b"), everett::bit_string::from_bytes("y")}
  }};
  auto native = store::native_array::build(records);
  auto scalar = everett::profile_array<everett::storage_policy<>>::build(records);
  if (everett::encode_native_sections(native).materialize() !=
      everett::encode_native_sections(scalar).materialize()) return 3;
  auto key = native.view().reconstruct_at(1);
  if (everett::compare_bits(key.prefix.view(), records[1].key.view())) return 4;
  return 0;
}
