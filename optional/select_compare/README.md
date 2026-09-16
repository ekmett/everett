Offset selection on actual file directories
==========================================

I use this optional experiment to compare ways of selecting a sampled record
position. It measures offset access, construction, and space. It does not measure
a complete lookup or merge, change the production format, or establish an x86
performance result from an ARM machine.

Every candidate receives exactly the same monotone integer sequence and inclusive
universe. The native samples include the terminal boundary and already subtract
fixed-width values where the file policy does so. Repeated residual positions,
including an all-zero sequence, are valid inputs. Sparse offsets do not imply
sparse Elias–Fano high bits: I report both $U/n$ and $n/H$, where
$H=(U\mathbin{\gg}L)+n$ is the high-bit length.

Candidates
----------

- `ef-current` is the unchanged production builder and checked random select.
  Its forward cursor is a separate access mode.
- `ef-trusted-control` retains the production 256-one directory, sparse exceptions,
  and word selector. It uses trusted owning arrays directly, removing directory
  validation and mapped/native accessor machinery while retaining ordinal and
  decoded-value checks. Its difference is not solely the cost of error checks.
- `ef-sub32` adds a 16-bit position every 32 high ones, within the existing
  256-one groups, with exact positions for the same long-span exceptions. Its
  measured constructor scans high ones; direct construction from original
  residual positions could reduce that cost.
- `ef-high-direct64` stores every high-one position: an intentionally expensive
  speed bound.
- `ef-sux-simple1` and `ef-sux-simple2` use Vigna's actual `SimpleSelect`, with
  constructor parameters 1 and 2, over the same EF high bits.
- `ef-sux-half` uses the actual `SimpleSelectHalf`. `ef-sux-half-fixed` is a
  separately labeled boundary correction described below.
- `direct64`, `direct32`, and `packed-absolute` store complete offsets, rather
  than an EF payload plus a selector. Direct32 has an explicit ineligible-width row when $U>2^{32}-1$.
  These are select-only representations; they do not supply a general rank index.

The EF alternatives keep identical low/high arrays. They check the ordinal and
decoded value, but trust the constructed high-select directory. Sux itself and
the direct/packed bounds expect valid ordinals. A faster trusted comparator is
not evidence that the production validation can be removed from untrusted files.

Workloads and boundaries
-----------------------

`export` uses the shared [matched logical fixtures](../fixed_kv_compare/README.md)
to construct raw byte KV02, raw bit KV02, typed byte KV02, and typed bit KV03
arrays. It builds a small-to-large fractional-index chain and exports the main augmented
and secondary terminal borrowed streams as well as native input/output streams.
The exporter rebuilds every EF directory and checks its low words, high words,
samples, sparse exceptions, and metadata against the original encoded arrays.
Thus there is no reconstructed guess about the offset distribution.

The retained collection exports all 192 directories. Timing covers native output
and main fractional-index directories, 96 real sequences, across all twelve
logical cases. Six larger sequences replay observed gaps from named real
sequences at $2^{20}$ or $2^{22}$ offsets. These are explicitly derived synthetic
sequences, not real files encoded at that scale. Three further synthetic cases
exercise the external Half boundary at spans 65535, 65536, and 65537.

Input generation, queries, exhaustive output validation, and compilation are
outside access timings. Random and clustered throughput use eight accumulators;
dependent latency feeds each selected value into the next ordinal. The recurrence
includes a modulo operation, whose arithmetic-only control is reported separately,
not subtracted. Binary search reports its actual select count. Forward cursor
runs scan complete sequences. Checksums and compiler barriers keep results alive.
These are library-built fixtures, not captured deployment files. Searching the
offset sequence with lower_bound is an access surrogate, not a key lookup.
These are warm resident-memory measurements, with no mmap, page faults, CRC,
filesystem durability, or fractional-index construction inside the timed access.

Each sequence runs in three fresh processes with an excluded warmup (`trial=-1`)
and three retained trials each. Warmup rows remain in the raw CSV files. Candidate
order rotates between processes. Construction holds a batch of independently
allocated objects until after the stop clock, then validates them exhaustively.
The batch count is exactly `clamp(65536 / n, 1, 256)`. Deallocation is excluded.
`whole_build_ns` includes payload and directory construction. `index_build_ns`
measures only the alternative directory over existing low/high arrays; production
builders have no corresponding isolated entry point, so this field is -1 for
those candidates. Access timings are per call, not per whole binary search.

Space counts
------------

`payload_bytes + auxiliary_bytes` counts immutable array contents, including
word padding, directory sentinels, and sparse spills. For EF, payload means low
and high words; auxiliary means the high-select directory. For direct/packed
representations, the entire representation is payload. The borrowed high bits
are counted exactly once. No invented common descriptor is included.

`allocated_bytes` counts retained array capacity, and `object_bytes` counts
resident C++ objects, including the heap-owned Sux object. Allocator headers and
alignment outside the arrays are not available from these APIs and are excluded.
Constant per-file metadata is also excluded from every candidate. Sux exposes no
persisted file format; its reported array contents are not labeled a serialized
Everett file. Each row includes raw byte counts so percentages can use an explicit
denominator: total representation, intrinsic EF payload, or raw high bitvector.

External source and boundary regression
---------------------------------------

I fetch [Sux](https://github.com/vigna/sux) separately at commit
`568903f1b7957ef03620ebad65d6ef68e031fef9`. Its source remains outside this
repository under its GPLv3 with GCC Runtime Library Exception notice. This
optional adapter does not bundle or relicense those headers. Preparation requires
a clean checkout at that revision.

At this pinned revision, `SimpleSelectHalf` uses `span > 65536` when choosing the
marker but `span < 65536` when choosing the payload layout. Exactly 65536 therefore
writes 64-bit entries while decoding 16-bit entries. The reproducer has 40000
residuals: the first 1024 are zero and the rest are 64512. Its low width is zero,
and high ordinal 1024 is at position 65536. Original Half fails verification;
the collector retains an explicit failure row and no timings for that case.
This is a finding about the pinned source, not a claim about every Sux version.

The [one-character patch](sux-half-boundary.patch) changes `>` to `>=`.
`prepare_sux.py` generates a renamed comparator with unchanged license notices,
plus a manifest of the source and generated hashes. Renaming the class and
rewriting its relative include paths lets both versions coexist. All candidates
are exhaustively checked; original Half is expected to fail only the exact
boundary reproducer among the correctness fixtures.

Reproduce
---------

```sh
git clone https://github.com/vigna/sux /tmp/select-sux
git -C /tmp/select-sux checkout 568903f1b7957ef03620ebad65d6ef68e031fef9
cmake -S optional/select_compare -B build-select -DCMAKE_BUILD_TYPE=Release \
  -DEVERETT_SUX_ROOT=/tmp/select-sux
cmake --build build-select -j1
ctest --test-dir build-select --output-on-failure
python3 optional/select_compare/collect.py --binary build-select/select_compare \
  --sux /tmp/select-sux --output /tmp/select-results
python3 optional/select_compare/analyze.py /tmp/select-results
```

For sanitizer correctness checks, configure a separate build with
`-DSELECT_SANITIZERS=ON`. The collector requires a committed, clean source tree,
records source/binary/external hashes, and refuses to overwrite an existing run.
It retains generated sequences locally; the report manifest hashes them so a
checkout can reproduce them without publishing large generated binary files.

Related measurements
--------------------

The external implementation is [Vigna's Sux](https://github.com/vigna/sux), not
an approximation of its algorithm. I have not measured
[Pasta](https://arxiv.org/html/2206.01149v2) or
[SPIDER](https://arxiv.org/html/2405.05214v1) here. Their reported overheads use
raw bitvectors as a denominator and should not be substituted for total EF space.
The [PDEP word-select study](https://arxiv.org/abs/1706.00990) distinguishes
word-select speed from end-to-end bitvector speed; Everett already has a BMI2
word-select path. This ARM experiment does not measure that path.

The cited results have narrower meanings than a universal select ranking:

- Pandey, Bender and Johnson report 2–4 times faster **word** select and
  20–68% faster full bitvector select in their x86 experiments.
- Kurpicz reports a 16.5% select improvement over cs-poppy in the tested x86
  workload. Table 1 gives 3.58% auxiliary space for pasta-flat, versus 9.88%
  and about 12.20% for simple-select 1 and 2. Those are overheads over the
  underlying bitmap, not ratios of total representations.
- SPIDER reports about 3.8% extra bitmap space. Its claim about eight billion
  bits concerns the strongest **rank** results; it is not a minimum size for
  using its select algorithm. Those measurements also use x86 hardware.

The pinned [pasta SIMD implementation](https://github.com/pasta-toolbox/bit_vector/blob/3ffb6e5a2e58c76425de8197bfe554eb1bf9fd94/include/pasta/bit_vector/support/find_l2_flat_with.hpp)
requires x86 intrinsics. Its scalar binary-search variant is a distinct possible
comparison. The [SPIDER implementation](https://github.com/williams-cs/spider)
also needs an ARM port and input-layout adaptation. I have not inferred their
performance on these small EF high-bit arrays from the published large-bitmap
experiments.

Retained results
----------------

The [Apple M2 Max run](results/2026-09-16-m2max/report.md) retains all 315
processes, including excluded warmups and correctness failures. Its
[evidence guide](results/2026-09-16-m2max/README.md) explains how to verify and
regenerate the tables.

Download the [raw observations](results/2026-09-16-m2max/raw.tar.gz),
[per-sequence summaries](results/2026-09-16-m2max/summary.json.gz), and
[evidence hashes](results/2026-09-16-m2max/sha256.json).
The [integration record](results/2026-09-16-m2max/integration.json) identifies
the identical public sources, and the
[assembly review](results/2026-09-16-m2max/assembly-review.json) records the
retained direct32 load loop.
