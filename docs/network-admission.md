# Admitting sorted blobs from peers

The intended flow is to retain received immutable native blobs, chain them onto
an existing world, build fractional indexes back over the new prefix, and spend
merge work as admission proceeds. Arbitrary incoming sizes are compatible with
local cascading. Their scheduling, publication latency and byte costs require
separate bounds. This document derives those distinctions; it does not specify
an implemented admission scheduler.

## 1. What can be adopted directly

A received native blob can keep its content-addressed identity and bytes;
its receiver-specific fractional index is a separate immutable object. Reusing
an existing index also requires its exact downstream augmented catalog version,
including sampled ordinals and duplicate ordering. An equal logical-state
fingerprint does not establish that routing layout. Pin the exact dependencies
until a replacement index is complete.

A standalone prebuilt base has no downstream catalog to index. It can be the
first searchable catalog immediately after validation. A preindexed prefix can
also be adopted when its complete dependency chain and policy match. Neither
case implies that a new tiny batch can cheaply build an index against an
arbitrary large existing head.

Admission preserves per-key chronology. Disjoint partition updates from one
shared base commute; arbitrary same-key arrows need their original composition
order. Assigning files size classes is not permission to sort history by size.
Merges must preserve that order, for example by combining contiguous history
intervals. See [categorical updates](arrows.md).

## 2. Exact new-prefix entry accounting

Fix the sampling interval \(K\ge3\). Index the new prefix from its new head
\(i=0\) toward its existing suffix at \(i=q\). Let:

- \(q\): number of received native files;
- \(n_i\): native entry count of new file \(i\);
- \(S=\sum_{i=0}^{q-1}n_i\): total incoming native entries;
- \(c=A_q\): augmented entry count of the unchanged old head;
- \(A_i\): augmented count of new catalog \(i\), including borrowed entries.

Sampling the **augmented** next catalog, including its borrowed entries, gives

\[
A_i=n_i+\left\lceil A_{i+1}/K\right\rceil,
\qquad
A_i=\left\lceil\sum_{j=i}^{q-1}\frac{n_j}{K^{j-i}}
                   +\frac{c}{K^{q-i}}\right\rceil.
\]

The closed form follows from the nested-ceiling identity for integer \(K\).
No ordering of the \(n_i\) by size is used.

Let \(B=\sum_{i=0}^{q-1}\lceil A_{i+1}/K\rceil\) count all newly borrowed
entries. Since \(\sum A_i=S+B\) and
\(\sum A_{i+1}=S+B-A_0+c\), integer rounding yields

\[
B\le\frac{S+c-A_0}{K-1}+q\le\frac{S+c}{K-1}+q.
\]

For fixed \(K\), unit-cost entries and \(c=O(S)\), all new-prefix indexing
therefore takes \(O(S+q)\) entry work. The old head may be scanned once to
extract samples; this also fits that bound. String decoding and emitted bytes
are additional costs, addressed below.

A tiny first prepend before a head of \(c\) entries still materializes
\(\lceil c/K\rceil\) borrowed entries. With successive prepends onto the
previous head, the old-head contribution then decays as
\(c/K,c/K^2,\ldots\), up to rounding: the aggregate bill is \(O(c/(K-1)+q)\),
not \(c/K\) anew for every prepend. That conclusion assumes the old suffix
stays fixed. Rebuilding against changed targets needs its own repair charge.

Credit reserved when the large head arrived can fund this aggregate work.
It does not make the first large dependency finish within a tiny arrival's
worst-case work allowance. Prepayment and completion time are different facts.

## 3. Query bounds and scheduling obligations

With valid outgoing contexts, search can start in any first catalog using
\(O(\log(A_0+1))\) entry comparisons and visit at most \(K\) candidates per
remaining catalog. For chain depth \(D\), the abstract entry bound is
\(O(\log(A_0+1)+KD)\); key reconstruction, values and arrow evaluation are
separate work. A large first fractional index does not add another logarithm.
Monotone file sizes are not needed for this local correctness argument.

A complete chain with \(R\) native entries has \(A_0\le R\). To conclude
\(O(\log(R+1))\) search for fixed \(K\), its published depth must also be
\(O(\log(R+1))\). Live bindings \(N\) replace \(R\) only after the
[history-to-live-size rebuilding invariant](rebuild.md) is established.

Depth does not follow from cheap indexing. A chain of one-entry files has
\(A_0\le2\) yet needs one visit per file. Empty routing catalogs eventually
carry one sample forever because \(\lceil1/K\rceil=1\). A minimum file size
bounds depth by total size divided by that minimum, not by its logarithm.

Geometric size control helps bound repairs and level occupancy. For example,
if \(n_{i+j}\le Cg^j n_i\) with \(g<K\), the recurrence bounds \(A_i\) by
\(Cn_i/(1-g/K)+1\). Without such control a tiny native file can own a large
fractional index. That affects local capacities even when total entry space
is linear. Small-to-large schedules also keep repaired predecessors small
relative to the downstream merge that changed their target.

Comparable-size merges and classes such as \(\lfloor\log_2 b\rfloor\) are
candidate scheduling tools. Direct admission at arbitrary classes still needs
an admission/repair proof; it does not inherit the original COLA schedule's
worst-case theorem. Declare whether \(b\) measures records, encoded bytes or
another work weight. Those choices do not establish interchangeable bounds.

## 4. Entry accounting is not a byte bound

Let the old head contain one smallest key consisting of \(T\) copies of `a`.
Prepend \(q\) one-record native files with distinct short keys `z` followed
by a counter. Each file is already sorted, and every new key exceeds the long
key. For \(K\ge3\), every new augmented catalog contains two entries and
samples that same long key at ordinal zero from its target.

Each separately front-coded borrowed stream therefore starts with a literal
copy of the \(T\)-unit key. The new indexes emit \(\Theta(qT)\) key units,
although \(S=q\), \(c=1\), and the borrowed-entry bound is just \(B=q\).
The incoming short keys take only \(O(q\log(q+1))\) units. Ordinary prefix
compression within each independent stream cannot remove its first literal.
Neither native LPFC nor fixed-value stride subtraction resolves this example.

Thus the entry recurrence alone proves no bound on actual index bytes or
string reconstruction work. Charge emitted key/framing units and their reads
explicitly. Shared immutable key spans, externally supplied first-key contexts,
or distinguishing separators are possible directions, each requiring its own
codec, query and pin-lifetime proof. **No such fix is selected here.**

## 5. Ingestion state and published state

Pending received files and unfinished index/merge work may exist in an ingestion
state. A published world needs a completed exact-target dependency chain with
the promised depth, or a separately justified bounded fallback query path.
Unbounded independent searches over pending files are not that fallback.

The completion rule must preserve the previous readable root while work is
unfinished, account for both index and native merges, and retain replay/progress
metadata. Immutable `.kv`/`.index` objects and planned SQLite world/pin/progress
metadata supply the intended boundaries; the network admission scheduler and
its byte-budget proof remain implementation work.

## Reference boundary

Bender et al., *Cache-Oblivious Streaming B-trees*, §3:
[original PDF p. 8](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf#page=8)
introduces levels and cascading;
[p. 9](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf#page=9)
gives Lemma 21's level-zero admission and unsafe-level scheduling argument;
[p. 10](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf#page=10)
completes index-linked visibility. The arbitrary-prefix equations and string
counterexample above are Everett-specific analysis, not claims from that paper.
