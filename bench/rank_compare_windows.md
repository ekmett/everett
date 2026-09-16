# Native Windows packed-rank comparison

I compared the portable scalar, SAD-first and qword-first public rank paths on
an AMD Ryzen 9 7950X3D running Windows 11 build 26200. The compiler was MSVC
19.44.35228 from Visual Studio 2022 Build Tools 17.14.40. Both AVX2 and AVX512
executed natively; this run used neither Rosetta nor another emulator.

The benchmark source is [rank_compare.cc](rank_compare.cc) at `ca93a61`.
Its original measured SHA-256 is
`3bf47da16e69832c8db80e80d6f97d6f3a6cd0c6350930d24c1782ef419e4266`.
The MSVC changes use `__umulh` for the unsigned product's high half and
`__declspec(noinline)` for the timed public calls. The NEON-only Poppy adapter
is omitted on x86. The query generation, oracles and timing loops are shared
with the other hosts' benchmark.

## Features, affinity and validation

CPUID and XGETBV reported:

| Field | Result |
| --- | --- |
| Leaf 1 ECX | `fed8320b` |
| Leaf 7 EBX | `f1bf97a9` |
| XCR0 | `e7` |
| AVX2 CPU/OS support | present |
| AVX512F + BW CPU/OS support | present |
| Process affinity / observed CPU | `0x1` / logical CPU 0 |
| Pinned CPU's data caches | 32 KiB L1, 1 MiB L2, 96 MiB L3 |

The Python controller set its process affinity before launching the compiler,
tests and benchmark; the child processes inherited it. The feature probe
confirmed both the affinity mask and the executing CPU. The L3 CPUID record
reports sharing by sixteen logical processors, placing this run on the CCD
with the larger cache. Initial sampled system CPU use was 5%, with about
98 GiB of physical memory available. I found no resource gate in the inspected
host coordination locations; this was a bounded single-core run in a unique
handoff directory.

The public `tests/rank.cc` and `tests/groups.cc` suites passed for **both** SAD
and qword headers under **both** `/arch:AVX2` and `/arch:AVX512`: eight native
test executions. This includes isolated nybble positions, maximum populations,
partial checkpoints, unaligned arrays and all policy group sizes. POSIX guard
pages are conditional tests and do not execute on Windows. These Windows runs
were not sanitizer runs.

The benchmark's target-check header rejects an AVX2 build without `__AVX2__`,
an AVX512 build without both `__AVX512F__` and `__AVX512BW__`, and an AVX2 build
that would instead select AVX512. Both benchmark builds also passed their
untimed checks for the two hot datasets. Every generated timed query was
validated against the independent raw-bit oracle, and all variants' checksums
matched. The [validation transcript](results/rank_compare_windows_validation.txt)
records commands, feature output, compiler output, successful tests and source
hashes.

## Pinned inputs and commands

The historical names in the raw CSV mean:

| CSV variant | Header / algorithm |
| --- | --- |
| `bitmap_rank` | Everett's full-bit-vector 2048/512 directory |
| `rank15_baseline` | `9f68e4d0fdecae73b7d28b748a0d062cc072fbfa`, portable fallback on x86 |
| `rank_groups15_baseline` | the same revision's typed wrapper around that fallback |
| `rank15_candidate` | SAD-first snapshot, SHA-256 `ed125be1898573ec2b926df930c96b909921fc506536d9c2e1c8b0363e0fb686` |
| `rank15_simd` | qword-first `aeaaa9d9896db9aec1a003a0a5ba7b0b6174bd06`, SHA-256 `80bbeb9777caf634abaf990863f7d93753c1c4e15e72f1537ad554b82352a34c` |

The [SAD reconstruction patch](rank15_sad_prototype.patch) reproduces that
snapshot from the pinned qword-first header. The scalar baseline is newer than
the original M2 comparison's `7732b1e` baseline, so those baseline columns should
not be treated as the same implementation.

After preparing those headers under `baseline/everett`, `sad_rank15.h` and
`qword_rank15.h`, and initializing the x64 MSVC environment, the essential
benchmark build was:

```bat
cl /nologo /std:c++20 /EHsc /O2 /DNDEBUG /W4 /WX /permissive- /arch:AVX2 ^
  /Ibaseline /DEVERETT_RANK_CANDIDATE=\"sad_rank15.h\" ^
  /DEVERETT_RANK_SIMD=\"qword_rank15.h\" rank_compare.cc /Fecompare-avx2.exe
start "" /b /wait /affinity 1 compare-avx2.exe hot_packed32KiB 7 1048576 core
```

The second build substitutes `/arch:AVX512`. The recorded builds additionally
force-included the target-check header described above; the transcript contains
the complete commands. The actual run used inherited affinity instead of the
equivalent `start /affinity` spelling shown here.

## Shared input and results

Every variant in each process used the same 873,600 source bits and 58,240
boundaries `g*15`. Packed variants shared the same 29,120 bytes of classes plus
3,640 bytes of checkpoints: **32,760 bytes** total. The full representation used
109,248 bytes of data plus 3,424 bytes of metadata: **112,672 bytes** total.
Packed data and checkpoint addresses were 64-byte aligned; full data and
metadata addresses were respectively 32 and 16 modulo 64. The shared query
array occupied 256 KiB.

Each row contains seven rotating-order trials of 1,048,576 queries. Construction,
allocation, oracle checks and I/O were outside the measured region. Independent
rows measure amortized public-call time. Dependent rows feed each result into
the next query's selection and therefore include that common dependency-chain
arithmetic; they are not isolated load-latency measurements.

| Target / implementation | Random rank, ns | Two-rank pair, ns | Rank + class pair, ns | Dependent rank, ns |
| --- | ---: | ---: | ---: | ---: |
| AVX2 / scalar | 8.961 | 13.754 | 9.430 | 10.797 |
| AVX2 / SAD | 4.289 | 8.898 | 5.272 | 8.363 |
| AVX2 / qword | 4.939 | 9.718 | 5.809 | 10.264 |
| AVX512 / scalar | 8.850 | 13.791 | 9.690 | 10.660 |
| AVX512 / SAD | 3.673 | 7.139 | 4.709 | 14.053 |
| AVX512 / qword | 4.344 | 7.908 | 4.928 | 10.258 |

All entries above are medians. For independent rank, SAD's AVX2 range was
4.130–4.682 ns versus qword's 4.663–5.068 ns; AVX512 ranges were 3.457–3.949
and 3.790–4.587 ns respectively. SAD had lower independent-rank and pair medians
on both targets in this run.

The AVX512 dependent result went the other way: SAD was
**14.053 [9.780, 18.836] ns**, while qword was
**10.258 [10.079, 115.014] ns**. Several other rows also had substantial
outliers. We should preserve these ranges rather than turn the independent
results into a claim of universal superiority. The full
[AVX2 CSV](results/rank_compare_windows_avx2.csv) and
[AVX512 CSV](results/rank_compare_windows_avx512.csv) retain every minimum,
median, maximum, control variant and checksum. This run does not establish
large-working-set, other-core, other-compiler or complete catalog-search costs.
