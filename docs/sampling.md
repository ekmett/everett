# Sampling groups and the choice of K

Updated 2026-09-15. The default remains **K = 15**. The typed byte and bit
profiles support **3, 7, 15 and 31** through
`storage_policy<Unit, Values, K>`. K counts entries; it does not change whether
key lengths, backspaces, values and physical offsets count bytes or bits.

K = 3 is sufficient for the local navigation rules and for constant per-level
augmentation along a single chain whose capacities grow by at most two per
link. It increases sampling space while reducing the number of records examined
in each window. This document derives that statement and states its assumptions.
It does not establish a complete redundant-level scheduler.

## 1. Relationship to the COLA reference

Bender et al.'s *Cache-Oblivious Streaming B-trees*, §3, describes power-of-two
COLA levels. Its lookahead layout samples every eighth entry and also allocates
duplicate-pointer cells. Its deamortized construction distinguishes visible and
shadow arrays; completing a data merge alone does not finish the associated
lookahead work. These are the scheduling and multi-catalog-search precedents,
not the count-class layout used here.
[Original paper, §3](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf#page=8).

**The recurrence and constants below are our derivation for Everett's separate
native and borrowed streams.** They are not a restatement of the paper's slot
allocation or a claim that its deamortization proof applies unchanged.

## 2. Why a local window works for K = 3

A catalog's augmented order is the virtual sorted interleaving of its native
records and borrowed keys. Borrow positions `0, K, 2K, ...` from the exact
**augmented** target catalog, including its borrowed keys. Sampling only native
target keys would leave the size of a target window unbounded.

Let R(g) count borrowed entries before virtual position Kg. The virtual window
`[Kg, min(K(g+1), size))` projects to one native range and one borrowed range.
Their lengths sum to at most K. Boundary ranks suffice: native rank is virtual
position minus borrowed rank. These facts hold for K = 3 just as they do for 15.

The remaining navigation rules also survive:

- Each projected physical range touches at most two physical sampling groups.
  Reaching its first record inspects at most K − 1 preceding record headers.
- Native entries precede equal borrowed entries. Every borrowed copy keeps its
  own false-borrow flag. If equal copies span several windows, the flag and
  native boundary rank identify the earlier unique native slot directly.
- A predecessor borrowed key can lie just before the selected window. The
  conservative prefix policy supplies its query-relevant context with one
  additional record probe, even when this window has no borrowed entries.
- A native match and a downstream route can both be returned. Equality does
  not terminate a general arrow-valued query; older matching segments may
  still contribute to composition.

The caller must supply the window containing the last augmented key at most
the query, using the first window when the query precedes the whole catalog.
The incoming boundary key supplies the decoding anchor. A small root or a
separately specified root search establishes the first window.

For fixed K, navigation remains O(K) entry/header work per catalog plus the
constant extra frontier probe. This is not a constant bound on bytes read or
arrow evaluation: records can have long keys or values. Prefix decoding and
payload access retain their work contracts from the [main design](design.md)
and [categorical update model](arrows.md).

## 3. Per-level augmentation along one chain

Index a nonempty chain of catalogs from small to large by \(i=0,\ldots,L-1\).
Let:

- \(n_i\) be the number of native records in catalog \(i\).
- \(A_i\) be its augmented record count, including borrowed records.
- \(B_i\) be its nominal native capacity, with \(n_i\le B_i\).

Assume one exact downstream target per catalog and terminal
\(A_{L-1}=n_{L-1}\).
Sampling position zero and then every Kth position gives

\[
A_i=n_i+\left\lceil\frac{A_{i+1}}K\right\rceil.
\]

Every retained sample counts in this recurrence, including a false borrow or
an equal key sampled more than once. For nominal capacities satisfying
\(B_{i+j}\le 2^j B_i\), write each ceiling error as \(\varepsilon_i\) in
\([0,1)\). Unrolling gives

\[
A_i=\sum_{j=0}^{L-1-i}\frac{n_{i+j}}{K^j}
  +\sum_{j=0}^{L-2-i}\frac{\varepsilon_{i+j}}{K^j}.
\]

Therefore, for K > 2,

\[
A_i\le B_i\sum_{j\ge0}\left(\frac2K\right)^j
       +\sum_{j\ge0}\frac1{K^j}
   =B_i\frac K{K-2}+\frac K{K-1}.
\]

The final additive constant covers ceiling effects. With fully occupied levels
that double in native size, the leading **per-level amplification** is
\(K/(K-2)\); the borrowed/native ratio is \(2/(K-2)\). For sparse levels the bound is
against nominal capacity, not the actual \(n_i\), which can be zero.

| K | Count bits per group | Count bits per augmented entry, ignoring tail/directory | Leading total/native ratio at a full level | Leading borrowed/native ratio |
| --- | --- | --- | --- | --- |
| 3 | 2 | 2/3 ≈ 0.667 | 3 | 2 |
| 7 | 3 | 3/7 ≈ 0.429 | 7/5 = 1.4 | 2/5 = 0.4 |
| **15** | **4** | **4/15 ≈ 0.267** | **15/13 ≈ 1.154** | **2/13 ≈ 0.154** |
| 31 | 5 | 5/31 ≈ 0.161 | 31/29 ≈ 1.069 | 2/29 ≈ 0.069 |

Thus K = 3 allows roughly two native capacities of borrowed entries at a full
level. A scheduler choosing K = 3 must reserve that augmentation instead of
assuming the K = 15 capacity constants.

### What the critical value means

At K = 2 the geometric ratio is one. In the exact doubling example, every
remaining depth contributes another \(n_i\) to \(A_i\), so \(A_i/n_i\) grows with the
remaining chain length. This breaks a constant per-level capacity argument.
It does not prove that every possible K = 2 data structure is impossible.
For a general per-link capacity growth bound g, the same argument requires
\(K>g\) and gives leading factor \(K/(K-g)\).

Nor is the table a multiplier on the entire store's byte size. For one finite
chain, summing its recurrence yields the separate bound

\[
\sum_i A_i
\le\frac K{K-1}\left(\sum_i n_i+L-1\right).
\]

This weaker global entry-count bound holds for K > 1 without a growth
assumption. The stronger per-level bound is what controls small catalogs and
their reserved capacities. Neither expression measures encoded string bytes,
unfinished outputs or retained historical worlds.

## 4. Space and scan work

For \(K=2^b-1\), a group population ranges from 0 to K and occupies exactly b
bits. `rank_groups<K>` stores those classes and prefix checkpoints; it does
not store the full origin pattern or require arbitrary within-group rank.
The table omits checkpoint overhead and the final partial group's padding.

Each physical stream has its own `select_groups<K>` over record boundaries
`0, K, 2K, ...` and its exact end sentinel. For a fixed encoding, increasing K
reduces the number of marked positions. The Elias–Fano cost depends on both
the number of marks and their residual universe, so it is not simply one
constant-size pointer saved every K entries.

Residual offsets remove a fixed value stride only when that width is common
to the entire indexed stream. A native fixed-value policy and its borrowed
zero-value role retain the same policy type while using different strides.
The physical offset units match the profile: byte offsets for the byte profile,
bit offsets for the bit profile. Rank groups still count entries in either case.

Increasing K usually reduces sample count and class/offset metadata, while
increasing the number of candidate records and framing headers inspected per
window. It can also change which borrowed prefixes need repair. Actual encoded
space, cache behavior, reconstruction work and value access therefore need
measurement; the count-class table alone does not select a fastest K.

## 5. Native-preserving index repair

Native keys use LPFC, with default restart factor 18. The native representation
is independent of the borrowed index's current shared cuts. For borrowed key
\(S_j\), ordinary FC retains the adjacent LCP \(a_j\). At each applicable virtual cut
Kg, the modified policy additionally limits the retained prefix of the preceding
borrowed key to its LCP with that cut's boundary key. Re-emitted units supply
the context needed to decode from the upper anchor.

Changing K moves these cuts, but does not invalidate the argument. The
bidirectional reference policy retains \(\min(a_j,a_{j+1})\), with zero endpoint
LCPs. Its extra suffix units are the positive falls of the adjacent-LCP sequence;
these equal the positive rises, bounded by ordinary FC's suffix units. The
actual-cut policy is no more redundant. Consequently its borrowed suffix units
are at most twice ordinary borrowed FC, independently of K. Headers, directories
and extra caller-imposed ceilings remain separate charges.

Within one P, `profile_blob<P>::reindex` preserves the exact native allocation
and native offset index while rebuilding borrowed data, rank classes and false-
borrow flags. Retained worlds can keep using the previous index. Its new routing
still needs the exact target identities and pin/publication protocol supplied
by the surrounding store.

Changing K changes P and the native physical group checkpoints as well. That is
a profile/format migration, not the same operation as reindexing against a new
target under one fixed P; this document does not promise native-byte reuse
across such a migration.

## 6. Where the chain proof stops

**Skipped levels.** A shortcut over d nominal levels can increase capacity by
\(2^d\) in one sampled edge. The twofold-per-link premise no longer holds. Keep
intermediate index catalogs, including catalogs with no native entries, or prove
and reserve the shortcut's larger augmentation separately. A large sample
stream cannot be charged to a source catalog's zero native occupancy.

**Several augmented children.** If one catalog samples d children whose nominal
capacities are each at most g times its own, the corresponding uniform bound
needs \(dg/K<1\). For example, two children each twice as large give a \(4/K\)
coefficient; the single-chain K = 3 argument does not cover that topology.
An actual dependency graph needs its own recurrence and accounting.

**Redundant arrays.** A bounded number of arrays per level can be handled by
separate chains or by a linear ordering that preserves the per-link growth
bound, but those are explicit structural choices. Array multiplicity alone does
not establish the chosen query graph or its buffer capacities.

**Scheduling and persistence.** The future scheduler must fund native merges,
borrowed-prefix reconstruction, rank/offset construction and publication at the
selected K's constants. It must preserve bounded active levels while forks may
adopt shared results at different times. Old snapshots, readers and checkpoints
retain their own dependency graphs. Local decoding correctness does not supply
these work deadlines or bound those historical bytes. Live-size rebuilding and
durable publication retain their separate contracts in [rebuild.md](rebuild.md)
and [durability.md](durability.md).

## 7. Executable evidence and remaining proof work

[`tests/profile_blob.cc`](../tests/profile_blob.cc) exercises byte and bit keys,
variable and fixed values, and K = 3, 7, 15 and 31: sixteen configurations. The
strict C++20 ASan/UBSan suite passed. Its oracles check projected windows,
query-limited decoding, every matching native segment along a sampled chain,
values with exact meaningful-bit lengths, many equal borrowed samples spanning
cuts, the borrowed-suffix bound, and old/new indexes sharing native bytes.

These are local codec and query results. They do not test a complete COLA merge
scheduler, disk publication, admission deadlines or crash recovery. The next
scheduler proof must name its actual active graph, native capacities, index
budgets, visible/unfinished representations and retained-history charges before
claiming a store-wide bound for any K.
