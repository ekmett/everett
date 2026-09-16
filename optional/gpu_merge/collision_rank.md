Collision bitmap and rank experiment
===================================

This checked alternative changes the ordering/deduplication stage of the
optional replacement merge. All six variants pass the full-file CPU oracle;
[complete-path measurements](collision-report.md) show mixed results across shapes. It preserves
the existing compressed input parser, prefix cache, exact FC length scan and
canonical payload/navigation emitters. It is not fractional-index cancellation
and does not enable arbitrary composition handlers.

The rank directory here is temporary GPU construction scratch. It maps
collision bits to survivor positions, then can be discarded after ordered
references have been built. It is not stored in KV03 or IX03 and does not
replace or retune Everett's rank15 directories or their CPU query implementation.
No production rank header changes are part of this experiment. The relevant
measurement is the complete merge pipeline, not a general-rank benchmark.

GPU construction of final rank15 sections is a separate, compatible use of
rank: [index_rank.hlsl](index_rank.hlsl) builds the existing packed classes and
checkpoints from the augmented origin stream. Temporary collision rank removes
older native duplicates; final rank15 counts every borrowed occurrence,
including false borrows. These stages have different inputs and purposes.

Temporary rank layouts
----------------------

The [self-contained rank512 helper](rank512.hlsl) gives each 512-bit block a 32-bit absolute
prefix and seven packed 9-bit prefixes for its 64-bit pairs. An exclusive rank
query uses the directory, at most one preceding 32-bit word, and a masked current
word: at most two popcounts. It retains Edward Kmett's attribution and the
associated [2015 rank/select design notice](rank512_notice.txt).

Everett's [rank.h](../../include/everett/rank.h) instead stores an 8-byte directory
per 2048 bits, with three packed populations for its 512-bit runs and absolute
epochs at $2^{32}$ bits. Its CPU intra-run queries have SIMD implementations.
Everett's rank15 stores populations at 15-occurrence cuts. This temporary stage
also needs positions inside those groups, so it retains the collision bitmap
and a directory suited to those queries. That does not change the final
rank15 layout or prevent its construction on the GPU.

[collision_rank.hlsl](collision_rank.hlsl) directly includes that helper. A small
`Load`/`Load3` adapter lets its unchanged `rank_prefix` template read the existing
structured buffer binding. A parallel constructor emits that same 512-bit
layout, using the current global scan for block prefixes. The `COLLISION_RANK2048`
variant reuses the existing Everett rank-count kernel and its 2048/512 layout.
The inherited `rank_build` and `rank_group_prefix` utilities require exactly
one 256-thread group. This experiment does not invoke them: its constructor is
the independent count → global scan → finish path.
The optional rank512 helper is available under Everett's
`BSD-2-Clause OR Apache-2.0` license; retained third-party notices are unchanged.
The benchmark needs no additional source checkout. Record all included shader
bytes and compile definitions with the measured executable artifact.

Corrected record ordinals
-------------------------

Let A be the older sorted unique input and B the newer sorted unique input.
For each B record, find its lower bound in A. Mark bit $C_i$ exactly when the
keys match. B's uniqueness means at most one incoming record matches an A
record. Atomic OR is still necessary because different matches can share one
bitmap word. A GPU clear precedes marking; rank construction follows completion
of all marks. Bits beyond A's length remain zero.

Write $R(i)=\sum_{t<i} C_t$ for exclusive collision rank, including the endpoint
$R(|A|)$. A surviving A record has final ordinal

$$i+\operatorname{lower\_bound}(B,A_i)-R(i).$$

A B record, whose lower bound in A is $i$, has final ordinal

$$j+i-R(i).$$

These count exactly the surviving A keys and B keys strictly less than the
record. The matched A key is not counted before B because the rank is exclusive;
that A occurrence is separately suppressed. The resulting positions are unique
and fill $|A|+|B|-R(|A|)$ slots without gaps. Every B record remains, including a
newer tombstone. Cancellation here removes an older duplicate occurrence, not a
logical key or an identity arrow.

These are **record** offsets. FC bit lengths still depend on the new predecessor,
selected value and global W15 position. Rank cannot correct old compressed byte
offsets into valid output byte offsets. The existing count → scan → emit stages
run on the resulting ordered references unchanged. All distinct keys remain,
so the replacement merge's direct-source-literal lemma still applies.

The three experimental paths
----------------------------

1. **Independent lower bounds.** Incoming B threads mark collisions and retain
   their lower-bound positions. Each surviving A separately searches B. Both
   inputs then scatter to the corrected positions. This is the simplest oracle
   for the formulas, but costs $O(|B|\log|A|+|A|\log|B|)$ comparisons.
2. **Tiled direct scatter.** A thread handles 32 consecutive records, performs
   one lower-bound search, then advances the other cursor monotonically inside
   its tile. Older A tiles avoid a binary search for every large-base record.
   For balanced inputs, B marking can use the same pattern. For very sparse B,
   keep independent B searches: scanning large gaps could otherwise touch most
   of A merely to find a few collisions. Across tiles, scanned opposite-input
   intervals do not overlap; boundary gaps are skipped by the tile's initial
   search. Work is linear scans plus one search per tile, although uneven key
   spacing can still give individual threads long scans.
3. **Bitmap, compact A, then Merge Path.** Rank compacts surviving A references
   to $i-R(i)$. Those survivors and all B now have disjoint key sets. Merge Path
   merges them directly, without a full-size keep bitmap/flag scan afterward.
   This keeps balanced diagonal partitions and avoids per-A lower bounds, at
   the cost of an additional compact-A reference buffer and pass.

The optional host exposes all three paths with both temporary rank layouts. Existing
Merge Path plus u32 keep flags remains the comparison baseline. Eligibility is
the exact registered replacement specialization. Borrowed index occurrences,
including false borrows, must never use this cancellation bitmap.

Scratch space and likely costs
------------------------------

Ignoring rounded tails and recursive scan scratch, the current baseline keeps
four u32 arrays over $N=|A|+|B|$: ordered references, keep flags, scanned keep
positions and final references, approximately $16N$ bytes. This excludes the
shared input descriptors/tree and later FC lengths, offsets and frames.

Direct scatter needs final references, saved B lower bounds, and rank storage:
approximately $4N+4|B|$ bytes plus the bitmap/directory. Rank512's bitmap and
directory require $(1/8+12/512)|A|\approx0.14844|A|$ bytes; its block totals and
exclusive scan add approximately $8|A|/512$ bytes. Everett's corresponding rank
storage is $(1/8+8/2048)|A|$, with smaller block-scan scratch but more bitmap
loads per query. The compact-A variant adds up to $4|A|$ bytes. A host can later
release temporary buffers when their consumers finish; the first comparison
should count actual retained allocations rather than assume those releases.

The savings trade full-size flag/scatter scans for bitmap clearing, atomic marks,
rank-directory construction and rank queries. Once key prefixes are cached,
those extra passes may matter more than comparison count. Dense collisions can
contend on bitmap words. The two temporary layouts should be compared only by
their contribution to this complete pipeline, with their scratch allocations
included; neither is proposed as a replacement for a production rank layout.

Validation and measurement plan
-------------------------------

The independent [scalar checker](collision_rank_scalar.py) passed 60 bitmap
layout cases and 149 replacement merges. It checks both directory layouts at
every position including EOF, both lower-bound strategies, unique gap-free
scatter, and chronological values/tombstones. Cases cover empty inputs, binary
and proper-prefix keys, all/no/partial collisions, skewed sizes, word/block/tile
boundaries and random inputs. This is not shader validation.

Before any timing, the `collision_rank_probe` kernel should compare GPU queries
at every position against scalar prefix counts. Probe all-zero/all-one/sparse
maps near 31/32/33, 63/64/65, 511/512/513 and 2047/2048/2049 boundaries; all-one
maps also test concurrent atomic hits in the same word. Check final references
against stable CPU replacement before reusing the existing complete KV03 byte,
CRC, decoded-row and adversarial oracles. Guard output allocations and retain
source mappings through command completion.

Suggested separate modes are `collision512`, `collision2048`,
`collision512-tiled` and `collision512-compact`. Start with correctness-only
small cases, then compare one balanced and one skewed fixture against the exact
same cached/word-emitter baseline. If justified, use the existing three-trial
matrix and add a sparse incoming case. Include clearing, collision searches,
atomics, rank construction/scans and scatter in the ordering and whole-path
times; report actual temporary capacities. Do not attribute a full merge result
to rank alone, and do not change durable barriers or claim durable throughput.

Dispatch bindings
-----------------

The compressed source/cache bindings stay unchanged. Match/scatter parameters
are `{A+B, A, B, unused, payload_delta_A, payload_delta_B, tree_base}`.

| Pass | Binding 0 | Binding 1 | Binding 2 | Binding 5 | Binding 6 |
|---|---|---|---|---|---|
| clear | unused | rank storage | unused | unused | unused |
| mark / mark_tiled | compressed A | rank storage | unused | unused | saved B lower bounds |
| Rank512 count | bitmap/rank storage | same rank storage | block totals | unused | unused |
| Rank512 finish | scanned totals | rank storage | block totals | unused | unused |
| Everett rank_count | bitmap | directory | block totals | unused | unused |
| Everett finish | scanned totals | directory + total | block totals | unused | unused |
| scatter / compact A | compressed A | output references | Everett directory when selected | B lower bounds when used | rank storage/bitmap |
| merge compact A | compressed A | final references | unused | compact A references | unused |
| rank probe | unused | query results | Everett directory when selected | query positions | rank storage/bitmap |

Clear/count/finish use parameter 0 as A. Match/scatter retain descriptor binding4,
compressed B binding7, common-prefix binding8 and min-tree binding9. Compact-A
Merge Path additionally receives surviving-A count in parameter3. Rank512 finish
and Everett finish dispatch at least one invocation for an empty A. The host reads
one final rank total to obtain survivor count; it does not expand record ranks.
