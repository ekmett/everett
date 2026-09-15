Bounded rank without cached totals
=================================

I remove the redundant 64-bit total from each rank owner and view. The encoded
word and checkpoint arrays do not change. `rank(g)` now requires an existing
group, and bitmap `rank(p)` requires an existing bit. The final boundary is not
a rank query. A caller obtains a group's upper prefix from its lower rank plus
its class; `count()` derives the total from the last valid prefix and class or
bit, with zero for an empty index.

This is a C++ API change: constructors no longer take a total, owning `.total`
fields are gone, and `count()` is a bounded payload query rather than a cached
`noexcept` scalar getter. Shape-only mapped opening does not call it. Projection
still checks its selected ranks against the authoritative native/borrowed sizes.
Raw views retain the builder-or-validated-metadata contract. Packed checked
addition now bounds by virtual size; a raw malformed directory is not checked
against a separately supplied borrowed total. Full-bitmap arithmetic retains
its existing trusted-directory contract. Complete semantic admission stays in
explicit scanners.

Measurements
------------

The [source](rank_bounds.cc) and [runner](rank_bounds.py) compare the final
candidate with baseline `c31f339321eebd68aa9319db8c31f788572f2538`. The
[raw table](results/rank_bounds_m2.csv) records minimum, median and maximum for
five rotating trials; [metadata](results/rank_bounds_m2.json) records exact
compiler flags and source/header hashes. Timing used the working-tree headers
subsequently committed as `f02434a`; the metadata also records that exact match.

Each trial performs 1,048,576 queries. Packed variants share the same arrays;
the bitmap shares its words and uses identical directory contents with distinct
C++ directory element types. Independent original populations supply the
oracle. Dependent queries feed each answer into the next position. Every query
is in range, and the pair workload computes `2*rank(g)+class_at(g)`.

On this M2 Max, Apple Clang 21 used `-O3 -DNDEBUG` with
`QOS_CLASS_USER_INITIATED`, without CPU affinity. Median nanoseconds:

| Workload | Cached total | Bounded |
|---|---:|---:|
| K=15 independent rank | 5.568 | 5.569 |
| K=15 independent rank plus class | 7.751 | 7.639 |
| K=15 dependent rank | 16.277 | 16.371 |
| K=15 dependent rank plus class | 17.151 | 17.229 |
| Bitmap independent rank | 6.012 | 6.133 |
| Bitmap dependent rank | 17.998 | 17.935 |

The full table includes K=3, 7 and 31 and all observed ranges. These results show
similar hot-query costs, not a general speedup. The measured views shrink by
exactly eight bytes. Arrays occupy 20,480 / 28,672 / 36,864 / 45,056 bytes for
K=3 / 7 / 15 / 31, and 33,800 bytes for the bitmap. The bitmap's reported
`view_bytes` includes a 16-byte benchmark adapter span in both variants.
This bounded run does not measure cold mappings, LLC-exceeding data, construction,
or `count()` latency.

Reproduce with the measured header revision using the current normalized
fixture and runner, under the host's resource gate:

```sh
python3 bench/rank_bounds.py --candidate f02434a94b7c1250c29d6d4e20a1401a0700cd4d \
  --build-dir build-rank-bounds --trials 5 --queries 1048576
```

The runner normalizes both pinned header snapshots. `rank_compare.sh` and
`neon_cult_rank.py` retain their pinned
historical headers and original observations. `other_rank.py` accepts either
old constructors or the new API without changing its default historical pins.

-Edward Kmett
