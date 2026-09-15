# Sampling groups and the choice of K

Updated 2026-09-15. **K = 15** is the default. The typed byte and bit
profiles support **3, 7, 15 and 31** through
`storage_policy<Unit, Values, K, BackspaceCode, W>`. K counts virtual entries;
W counts physical records between offset checkpoints and defaults to K. Neither
changes whether keys, backspaces, values and offsets count bytes or bits. Exact
query agreement and cut LCPs always count bits.

K = 3 is sufficient for the local navigation rules and for constant per-level
augmentation along a single chain whose capacities grow by at most two per
link. It increases sampling space while reducing the number of records examined
in each window. Below, we derive that statement under explicit assumptions.
I still need a complete redundant-level scheduler proof.

## 1. Relationship to the COLA reference

Bender et al.'s *Cache-Oblivious Streaming B-trees*, §3, describes power-of-two
COLA levels. Its lookahead layout samples every eighth entry and also allocates
duplicate-pointer cells. Its deamortized construction distinguishes visible and
shadow arrays; completing a data merge alone does not finish the associated
lookahead work. This gives us a precedent for scheduling and multi-catalog
search. Everett uses a different count-class layout.
[Original paper, §3](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf#page=8).

**We derive the recurrence and constants below for Everett's separate native
and borrowed streams.** The paper's slot allocation and deamortization proof
need separate adaptation to this representation.

## 2. Why a local window works for K = 3

A catalog's augmented order is the virtual sorted interleaving of its native
records and borrowed keys. We borrow positions `0, K, 2K, ...` from the exact
**augmented** target catalog, including its borrowed keys. Sampling only native
target keys would leave the size of a target window unbounded.

Let R(g) count borrowed entries before virtual position Kg. The virtual window
`[Kg, min(K(g+1), size))` projects to one native range and one borrowed range.
Their lengths sum to at most K. Boundary ranks suffice: native rank is virtual
position minus borrowed rank. These facts hold for K = 3 just as they do for 15.

We retain the remaining navigation rules as well:

- Each stream has its own physical checkpoints every W records. Reaching a
  selected lane parses at most W − 1 preceding controls, without reconstructing
  or comparing those earlier keys. Forward ranges contain at most K records in
  total. A slice touches at most two physical blocks when W ≥ K; arbitrary W
  needs the corresponding block-count bound.
- Native entries precede equal borrowed entries. Every borrowed copy keeps its
  own false-borrow flag. If equal copies span several windows, the flag and
  native boundary rank identify the earlier unique native slot directly.
- A predecessor borrowed key C can lie just before the selected window. At
  boundary B, the exact index-local $\ell=\mathrm{lcp}_{\mathrm{bits}}(C,B)$
  repairs its query comparison by taking the minimum with the incoming exact
  agreement. The known order resolves equality without fetching C's length.
  C is absent iff borrowed rank
  is zero, and remains present when that rank equals a nonzero stream length.
- A native match and a downstream route can both be returned. Equality does
  not terminate a general arrow-valued query; older matching segments may
  still contribute to composition. A false-borrow value outside the projected
  range is a separate bounded ordinal probe.

The caller supplies the window containing the last augmented key at most the
query, using the first window when the query precedes the whole catalog. The
incoming comparison describes that boundary against this exact query. A
`query_root` prepares an empty-native routing prefix until the head fits within
one group, establishing the first window from the empty-key comparison.

Navigation takes O(K + W) entry/control work per catalog. For fixed K and W,
this is constant per catalog; it is not a bound on encoded count bits, compared
literal bits, requested value bytes or arrow evaluation. The
[comparison design](comparison-fc.md), [main design](design.md), and
[categorical update model](arrows.md) state those separate contracts.

### Constructing samples from a pinned pair

Start by pinning one exact, immutable `.kv` + `.index` pair. Its augmented order
is a **tagged occurrence sequence**: we preserve all borrowed copies, including
false borrows, with native entries before equal borrowed entries and equal
borrowed entries in their original order. Sample positions `0, K, 2K, ...` in
that sequence. The pair stays pinned; extraction neither compacts it nor
requires a physically merged copy or re-encoded target.

At cut $t=jK$, let $r(j)=R(j)$ be the borrowed population before that cut.
The next native and borrowed ordinals are respectively
$jK-r(j)$ and $r(j)$. The lesser available next key, using the same tie rule,
is the sample. The two physical Elias–Fano indexes locate the containing groups;
bounded header scans locate those records. They provide **locations**, not
independent full-key reconstruction context. Both streams use ordinary FC:
repeatedly calling `reconstruct_at` at successive sample positions can walk the
same long prefix chain repeatedly. Query comparison contexts do not supply the
complete sample keys a builder needs to emit.

Two sequential decoding cursors suffice. Keep their key contexts, advance in
merged order, and emit every Kth occurrence. Value framing lets us skip the
payloads. If we're building the target index at the same time, its merged-order
walk can emit these samples while constructing rank classes and exact cut LCPs.
Otherwise we make a separate pass. In either case, charge
visited headers, prefix/suffix decoding, key comparisons and emitted sample
bytes. A separate streaming pass can visit all A augmented entries; producing
only $\lceil A/K\rceil$ samples does not make it O(A/K) work.

`sample_cursor<P, Target>` implements this scan over an owned pin to the exact
pair. `Target` defaults to `profile_blob<P>` and can be `mapped_blob<P>`.
It keeps two decoding contexts, preserves tagged ordering and decodes each
record once over a full traversal. Values remain views of their source payloads.
`index_builder<P>` consumes consecutive samples with one incoming lookahead and
one outgoing slot. `index_pipeline<P>` connects the stages and binds each
completed pair to the exact target that supplied its samples. The batch
`profile_blob<P>::build`/`reindex` interfaces still accept supplied samples;
tests use a separately materialized ordering as their independent oracle.
Low-level builders trust their sampler's provenance and check order, ordinals
and count; these checks do not authenticate arbitrary externally supplied keys.

`index_builder<P, Native>` can likewise read a pinned `mapped_native<P>` in
place; its default native type is `profile_array<P>`. `finish_index(target_size)`
returns a `profile_index<P>` containing only the newly constructed borrowed
stream and navigation metadata. `encode_index_sections` writes that artifact
against the unchanged native identity and exact target identity. The native
FC bytes and Elias–Fano offsets remain in their original file. The builder
retains its native owner, and the sampler retains its target pair; the caller
keeps the corresponding catalog pins through sealing and publication.

The standalone artifact does not own either dependency. Its supplied target
extent checks the number of samples, while binding the sealed mapped pair checks
declared identities and shapes. An explicit scan checks sampled key contents.
For a terminal pair, `profile_index<P>::native_only(native_count)` constructs an
empty borrowed stream and all-zero rank and cut directories from the admitted
count alone, without walking native keys. Closing and draining an index builder
against an empty target produces the same representation.

**Unselected space/time option.** An ephemeral outgoing-sample sink avoids the
second key-encoding pass only when a fresh target index and its upstream samples
are built together. For an arbitrary existing pair, the baseline extractor performs the key scan
described above; skipping values does not remove that scan. A retained, front-coded outgoing sample section within `.index` could
let later forks reuse the export. That adds space beyond the original minimal
blob, and sampling every Kth occurrence does not guarantee retaining only 1/K of
the key bytes. Reusing that export requires the exact containing native/index pair and its
sampling policy. A downstream-index change creates a new pair whose export must
be rebuilt or validated anew; the native bytes stay unchanged. I leave this as
an option for measurement; I have not selected or implemented the format extension.

### Streaming construction pipeline

`index_pipeline<P>` keeps a few fingers per index under construction. Each stage
merges its native keys with incoming samples from its exact target pair and
passes every Kth augmented occurrence to the next stage. Ordinary front coding
is enough for the handoff: a literal first sample, then a backspace count in P
units and suffix relative to the preceding sample from that producer. Producer
and consumer retain one coding context each. The final borrowed stream also
uses ordinary FC; its exact cut LCPs are separate metadata. Each stage retains:

- Native and incoming-sample positions, with a next-key lookahead or explicit
  end-of-stream for each input. A temporarily empty queue is not end-of-stream.
- The virtual ordinal and its residue modulo K, cumulative borrowed rank and
  the current partial rank class.
- Front-coding contexts, the last borrowed key, and the exact bit LCP between
  that key and each virtual cut boundary.
- Physical block positions at interval W, absolute retained-prefix counts, final
  key lengths, and residual offsets staged until the final extent determines
  the Elias–Fano encoding.

If a stage has n native entries and receives s samples, it emits
$\lceil(n+s)/K\rceil$ samples. Thus the contribution from one original source
shrinks by roughly K per edge, but every stage adds its own native entries.
Bounded queues and backpressure let stages advance at different rates; they
must retain enough lookahead to establish the next merged key. Key lengths,
prefix work and values mean that byte traffic and execution time need not shrink
by K along with the sampled occurrence count.

A sample key and target ordinal can become stable before the output file is
sealed. Construction may forward those samples immediately under the reserved target-pair generation.
The implemented pipeline produces encoded in-memory pairs. Its step budget
counts cursor events; source advances consume at most K records, while key
bytes and allocations have their own costs. `finish()` separately constructs
the remaining Elias–Fano/rank metadata and attaches exact target pins. It can
perform linear work in staged metadata and is not a worst-case scheduler bound.
The completed graph is the input to the [durable publication protocol](durability.md);
old representations keep their pins throughout construction.

## 3. Per-level augmentation along one chain

We index a nonempty chain of catalogs from small to large by $i=0,\ldots,L-1$.
Let:

- $n_i$ be the number of native records in catalog $i$.
- $A_i$ be its augmented record count, including borrowed records.
- $B_i$ be its nominal native capacity, with $n_i\le B_i$.

We assume one exact downstream target per catalog and terminal
$A_{L-1}=n_{L-1}$.
Sampling position zero and then every Kth position gives

$$
A_i=n_i+\left\lceil\frac{A_{i+1}}K\right\rceil.
$$

Every retained sample counts in this recurrence, including a false borrow or
an equal key sampled more than once. For nominal capacities satisfying
$B_{i+j}\le 2^j B_i$, write each ceiling error as $\varepsilon_i$ in
$[0,1)$. Unrolling gives

$$
A_i=\sum_{j=0}^{L-1-i}\frac{n_{i+j}}{K^j}
  +\sum_{j=0}^{L-2-i}\frac{\varepsilon_{i+j}}{K^j}.
$$

Therefore, for K > 2,

$$
A_i\le B_i\sum_{j\ge0}\left(\frac2K\right)^j
       +\sum_{j\ge0}\frac1{K^j}
   =B_i\frac K{K-2}+\frac K{K-1}.
$$

The final additive constant covers ceiling effects. With fully occupied levels
that double in native size, we obtain a leading **per-level amplification** of
$K/(K-2)$; the borrowed/native ratio is $2/(K-2)$. For sparse levels the bound is
against nominal capacity, not the actual $n_i$, which can be zero.

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
remaining depth contributes another $n_i$ to $A_i$, so $A_i/n_i$ grows with the
remaining chain length. This breaks a constant per-level capacity argument.
It does not prove that every possible K = 2 data structure is impossible.
For a general per-link capacity growth bound g, the same argument requires
$K>g$ and gives leading factor $K/(K-g)$.

Nor is the table a multiplier on the entire store's byte size. For one finite
chain, summing its recurrence yields the separate bound

$$
\sum_i A_i
\le\frac K{K-1}\left(\sum_i n_i+L-1\right).
$$

This weaker global entry-count bound holds for K > 1 without a growth
assumption. The stronger per-level bound is what controls small catalogs and
their reserved capacities. We have not counted encoded string bytes, unfinished
outputs or retained historical worlds in either expression.

## 4. Space and scan work

For $K=2^b-1$, a group population ranges from 0 to K and occupies exactly b
bits. `rank_groups<K>` stores those classes and prefix checkpoints; it does
not store the full origin pattern or require arbitrary within-group rank.
The table omits checkpoint overhead and the final partial group's padding.
Exact cut LCPs are a separate unsigned 64-bit count per virtual group: currently
$8\lceil A/K\rceil$ bytes for A augmented entries, including unused zero
slots where no borrowed predecessor exists. They must be included in total
index space; the packed class width is not the whole navigation overhead.

Each physical stream has its own `elias_fano` over record boundaries
`0, W, 2W, ...` and its exact end sentinel. Increasing W reduces the number of
physical marks but increases the number of controls parsed to reach a selected
lane. The Elias–Fano cost depends on both the mark count and the residual
universe. There is no third directory for a materialized augmented stream.

Residual offsets remove a fixed value stride only when that width is common
to the entire indexed stream. A native fixed-value policy and its borrowed
zero-value role retain the same policy type while using different strides.
The physical offset units match the profile: byte offsets for the byte profile,
bit offsets for the bit profile. K and W count records in either case.

The two tuning choices are independent. Increasing K reduces downstream samples,
rank classes and exact cut-LCP counts, but permits more forward candidates per
window. Increasing W reduces physical checkpoints and offsets, but permits more
control parsing before those candidates. With W = O(K), navigation remains
O(K) entry/control work. A power-of-two W such as 16 is compatible with K = 15;
only K needs the $2^b-1$ form for packed class counts. Actual encoded space,
cache behavior, literal comparisons and value access need measurement. The
count-class table alone cannot tell us which policy is fastest.

## 5. Native-preserving index repair

Both physical streams use ordinary FC. The native stream's adjacent prefixes
and W-spaced physical checkpoints depend only on its native records. The borrowed
stream's prefixes depend only on its borrowed records. The exact cut LCPs depend
on their virtual interleaving, so they live in the independently rebuilt index.
No prefix needs to be shortened or re-emitted merely to align those two streams.

For borrowed rank $i>0$ at a cut, construction records the exact bit LCP of
borrowed key $i-1$ and that cut's boundary. If the same borrowed key precedes
several cuts, every cut retains its own scalar. A single minimum across cuts
would lose information needed for query comparison. If $i=0$, the scalar is
unused and stored as zero. The terminal $i=I>0$ case still has a predecessor;
its full length comes from stored terminal metadata.

Within one P, `profile_blob<P>::reindex` preserves the exact native allocation
and native offset index while rebuilding ordinary borrowed FC, rank classes,
cut LCPs and false-borrow flags. Retained worlds can continue using the previous
index. New routing retains the exact target identities and follows the
pin/publication protocol supplied by the surrounding store.

The policy includes both K and W. Changing W changes physical checkpoints and
offset sampling; changing K changes virtual samples and ranks. Typed builders
require a common P throughout the chain. Native sharing during `reindex` applies
to one fixed P; it is not permission to reinterpret different policy metadata.

## 6. Where the chain proof stops

**Skipped levels.** A shortcut over d nominal levels can increase capacity by
$2^d$ in one sampled edge. The twofold-per-link premise no longer holds. Keep
intermediate index catalogs, including catalogs with no native entries, or prove
and reserve the shortcut's larger augmentation separately. A large sample
stream cannot be charged to a source catalog's zero native occupancy.

**Several augmented children.** If one catalog samples d children whose nominal
capacities are each at most g times its own, the corresponding uniform bound
needs $dg/K<1$. For example, two children each twice as large give a $4/K$
coefficient; the single-chain K = 3 argument does not cover that topology.
An actual dependency graph needs its own recurrence and accounting.

**Redundant arrays.** A bounded number of arrays per level can be handled by
separate chains or by a linear ordering that preserves the per-link growth
bound, but those are explicit structural choices. Array multiplicity alone does
not establish the chosen query graph or its buffer capacities.

**Scheduling and persistence.** We still need a scheduler that funds native merges,
borrowed-stream construction, cut LCPs, rank/offset construction and publication
at the selected K/W constants. It must preserve bounded active levels while forks may
adopt shared results at different times. Old snapshots, readers and checkpoints
retain their own dependency graphs. Local decoding correctness does not supply
these work deadlines or bound those historical bytes. Live-size rebuilding and
durable publication retain their separate contracts in [rebuild.md](rebuild.md)
and [durability.md](durability.md).

## 7. Executable evidence and remaining proof work

[`tests/profile_blob.cc`](../tests/profile_blob.cc) and
[`tests/query.cc`](../tests/query.cc) compare local windows and complete cascades
with independent sorted-record oracles. The relevant cases include byte and bit
keys, fixed and variable values, K = 3, 7, 15 and 31, independent physical W,
empty streams, proper prefixes, and equal borrowed occurrences spanning cuts.
The tests check every native match and its value, query comparison transfer,
absent and terminal borrowed frontiers, false-borrow recovery, and old/new indexes
sharing native bytes. The [implementation ledger](implementation.md) records
the verified configurations and build results.

The [Lean model](../proof/README.md) proves sampled-window and ordered-prefix
laws over abstract keys. Those results and the executable oracles address
different layers. Neither establishes a complete COLA merge scheduler, disk
publication, admission deadline or crash recovery implementation. A store-wide
bound for any K also needs a scheduler proof naming the actual active graph,
native capacities, index budgets, visible/unfinished representations and
retained-history charges.
