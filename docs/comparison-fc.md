# Front coding with comparison state

I use ordinary front coding in each physical stream, with enough navigation
state to compare keys without reconstructing their inherited prefixes. The
[query API](query.md) connects these comparisons into complete chain traversal.
Each borrowed route carries exact cut LCPs alongside its origin rank and FC
stream. I derive the transfer below for one borrowed route; the
[COLA index](cola-indexes.md) applies it independently to its two routes.

## Entering a projected stream

Let $B$ be the sampled boundary at virtual cut $q$. For either physical stream,
let $A$ be its preceding key and $C$ its first candidate at or after the cut.
Sorted order gives

$$
A\le B\le C.
$$

Suppose the front-coded record for $C$ retains $r$ units from $A$. Every key
between $A$ and $C$ shares those units, so $B$ supplies the same retained prefix.
We need its comparison state against the query, not the prefix bytes themselves.
Native-before-borrowed ties preserve the non-strict inequalities. A missing
predecessor uses the literal first record. An empty projected range has no
candidate to decode.

At this first record, the stored backspace refers to $A$, not to $B$. In
particular, it does not give the exact LCP with $B$ or identify a differing bit.
Subsequent records in that physical stream use their actual predecessor state.

## The preceding borrowed key

There is a second boundary. The borrowed predecessor needed for the next hop
can lie before this window. Calling it $C$ again, we have $C\le B$, so the
forward sandwich does not let us decode it from $B$.

For $K=3$, consider these two possible targets:

```text
a0 a1 a2 | aa ab az | za
b0 b1 b2 | ba bb bz | za
```

Their sampled keys are `[a0, aa, za]` and `[b0, ba, za]`. Merge either with native
keys `[bx, by, zz]`. Query `bz` enters the second virtual group at boundary `by`.
The preceding borrowed key is `aa` or `ba`; its local FC record is the same in
both cases: retain one unit and append `a`. The next borrowed key `za` is literal.
The middle target group's controls and suffixes also agree: retain one unit,
then append `a`, `b`, or `z`. Only the second target contains `bz`.

The missing inherited `a` or `b` occurs earlier. This example establishes that
the incoming boundary's comparison state, these local frames and their lengths
are insufficient. The full files remain distinguishable. In a short fixture,
replaying an earlier record in the same physical block may recover the context;
long shared-prefix runs can put that context arbitrarily far back.

### One exact LCP per cut

We can supply the missing information as a scalar. Store

$$
\ell=\mathrm{lcp}(C,B)
$$

for the preceding borrowed key $C$ at this exact cut. For an actual routed
boundary, $C\le B\le Q$, where $Q$ is the query. Ordered strings satisfy

$$
\mathrm{lcp}(C,Q)=
\min\bigl(\ell,\mathrm{lcp}(B,Q)\bigr).
$$

The identity includes equal keys and proper-prefix endpoints. Since $C\le Q$,
equality holds exactly when this LCP spans all of $Q$. Otherwise $C<Q$. We do
not need $C$'s full length. At the initial cut there is no preceding borrowed
occurrence to repair.

This scalar belongs to the exact fractional-index view. It depends on the
current cut layout and borrowed frontier, and stays pinned with those
dependencies. It is not native `.kv` metadata. If the same borrowed key precedes
several cuts, each cut needs its own exact LCP; a single minimum cannot stand in
for all of them.

The streaming builder has both keys and records the exact LCP at each cut.
Alternatively, the LCP of
sorted endpoints is the minimum adjacent LCP along the intervening sequence,
which suggests maintaining a running minimum since the last borrowed key.
That construction still needs checks for the cut's before/after convention and
native-before-borrowed ties.

False-borrow recovery of a native match before the projected range continues to
use the rank-derived native ordinal. It needs the value at that ordinal, without
reconstructing its key.

## Comparison transfer within a block

Separate content mismatches from string endpoints. Let $d$ be the first content
mismatch between the previous key and the query, or infinity if none exists.
For a record retaining $r$ units, let $e$ be its first literal mismatch against
the corresponding query interval, again allowing infinity. Necessarily $e\ge r$.
The content-mismatch transfer is

$$
T_{r,e}(d)=
\begin{cases}
d & d<r,\\
e & d\ge r.
\end{cases}
$$

For an earlier record $a$ followed by $b$, composition has summary

$$
(r_a,e_a)\mathbin{;} (r_b,e_b)=
\left(\min(r_a,r_b),
\begin{cases}
e_a & e_a<r_b,\\
e_b & e_a\ge r_b.
\end{cases}\right).
$$

The invariant $e\ge r$ makes this composition valid and is preserved by it.
Without that invariant the formula is false. The mismatch's direction travels
with whichever event is selected. Full key and query lengths handle endpoints
separately; an endpoint is not an arbitrary content event below $r$.

Associativity permits an ordered parallel prefix scan. It does not permit
reordering records. A 16-lane scan takes four doubling stages after the literal
comparisons are available.

## Two physical offset directories

For the single-route layout, keep one Elias–Fano directory for native records and
one for borrowed records, together with interleaving rank. A two-route COLA
index adds the second borrowed stream's directory and rank; its native
directory is shared by both projections. Neither layout stores an offset
directory for the virtual merged order.

The cascade stride $K$ and codec block width $W$ serve different purposes.
For the single-route projection at virtual cut $q=Kk$, half-open borrowed rank gives

$$
i=\mathrm{rank}_{\mathrm{borrowed}}(q),\qquad d=q-i.
$$

Seek native block $\lfloor d/W\rfloor$ and borrowed block $\lfloor i/W\rfloor$,
then enter at lanes $d\bmod W$ and $i\bmod W$. These directories replace the
corresponding physical sampling cadence when $W$ changes. The native directory
continues to know nothing about later fractional-index layouts. Fixed-width
values retain their separately calculated offset contribution.

The first record in each block stores an absolute **retained-prefix length**
instead of a backspace. Adding its suffix length gives that key's full length;
the remaining records use relative backspaces. Neither the preceding block nor
its key contents are needed to parse these controls. Controls before the selected
lane can be parsed without reconstructing their keys; the selected lane enters
with the carried comparison state.

A nonempty interval of $L$ records can cross at most
$\lceil(W-1+L)/W\rceil$ blocks. Since each projected interval has $L\le K$,
$W\ge K$ limits it to two. That two-block bound does not hold for arbitrary
independent choices of $W$ and $K$.
Control parsing costs $O(W)$ per touched block. Keeping $W=O(K)$ retains an
$O(K)$ entry/control bound for the whole projected window.

That span counts forward candidates. Repairing the preceding borrowed comparison
uses the cut LCP even when $i$ is the stream's length: it reads no preceding
record. The repaired context leaves its full length unknown unless equality
establishes it. Fetching a false-borrow native value at $d-1$ still requires its
encoded record and can touch an additional preceding block; the comparison
counters exclude that separate probe.

The index currently stores each exact cut LCP in an unsigned 64-bit word.
This allows direct addressing without another Elias–Fano directory and costs
about $64/K$ bits per augmented occurrence, including unused zero entries where
there is no preceding borrowed key. Compressed counts or a narrower validated
key-length limit are possible space tradeoffs.

## SIMD encoding experiment

One candidate bit-profile block separates unary widths, packed integer tails,
and concatenated literal bits. For order-zero exponential-Golomb coding of $x$,

$$
w=\lfloor\log_2(x+1)\rfloor,\qquad x=2^w-1+\mathrm{tail}.
$$

Scalar scans can locate the unary terminators for the block's prefix and
literal-length counts. Prefix sums locate the tails and literals. With initial
absolute retained count $r_0$, suffix lengths $s_j$ and later backspaces $b_j$,

$$
L_0=r_0+s_0,\qquad
L_j=L_0+\sum_{t=1}^{j}(s_t-b_t),\qquad r_j=L_j-s_j.
$$

The initial retained position comes from the block itself; no predecessor length
or surrogate boundary length appears in this calculation.

We can align each literal with its query interval, compare those intervals
independently, then scan the transfer summaries in order. Overflow, bounded
loads, partial lanes and endpoints remain part of correctness. An explicit
32-bit key-position limit may be useful; a 64-bit file offset alone does not
force every in-key position to occupy 64 bits.

The scalar implementation defaults to $W=K$. The
[whole-query comparison](../bench/query_compare.md) favors $W=15$ over $W=16$
with $K=15$ in its measured fixtures. That does not select a transposed control
layout or establish a SIMD speedup. The policy's other Golomb and exponential-Golomb choices also
need an explicit encoding decision before this layout can replace them.

## Verification boundary

The existing fractional-index proof establishes ranks, sampled windows, exact
target retention and false-borrow ordinals over abstract ordered keys. It does
not yet connect these string comparisons to encoded bytes or bit positions.
The [proof guide](../proof/README.md) records the checked mathematical pieces.

The independent tests compare exact cut LCPs, per-candidate states and complete
queries with full-key oracles. They cover reindexing, equality across cuts,
empty projections and the preceding-frontier counterexample. Protected pages
verify that control-only entry skips key payloads and borrowed-frontier repair
does not replay the preceding block. Reindexing preserves native bytes and pins the exact new
index dependencies. Construction reconstructs sequentially. The explicit
[mapped codec scan](mapped-blobs.md) verifies sequential FC framing, valid
prefix retention and ordering, navigation directories, cut LCPs and exact target samples.
`file<P>::scan` supplies the lower-level envelope, CRC and padding check.

`profile_view::reconstruct_at` is a separate full-reconstruction operation.
Under ordinary FC its cost includes the preceding context it traverses. A
standalone profile encoder can opt into locality-preserving restarts for this
operation; blob construction and cascade queries use ordinary FC.
