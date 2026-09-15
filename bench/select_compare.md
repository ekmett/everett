Elias–Fano Selection and Construction
===================================

This comparison retains bounded within-word selection, shared construction for
`select15`, NEON validation, and NEON packing for 8- and 16-bit low fields.
The encoded arrays and public interfaces are unchanged.

The baseline is `62ead3fab9d0ee5bda1b47780b7905a45aae1182`.
The integrated candidate is `bc24f443cdee7d12492ac8a6f48e8587de0fc147`.
The [runner](select_compare.py) pins both revisions and builds the same
[benchmark source](select_compare.cc);
[the environment record](results/select_environment.json) records the exact
header, benchmark, and prototype hashes used for these measurements.

Retained changes
----------------

- The portable word selector computes byte populations and cumulative counts,
  locates the containing byte with marked subtraction, then selects through a
  nibble and a two-bit pair. It replaces up to 63 dependent clear-lowest-bit
  iterations with bounded arithmetic.
- A compiler-targeted x86-64 BMI2 branch uses `PDEP` followed by trailing-zero
  count. No runtime dispatch or exported ISA flags are added. Rosetta execution
  validates this branch; native x86 performance remains unmeasured here.
- `select15` uses the existing generic low packer and high-word accumulator.
  Its samples, exceptions, low/high words, and sentinel retain their layout.
- NEON checks two adjacent unsigned comparisons at a time. Little-endian NEON
  narrowing packs complete 8- and 16-bit tiles; incomplete tiles retain bounded
  scalar packing. Other widths retain the existing fixed-shift packer.

The selection precondition is `ordinal < popcount(word)`. Every cumulative byte
population is at most 64; the marked subtraction has no cross-byte borrow. Each
packing tile consumes exactly eight or four input words and stores one output
word. No extra alignment or padding is required.

The existing sparse lookup and scalar dense scan remain. Their first-word mask,
65-word navigation bound, 4096-bit span check, and guarded tail behavior are
unchanged. A `select15`-only consumer now instantiates the shared 64-width packer
dispatch; typed consumers already included it. Compile-time cost was not measured.

Method
------

Apple M2 Max, AppleClang 21, C++20, `-O3 -DNDEBUG`, strict warnings,
`QOS_CLASS_USER_INITIATED`, without core pinning. Every local build and timing run
held both the host `cpu-heavy` lease and a lease for its build directory.

Each query variant reads the same low/high arrays and matching sample metadata.
Inputs are deterministic monotone residuals; every timed query is checked against
the original residual array. Current, baseline, fixed-size, and prototype builders
produce equal complete encoded checksums. Queries and construction variants
rotate order over five trials. Each query trial performs 524,288 lookups through
65,536 shared positions (512 KiB). The high array is tested at both 0 and 8 modulo
64 using identical query positions. Results are resident-memory timings, with
allocation, construction, and correctness checks outside query timing.

The hot case has 4,096 entries and 5,376 encoded array bytes; the larger case has
2,097,152 entries and 2,752,512 encoded array bytes. Both have low width 8. These
are equal logical inputs and equal encoded layouts, not a claim that the larger
index exceeds every cache. The harness also retains source/oracle arrays,
metadata copies, and owned build outputs; these are excluded from encoded byte
totals, as are C++ object fields and allocator overhead.

Build measurements retain complete owners beyond the timed call, replace the
previous owner, and checksum every output array afterward. They include
allocation and destruction of the previous result. Packing and validation rows
measure those operations separately; validation variants run in fixed order.
Their inputs remain resident. No storage I/O or end-to-end store latency is timed.

Native M2 results
-----------------

Medians in nanoseconds; the query rows below use high-array alignment 0 modulo 64.
Every CSV also includes minimum/maximum, both alignments, and checksums.

| Operation | Baseline | Retained candidate | Evaluated SIMD scan prototype |
| --- | ---: | ---: | ---: |
| Generic select, hot | 38.705 | 29.961 | 36.441 |
| Generic select, larger | 51.300 | 36.274 | 42.919 |
| `select15`, hot | 39.297 | 29.459 | — |
| `select15`, larger | 50.921 | 37.125 | — |
| Generic build, hot, per entry | 1.346 | 1.041 | 1.066 |
| Generic build, larger, per entry | 1.417 | 1.131 | 1.182 |
| `select15` build, hot, per entry | 3.348 | 1.037 | — |
| `select15` build, larger, per entry | 3.420 | 1.080 | — |
| Pack width 8, hot, per entry | 0.168 | 0.081 | 0.081 |
| Pack width 16, hot, per entry | 0.230 | 0.124 | 0.121 |
| Pack width 8, larger, per entry | 0.186 | 0.148 | 0.153 |
| Pack width 16, larger, per entry | 0.231 | 0.144 | 0.146 |
| Validate monotonicity, hot, per entry | 0.836 | 0.264 | — |
| Validate monotonicity, larger, per entry | 0.885 | 0.284 | — |

For the generic hot query, the baseline range was 38.135–38.868 ns and the
candidate range was 29.179–30.262 ns. Larger ranges were 50.476–52.783 ns and
36.232–38.532 ns. The zero-residual and one-sparse-group fixtures also agree with
the oracle; their complete data is retained below. The sparse fixture contains
one exceptional group; it does not isolate exception-only lookup latency.

Rejected prototypes and limits
------------------------------

The four-word SIMD population/batch-scan prototype is slower than the retained
scalar scan plus bounded word select in these cases. This does not rule out other
SIMD select designs. The prototype's width 32 narrowing loop is also excluded:
hot packing measured 0.226 ns/entry versus the baseline 0.163, and larger packing
0.245 versus 0.181. Widths1/7/63 are controls whose packing algorithm is unchanged.

[The stage-one patch](select_stage1.patch) reconstructs the initial broadword and
sharing change from the pinned baseline. [The prototype patch](select_simd_prototype.patch)
then reconstructs the evaluated SIMD scan, width 8/16/32 narrowing, and validation.
They are benchmark fixtures, not additional production backends. The runner's
`--prototype` option reconstructs both automatically; default runs compare only
the baseline and retained implementation.

Rosetta AVX2 medians for generic hot select were 48.498→37.337 ns with BMI2
explicitly disabled, and 46.357→36.932 ns with BMI2 enabled. These are separate
translated runs, not native x86 performance or proof of a PDEP speed advantage.
The BMI2 executable contains `pdepq`. No native x86 measurements were obtained
for this comparison.

Validation and reproduction
---------------------------

The public EF views pass every 16-bit bitmap in each quarter of a word, full-word
selection ordinals, random bitmaps, and exact 1..65-word spans ending at a protected
page. Additional cases exercise low widths 0..63, both source and destination
packing guards, all short constructor tails, unsigned decreases in every SIMD
lane, partial record groups, equal residuals, sparse exceptions, and exact
select15/generic encoded agreement against bit-at-a-time oracles.

`tests/groups.cc` passed in Release and ASan/UBSan on ARM and Rosetta AVX2 with
BMI2 enabled and disabled (six executions). Focused Release+ASan CMake/CTest
passed groups, rank, front, profile, relocated installation, and embedded
consumption (six CTests). Header/source hashes were unchanged during verification.

Run from the repository root, wrapping each command in the host's resource gate
with `cpu-heavy` and the corresponding build-directory lease:

```sh
python3 bench/select_compare.py --prototype --entries 4096 --trials 5 --queries 524288 --output build-select/hot.csv
build-select/select-arm64 2097152 5 524288 dense
build-select/select-arm64 4096 5 524288 zero
build-select/select-arm64 4096 5 524288 sparse
python3 bench/select_compare.py --arch x86_64 --entries 4096 --trials 5 --queries 524288
python3 bench/select_compare.py --arch x86_64 --bmi2 --entries 4096 --trials 5 --queries 524288
```

The x86 commands above execute through Rosetta on macOS; only run an ISA target
on a host that supports it. The benchmark uses Apple/GNU-style compilers.

- [M2 hot](results/select_m2-final-hot.csv), [larger](results/select_m2-final-larger.csv),
  [zero](results/select_m2-final-zero.csv), [sparse](results/select_m2-final-sparse.csv)
- [Rosetta broadword](results/select_rosetta-broadword-hot.csv),
  [Rosetta BMI2](results/select_rosetta-bmi2-hot.csv)
- [Environment and hashes](results/select_environment.json),
  [validation log](results/select_validation.txt)
