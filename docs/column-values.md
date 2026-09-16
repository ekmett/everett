Column Values and Range Aggregates
=================================

This is a future design direction. I want byte-oriented key navigation with
sort-owned value columns, including bit-sliced integer columns. The choice of
key framing and the physical layout of a value column are independent. A
column can use packed bits while key comparisons, front coding and offsets
continue to count bytes.

The current store has typed values and resolved range iterators. It does not
yet expose column storage, rank-based range aggregates or secondary indexes.
The ordinary byte path and its shader implementation take priority; these
extensions should reuse their ordering, merge and snapshot machinery.

Bit Planes in Native Order
--------------------------

For a nonnegative integer column, store one bitmap $B_b$ for each bit position
$b$. Its entry at native ordinal $i$ is bit $b$ of that row's value. If a key
range maps to native ordinals $[l,r)$, then

$$
\mathrm{sum}(l,r)=\sum_{b=0}^{w-1}2^b
  \left(\mathrm{rank}_{B_b}(r)-\mathrm{rank}_{B_b}(l)\right).
$$

Here rank counts ones strictly before its argument. With constant-time rank,
we need two queries per plane: $O(w)$ work for a $w$-bit value, or $O(\log U)$
for values below $U$. This bound does not depend on the number of rows inside
the range. It is not $O(\log w)$. The aggregate accumulator must represent the
chosen sum domain; it need not have the width of one stored value.

We use the fractional cascade to locate both range endpoints in each native
run. For $L$ contributing runs, the total is endpoint-routing work plus
$O(wL)$ rank queries. These queries need rank at arbitrary native ordinals;
the existing fractional index's sampled rank structure has a different job.
I would keep its representation unchanged.

Each column must specify its ordinal domain. Dense planes can use every native
ordinal. A column packed only for a particular sort or for present values also
needs a way to translate native ordinals into column ordinals. Sort runs can
use their contiguous native interval; optional membership may need a separate
bitmap and rank. A zero numeric value remains distinguishable from an absent
binding or a null column.

Signed Bookkeeping
------------------

Replacement values cannot simply be summed across files: that would count old
versions repeatedly. For an additive aggregate, admission records the change
in the quantity we are measuring. Insertion contributes $+v$, replacement
contributes $-v+v'$, and deletion contributes $-v$. The existing required lookup
supplies and validates the old value.

For example, a base value of 10 followed by replacement with 13 and deletion
contributes $10+3-13=0$. The corresponding live-row count contributes
$1+0-1=0$. Keeping count separately handles deletion of a row whose value is
zero. This is the same accounting principle as composing updates: merge
combines contributions without changing their sum.

One physical representation uses positive and negative bit planes:

$$
\mathrm{sum}(l,r)=\sum_b 2^b
 \left(\Delta\mathrm{rank}_{B_b^+}(l,r)
       -\Delta\mathrm{rank}_{B_b^-}(l,r)\right).
$$

Another uses signed two's-complement planes, assigning the sign plane weight
$-2^{w-1}$. The sort must choose the arithmetic domain and the representation
of composed changes. Widening, modular arithmetic and separate positive and
negative contributions have different storage contracts.

A negative contribution must survive until its positive history has been
absorbed. Rebuilding can emit a new live base and retire the accounted history
for that world. Snapshots retain their own exact files. Dropping a negative
entry merely because a two-file merge sees it would break the sum when an
older positive entry remains elsewhere.

The table fingerprint remains a separate summary. Its update is the validated
change in the sort's hash contribution. A numeric sum of values does not in
general determine their hashes or the table fingerprint.

Other Operations
----------------

The useful abstraction is a projection into an additive summary domain. For a
projection $f$, a replacement contributes $f(v')-f(v)$; deletion contributes
$-f(v)$. We can carry several such summaries together.

| Aggregate | Information to retain |
| --- | --- |
| Count | Signed changes in row or non-null membership |
| Sum | Signed value contributions |
| Mean | Sum and count; divide after combining runs |
| Variance | Sum, sum of squares and count in the chosen arithmetic domain |
| Weighted sum | Changes in the projected product, with validated old operands |
| XOR | Per-plane parity; each contribution is its own inverse |

For sum of squares, the update is $v'^2-v^2$, not $(v'-v)^2$. In general an
aggregate projection need not preserve the physical representation of a diff.
Admission must have enough information to construct its projected change.

In the categorical presentation, an update $a:x\to y$ contributes
$\delta_f(a)=f(y)-f(x)$. For composable updates,
$\delta_f(b\circ a)=\delta_f(a)+\delta_f(b)$. The intermediate state's two
contributions cancel, which is why the aggregate survives regrouping during
merges. We choose the absent state's projection to be zero.

Min and max are different: removing the current extreme does not reveal its
successor. They need retained candidates, value-ordered indexes, hierarchical
summaries with repair, or a resolved scan. Quantiles and distinct counts also
need their own structures or explicit approximation contracts. Rank on each
value plane alone does not give all of these operations the sum bound.

Construction and Merging
-------------------------

A merge already establishes output order and groups equal keys. Each column
must compose that group's contributions according to its own law before
discarding superseded physical records: a base 10 and a later delta 3 must
produce 13, even if a replacement-valued column selects only its newest entry.
The shared output key order then determines column ordinals. Columns can copy,
compose or transpose their resulting data independently, followed by rank
construction. This gives SIMD and GPU work without requiring bit-level key
decoding. Variable columns can keep their own offset directories and codecs.

All columns and their navigation metadata belong to one immutable file version
or one explicitly pinned object graph. Publication must expose the matching
keys, columns and summaries together. An interrupted merge may leave private
outputs, but cannot publish a key permutation with columns from another one.

Secondary Indexes
-----------------

A secondary index can order a derived value followed by the primary key:
`(index sort, derived value, primary key)`. Framing keeps that composite
unambiguous, and the primary key distinguishes rows sharing the same derived
value. A replacement removes the old derived entry and inserts the new one.

Base changes and derived changes must publish as one logical transaction.
Snapshots pin a consistent version of both. Disjoint primary keys continue to
produce disjoint composite secondary keys even when their derived values agree.
A shared posting-list representation instead requires an explicit composition
law for concurrent membership changes.

Schema changes and backfilling an index need an initial snapshot, a changes
stream and an atomic handover to the completed index. Uniqueness constraints
add cross-key validation; ordinary disjoint primary-key ownership alone does
not establish them.

Related Work
------------

Patrick O'Neil and Dallan Quass describe projection and bit-sliced indexes for
aggregation and predicate evaluation in
[Improved Query Performance with Variant Indexes](https://courses.cs.duke.edu/spring03/cps216/papers/oneil-quass-1997.pdf)
(SIGMOD 1997). Denis Rinfret, Patrick O'Neil and Elizabeth O'Neil develop
addition, subtraction and rowwise minimum over bit-sliced columns in
[Bit-Sliced Index Arithmetic](https://www.cs.umb.edu/~poneil/SIGBSTMH.pdf)
(SIGMOD 2001). Rowwise minimum of two represented columns differs from finding
the minimum live value of a versioned key range.

The rank-at-endpoints formula and the signed, immutable-run bookkeeping above
are the proposed application to this store. See [typed updates](typed-world.md),
[categorical changes](arrows.md), [range cursors](typed-scan.md) and
[rebuilding](rebuild.md) for the existing contracts they would extend.
