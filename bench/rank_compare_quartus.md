Native AVX2 packed-rank comparison
=================================

On this Core i9-12900K, the SAD-first AVX2 reduction had a lower median than
the qword-first reduction in every measured whole-query workload. For the hot
packed dataset, independent rank took 2.896 ns with SAD and 3.235 ns with the
qword-first reduction. The large dependent query chain is a useful limit:
the scalar implementation beat both SIMD implementations there.

The [CSV](results/rank_compare_quartus_avx2.csv) retains all five variants,
checksums, storage sizes and timing ranges. The
[environment record](results/rank_compare_quartus_environment.json) records
the compiler, CPU, source hashes, flags and timestamps. These are native Linux
AVX2 measurements from September 15, 2026; this host did not expose AVX-512.

Sources and reductions
----------------------

I use the same original bit vector and exclusive group boundary, `rank(15*g)`,
for every variant. The three packed variants share their class and checkpoint
allocations. This comparison does not include RRR coding or decoding.

| CSV variant | Implementation |
| --- | --- |
| `rank15_baseline` | Scalar packed-prefix implementation from `9f68e4d0fdecae73b7d28b748a0d062cc072fbfa`. |
| `rank_groups15_baseline` | The `rank_groups_view<15>` wrapper from the same revision, which already delegates to `rank15_view`. |
| `rank15_candidate` | SAD-first AVX2 prototype reconstructed with the [patch](rank15_sad_prototype.patch). |
| `rank15_simd` | Qword-first AVX2 implementation from `aeaaa9d9896db9aec1a003a0a5ba7b0b6174bd06`. |
| `bitmap_rank` | Full bit-vector rank directory from `9f68e4d`, retained as a reference in the CSV. |

Both AVX2 implementations load two 32-byte vectors for a readable full
checkpoint, select the low and high nybbles before the requested boundary,
and add the byte populations. Each resulting byte is at most 60. SAD-first
uses `vpsadbw` to sum groups of eight bytes into qwords, then reduces those
qwords. Qword-first reduces four qwords while the eight independent byte lanes
remain at most 240, then widens and horizontally sums those bytes in scalar
code. Both retain a bounded scalar path for short final checkpoints.

The reconstruction patch preserves the exact measured SAD header, including
its scalar reduction being written inline instead of in a shared helper.
Its SHA-256 is
`ed125be1898573ec2b926df930c96b909921fc506536d9c2e1c8b0363e0fb686`.
The benchmark source is Git blob
`506524722923b7aac1af17cc3f29173af9f5fb01`, committed by the compiler portability
change `ca93a619a45f1a7ab3a923e2f5793fa9211678d5`. Pinning these sources avoids
silently changing either prototype when the installed headers change.

Host and method
---------------

- Intel Core i9-12900K, family 6/model 151/stepping 2, microcode `0x3a`.
- Linux `6.8.0-100-generic`, glibc 2.35, `MemTotal` 98,628,284 KiB.
- Ubuntu Clang 20.0.0, build
  `++20241101031257+b74e588e1f46-1~exp1~20241101151424.2024`.
- Flags: `-std=c++20 -O3 -DNDEBUG -mavx2 -mpopcnt -mno-avx512f
  -Wall -Wextra -Werror`; no LTO.
- `taskset -c 12` pinned timing to a permitted performance-core thread.
  Its SMT sibling is CPU 13. L1 data cache is 48 KiB, private-core L2 is
  1,280 KiB, and shared L3 is 30 MiB.
- The existing host resource gate serialized cooperating heavy jobs.
  `intel_pstate` retained its `powersave` governor and `balance_performance`
  preference. Affinity does not isolate the SMT sibling or fix clock speed;
  no power or service settings were changed.

Each row contains five trials of 1,048,576 queries. The five variants run in
one process, with their order rotated each trial so each runs first once.
Source bits use deterministic SplitMix64 output; query positions are also
deterministic and shared between variants. Construction, allocation, oracle
checking and file I/O are outside the timed region. Timed calls include query
indexing and indirect function dispatch. Inspection of native assembly
confirmed query loads and indirect calls remain inside the timing loops.

`pair_two_ranks` computes `rank(g) + rank(g+1)`. The equivalent
`pair_rank_plus_class` computes `2*rank(g) + class_at(g)`. Dependent rank uses
the previous answer to choose the next group; it includes the common
XOR/multiply/query-selection work and limits overlap between queries. It is
not an isolated hardware memory-latency measurement.

Every generated query, including each complete dependent chain, was checked
against an independent raw-word population-prefix oracle before timing.
Timed checksums agree across all variants. The public `everett.rank` and
`everett.groups` tests from `aeaaa9d` passed both Release and ASan/UBSan builds
on this host with AVX2 enabled. Those tests include every nybble position and
protected-page tails. Oracle tests retained assertions with `-UNDEBUG` and
also used `-Wpedantic`. The sanitizer build used
`-O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer`.

Storage
-------

| Case | Logical bits | Packed classes + checkpoints | Full data + directory | Query array |
| --- | ---: | ---: | ---: | ---: |
| `hot_packed32KiB` | 873,600 | 29,120 + 3,640 = 32,760 B | 109,248 + 3,424 = 112,672 B | 256 KiB |
| `large_packed96MiB` | 2,684,353,920 | 89,478,464 + 11,184,808 = 100,663,272 B | 335,544,256 + 10,485,768 = 346,030,024 B | 8 MiB |

These are encoded array sizes; vector capacities matched them. They exclude
C++ view/vector objects, allocator bookkeeping and untimed oracle buffers.
All data is in memory; this does not measure storage I/O. The large packed
and full representations both exceed the shared 30 MiB L3 cache. Hot packed
classes began at byte offset 32 modulo 64, and large classes at offset 16;
both checkpoint arrays began at offset 16. All packed variants share those
same addresses. Full-data offsets were 0 and 16 respectively.

Results
-------

Entries are **median [minimum, maximum] nanoseconds per query**, across five
trials. The paired rows report the complete pair operation.

| Hot packed 32 KiB | Scalar | SAD-first AVX2 | Qword-first AVX2 |
| --- | ---: | ---: | ---: |
| Independent rank | 9.212 [9.088, 9.544] | 2.896 [2.879, 2.940] | 3.235 [3.221, 3.279] |
| Two-rank pair | 13.530 [13.503, 13.554] | 5.136 [5.129, 5.180] | 6.109 [6.109, 6.156] |
| Rank-plus-class pair | 9.839 [9.763, 9.921] | 3.234 [3.218, 3.258] | 3.694 [3.689, 3.720] |
| Dependent rank | 10.309 [10.176, 10.419] | 7.931 [7.920, 7.959] | 8.440 [8.427, 8.628] |

| Large packed 96 MiB | Scalar | SAD-first AVX2 | Qword-first AVX2 |
| --- | ---: | ---: | ---: |
| Independent rank | 30.900 [30.190, 31.067] | 18.158 [18.114, 18.274] | 21.407 [21.378, 21.513] |
| Two-rank pair | 49.053 [48.243, 49.580] | 30.738 [30.715, 31.108] | 35.485 [35.338, 35.598] |
| Rank-plus-class pair | 34.145 [33.646, 34.325] | 21.801 [21.761, 21.866] | 23.853 [23.822, 23.968] |
| Dependent rank | 89.827 [88.399, 92.079] | 92.089 [91.948, 92.602] | 92.929 [92.732, 93.295] |

This supports SAD-first over qword-first for these AVX2 workloads on this
host. It does not establish a universal ISA or microarchitecture ranking,
and the large dependent row prevents a blanket SIMD-over-scalar conclusion.
The retained bitmap and typed-wrapper controls are available in the CSV;
the table focuses on the reduction choice using identical packed storage.

Reproduction
------------

Run the following on an AVX2 Linux machine, through its configured resource
gate. CPU 12 was suitable on this host; choose a permitted performance-core
thread on another host. The source extraction and compilation use only an
ignored build directory. The general `rank_compare.sh` runner pins the earlier
Apple comparison and therefore does not select these three snapshots.

```sh
repo=$(git rev-parse --show-toplevel)
build="$repo/build-rank-quartus-repro"
mkdir -p "$build/baseline/everett"
for header in rank rank15 rank_groups; do
  git show "9f68e4d0fdecae73b7d28b748a0d062cc072fbfa:include/everett/$header.h" \
    > "$build/baseline/everett/$header.h"
done
git show aeaaa9d9896db9aec1a003a0a5ba7b0b6174bd06:include/everett/rank15.h \
  > "$build/qword_rank15.h"
cp "$build/qword_rank15.h" "$build/sad_rank15.h"
patch "$build/sad_rank15.h" < "$repo/bench/rank15_sad_prototype.patch"
git cat-file blob 506524722923b7aac1af17cc3f29173af9f5fb01 \
  > "$build/rank_compare.cc"
clang++ -std=c++20 -O3 -DNDEBUG -mavx2 -mpopcnt -mno-avx512f \
  -Wall -Wextra -Werror -I"$build/baseline" \
  "-DEVERETT_RANK_CANDIDATE=\"$build/sad_rank15.h\"" \
  "-DEVERETT_RANK_SIMD=\"$build/qword_rank15.h\"" \
  "$build/rank_compare.cc" -o "$build/rank_compare"
taskset -c 12 "$build/rank_compare" check 1 32768 core
taskset -c 12 "$build/rank_compare" hot_packed32KiB 5 1048576 core \
  > "$build/hot.csv"
taskset -c 12 "$build/rank_compare" large_packed96MiB 5 1048576 core \
  > "$build/large.csv"
```

The exact source SHA-256 values in the environment record allow checking the
extracted inputs before running. Record new alignment, frequency policy and
cache details with any new timings; allocation addresses are not fixed by
the deterministic input seed.
