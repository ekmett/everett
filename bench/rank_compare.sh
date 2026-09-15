#!/bin/sh
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

set -eu
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
build=${DIET_RANK_COMPARE_BUILD:-"$repo/build-rank-compare"}
baseline=7732b1ed551dccc05256964106b7091e56e65bc0
candidate=91bd022eaaf6334cdf6b1391389c7ea2eb706d48
python3 "$repo/bench/snapshot.py" "$baseline" "$build/baseline" \
  include/diet/rank.h include/diet/rank15.h include/diet/rank_groups.h
python3 "$repo/bench/snapshot.py" "$candidate" "$build/candidate" include/diet/rank15.h
"${CXX:-clang++}" -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Werror \
  -I"$build/baseline/include" "-DDIET_RANK_SIMD=\"$build/candidate/include/diet/rank15.h\"" \
  "$repo/bench/rank_compare.cc" -o "$build/rank_compare"
exec "$build/rank_compare" "$@"

# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Reproduces the historical full-vector and packed-rank comparison.
