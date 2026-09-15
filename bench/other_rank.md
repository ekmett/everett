Packed group and bitmap rank reductions
======================================

I reduce the packed classes directly instead of extracting up to 127 counts
one at a time. The encoding and public APIs are unchanged. I select portable
broadword addition for `K=3`, bounded NEON reductions for `K=7` and `K=31`, and
the existing implementation for `K=15`. Other group sizes keep the general
fallback. The full bitmap rank query also gains a bounded NEON prefix reduction.

The [source](other_rank.cc) and [runner](other_rank.py) compare the pinned
baseline `62ead3fab9d0ee5bda1b47780b7905a45aae1182` with the main-reachable
candidate `5817e02b2672ed92a01bf13b5f4934341bded64a`.
The runner extracts the required headers from Git, so subsequent header edits do
not silently change this comparison. It also generates an all-NEON alternative
by enabling the existing two-bit specialization. [Metadata](results/other_rank_m2max_environment.json)
records the compiler, flags, header hashes, source hash and run parameters.

The implementations
-------------------

A checkpoint begins every 128 classes. Two-, three- and five-bit classes occupy
four, six and ten words per complete checkpoint respectively.

* For `K=3`, the portable reducer adds adjacent two-bit fields into four-bit
  lanes, then adds adjacent four-bit lanes into byte lanes. It accumulates
  at most four words and widens before the final sum. A byte contributes at
  most twelve per word, so this accumulation cannot carry into its neighbor.
* For `K=7` and `K=31`, portable code counts weighted bit planes. Masks rotate
  with the word's position so a class split across words keeps its original
  place weights. NEON processes two words at a time, masks the requested prefix,
  counts the same planes, and widens before combining vectors. The complete
  five-bit checkpoint sums to at most 3968.
* A full bitmap query masks its selected 512-bit run and counts the remaining
  bits with NEON. It performs no payload load at a 512-bit boundary: the
  directory already contains the answer. A short last run retains bounded
  scalar loads.

The NEON group reducers load only when a complete checkpoint is readable;
short checkpoints use the portable path. All loads require only `uint64_t`
alignment. The grouped NEON path requires little-endian AArch64 with NEON.
Other targets use the portable reducers. No ISA flags are exported by the
installed CMake target, and the x86 builder is unchanged.

What I measured
---------------

These are complete public `rank` calls, plus a separate rank-and-class case
that computes `2*rank(g)+class_at(g)`. The latter observes both outputs needed
by window projection. The baseline, explicit NEON alternative and selected
implementation use the public views. `portable_packed` is a checked adapter
over the same encoded arrays and the production portable helper.

Classes are uniformly distributed in `[0,K]`. The oracle is the prefix sum of
the original unpacked population array. Bitmap queries use a separate prefix
oracle over the original source words. Every query, including the complete
dependent sequence, is checked before timing. Each timed checksum must agree
across all implementations.

Independent queries use pregenerated positions. Dependent queries derive the
next position from the preceding answer and a pregenerated random word. Both
include the same indirect-call overhead. Construction, allocation and oracle
work occur outside timing. Each case warms up first, rotates variant order
between five trials, and retains the median and full range. The host resource
lease serializes cooperating CPU-heavy work, and the benchmark requests
`QOS_CLASS_USER_INITIATED`. It does not fix CPU frequency or exclude unrelated
host activity.

The hot cases have 16,421 classes: 5,144 / 7,192 / 11,296 encoded bytes for
`K=3/7/31`. The hot bitmap has 253,577 bits and 32,704 encoded bytes. Larger
cases have 33,554,469 classes and 10,485,784 / 14,680,088 / 23,068,704 encoded
bytes. The larger bitmap has 536,871,049 bits and 69,206,056 encoded bytes.
These sizes count the stored arrays, excluding fixed view fields and allocator
capacity. Each query array occupies 512 KiB hot and 8 MiB large. The different
`K` cases fix class count, not one common virtual universe; comparisons here
are between implementations of the same case.

Native AArch64 results
---------------------

Apple M2 Max, AppleClang 21, C++20, `-O3 -DNDEBUG`; 1,048,576 queries per
trial. Values below are nanoseconds per query, shown as **baseline → selected**.
The [hot CSV](results/other_rank_hot_m2max.csv) and
[larger CSV](results/other_rank_large_m2max.csv) retain all alternatives and ranges.

| Hot case | Independent rank | Dependent rank | Independent rank + class | Dependent rank + class |
| --- | ---: | ---: | ---: | ---: |
| `K=3`, portable | 32.145 → 9.017 | 31.952 → 11.397 | 34.164 → 9.728 | 35.760 → 13.593 |
| `K=7`, NEON | 54.963 → 5.617 | 57.076 → 17.565 | 58.047 → 7.283 | 60.357 → 18.116 |
| `K=31`, NEON | 55.140 → 11.219 | 56.991 → 22.349 | 56.706 → 12.950 | 61.184 → 23.194 |
| Full bitmap, NEON | 9.522 → 4.369 | 16.788 → 13.319 | — | — |

I choose the two-bit portable path for the dependent lookup: its public hot
rank-plus-class result is 13.593 ns, versus 17.446 ns for the all-NEON
alternative. NEON has higher independent throughput (4.953 ns versus 9.728 ns),
so this is an explicit latency/throughput tradeoff.

For `K=7`, the selected NEON rank-plus-class result improves both hot patterns
over the portable adapter: 7.283 versus 15.409 ns independent and 18.116 versus
20.498 ns dependent. For `K=31`, NEON improves independent paired throughput
(12.950 versus 19.408 ns), while the portable adapter has a slightly smaller
hot dependent median (22.330 versus 23.194 ns). A rank-only microbenchmark does
not establish the best choice for window projection.

| Larger case | Independent rank | Dependent rank | Independent rank + class | Dependent rank + class |
| --- | ---: | ---: | ---: | ---: |
| `K=3`, portable | 40.598 → 10.421 | 47.293 → 19.121 | 45.872 → 11.355 | 47.505 → 22.423 |
| `K=7`, NEON | 67.871 → 7.266 | 58.417 → 22.643 | 75.126 → 10.524 | 79.853 → 36.339 |
| `K=31`, NEON | 70.115 → 19.187 | 71.593 → 43.117 | 68.983 → 21.076 | 117.245 → 55.604 |
| Full bitmap, NEON | 25.207 → 23.863 | 109.700 → 104.834 | — | — |

The larger ranges vary substantially. For example, `K=7` dependent paired
queries range from 23.310–42.632 ns for selected NEON and 25.359–51.671 ns for
the portable adapter, whose median is 27.616 ns. The bitmap dependent ranges
are 101.308–114.275 ns baseline and 101.389–109.334 ns selected. I do not infer
a universal large-working-set winner from these overlapping samples. These
are resident-memory measurements, not page-fault, disk-I/O or whole-cascade
results.

Validation and limits
---------------------

* Independent original-population prefix oracles cover every class boundary,
  isolated bits and complements, all-one populations, word/vector crossings,
  unaligned starts, short checkpoints and partial virtual groups. `K=63`
  checks the unchanged general fallback.
* POSIX guard pages cover all short final checkpoints and bitmap tails with
  nonzero padding. A separate test protects the whole bitmap payload and checks
  directory-only 512-bit boundaries.
* CMake rank/group tests pass under native ASan/UBSan. A forced-portable build
  passes the same rank tests with ASan/UBSan. The x86-64 AVX2 test binary passes
  through Rosetta. Installed and embedded package consumers also pass.
* The benchmark checks the pinned baseline and all alternatives under native
  and Rosetta ASan/UBSan. Its AVX2 lookup-popcount builder experiment is retained
  only in the harness; the production x86 builder remains unchanged. No native
  x86 timing was obtained, and Rosetta correctness is not native x86 performance
  evidence.

The stored class packing, checkpoint spacing, public signatures, object sizes
and exceptional-input behavior are unchanged. Borrowed views continue to check
section shapes; these optimizations do not validate untrusted directory contents.

Reproduction
------------

Run through the host's usual exclusive CPU/build resource gate. The runner
does not acquire a second lease internally. Both pinned commits must be present
in the clone's history.

```sh
python3 bench/other_rank.py --build-dir build-other-rank --mode all \
  --trials 5 --queries 1048576
python3 bench/other_rank.py --build-dir build-other-rank-check --mode check \
  --trials 1 --queries 32768 --sanitize
```

The CSV is written to standard output and metadata to
`BUILD_DIR/other_rank_metadata.json`. For a supported x86 target, add
`--cxx-flag=-mavx2 --cxx-flag=-mpopcnt`; these flags apply only to the benchmark.
The `check` mode exercises the builder candidate's oracle without timing it.
