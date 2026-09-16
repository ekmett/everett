# Updates in a category chosen per key

A replacement tells us the new value of a key.
A patch tells us how to get there. To extend the [Diet design](design.md)
to patches and other composable changes, we need to say when two changes can
compose and what their composition means. Ordinary category theory gives us
just those laws.

The [typed executor](typed-cola.md) dispatches updates and chronological
composition through the selected sort. Replacement, counter and noncommutative
append instances exercise that path; the append instance also survives mapped
publication and reopening. Conditional contributions validate their source
values before applying a batch. The richer dependency and evaluation contracts
below describe what a sort needs when an arrow is a program rather than a small
encoded value. The [implementation ledger](implementation.md) records the
tested boundaries.
The [Lean proof core](../proof/README.md) checks typed composition, disjoint
updates and adjacent-merge adoption in an abstract model.

## 1. A family of categories

Let $\mathcal K$ be the discrete set of full, sort-qualified logical keys.
A key includes its sort identity and its canonical local key, as specified in
[Sorts and key policies](keys.md). Give each $k\in\mathcal K$ a category
$\mathcal C_k$:

- Objects are the admissible configurations of that key.
- An arrow $f:x\to y$ is an allowed change from one configuration to another.
- The identity $1_x$ is the default change at configuration $x$.
- Composition combines compatible changes, retaining their chronological order.

The full key chooses the category; a sort may provide a category resolver rather
than one category shared by every key of that sort. Values at different keys
need not have the same type, update language, or composition algorithm. A counter,
a document edited by splices, and a large structured value edited by patches can share the same
outer store. Describing an arrow's endpoints mathematically does not require
serializing two full configurations with every change.

We choose baseline objects $o_k$ and define a sparse cola as a family
$X=(X_k)_k$, with $X_k=o_k$ outside finitely many keys. An update $f:X\to Y$ is a family
$f_k:X_k\to Y_k$, with identity arrows outside finite support. These form
the **category of finite-support sections**, or restricted product

$$
\mathcal W=\prod_{k\in\mathcal K}^{\mathrm{fin}}(\mathcal C_k,o_k).
$$

Composition and identities are componentwise. For a finite key space this is
the ordinary product of the per-key categories. A baseline may be an absent
binding, or an initial configuration supplied by the cola definition.

Omitting a key from an update means $1_{X_k}$. This differs from deleting the
key. Where supported, deletion is an arrow to a distinguished absent object;
the category determines which such arrows exist. No invertibility is required.

The key-to-category rule and its interpretation version belong in the pinned
cola/schema context. Changing them requires explicit migration or transport.
An old arrow cannot silently acquire a different meaning when loaded by a newer
program. Exact source compatibility also needs its own validation: the weak
cola fingerprint is not a proof that an arrow is applicable.

### Logical key identity and multiplicity

There is still one logical configuration per key. A key may occur in several
history files because it has changed several times. Those occurrences describe
changes to one binding.

Under our key-addressed update rules, simultaneous bindings $(k,a)$ and
$(k,b)$ leave `delete k` or `replace k` underspecified. The operation does
not identify which old value to cancel in the fingerprint or which member's
state to validate. Composing history arrows does not resolve this ambiguity.
Independently addressable members need a selector in their logical key, such as
$(k,\text{member identity})$.

Indistinguishable unit-valued occurrences have only multiplicity as state:
$n_k\in\mathbb N$. Repeating $(k,())$ is effectively a unary encoding
of the unique binding $(k,n_k)$. The count representation stores this directly,
with zero represented by absence. A decrement requires establishing $n_k>0$;
it contributes one weak mutation, but removes a live key only on $1\to0$.
Deleting the whole count binding instead removes all its occurrences; it is a
different operation from decrementing once. We keep the exact natural count
independently of its fingerprint, and distinguish total multiplicity from the
number of occupied keys:

$$
M=\sum_k n_k,\qquad N=\sum_k[n_k>0].
$$

To preserve the fingerprint of unit occurrences, we choose
$\phi_k(n)=n\,h_K(k)h_V(())$, interpreting multiplication by $n$ as
repeated addition in the chosen algebra. A decrement subtracts exactly one
unit contribution. An arbitrary hash of the encoded natural number would be
a different valid fingerprint policy, not automatically this multiset sum.
In characteristic two, the literal unit sum retains only multiplicity parity:
two identical contributions cancel. Hashing the aggregate count and updating
by its endpoint difference avoids this forced parity collapse, while remaining
a weak fingerprint with possible collisions.

## 2. Histories are simplices; merges compose adjacent segments

A per-key history is a composable chain

$$
x_0\xrightarrow{f_1}x_1\xrightarrow{f_2}\cdots
\xrightarrow{f_m}x_m.
$$

This is an $m$-simplex of the nerve $N(\mathcal C_k)$, equivalently a
functor $[m]\to\mathcal C_k$. An inner face composes two adjacent arrows;
a degeneracy inserts an identity. These are the ordinary nerve operations in
[Kerodon, §1.3.1](https://kerodon.net/tag/002M).

A blob represents a consecutive segment of a key's history by its composite,
with **at most one native entry per key per blob**.
Merging coarsens this factorization. Associativity permits any schedule of
adjacent contractions:

$$
h\circ(g\circ f)=(h\circ g)\circ f.
$$

Thus merge order can vary while the order of changes at each key stays fixed.
Composing across an intervening same-key arrow requires including that arrow,
or a separately established commutation/rebase law. Physical file membership
alone does not establish adjacency. Outer nerve faces discard endpoints and
are not the operation that preserves a history's full composite.

A merge must produce one composite for every overlapping key. This makes the
adjacency condition concrete: if an intervening same-key segment lies outside
the selected inputs, the merge is ineligible under this format. Retaining
multiple uncomposable entries for a key would require a different duplicate-key
search and segment-count bound, and would not by itself define
deletion/replacement of ambiguous logical duplicates.

The encoding must respect composition. If $\star$ composes encoded arrows,
we need

$$
\llbracket b\star a\rrbracket
=\llbracket b\rrbracket\circ\llbracket a\rrbracket.
$$

Different parenthesizations may produce different bytes representing the same
arrow. Canonical serialization is an additional property. Shared merge results
therefore retain exact input identities, ordering, category/codec versions and
any normalization context in their cache recipes.

Replacement updates have a stronger, specialized law: a later replacement
determines the resulting value independently of earlier values. General arrows
do not permit newest-record-wins resolution. A merge composes the relevant
changes oldest to newest; it drops earlier effects only when the chosen policy
provides an absorption law. Dropping their physical dependencies is a separate
lifetime check.

## 3. Partition independence is componentwise composition

Now take two workers updating distinct keys, with $f:x\to x'$ and $g:y\to y'$.
Their updates extend by identities to the other coordinates. The square
commutes:

$$
(1_{x'},g)\circ(f,1_y)
=(f,1_{y'})\circ(1_x,g)
=(f,g).
$$

The identity in the second step is at the state produced by the first step.
This is why disjoint changesets computed against the same round base can be
admitted in either order, even when their keys use different categories.
Within each partition, same-key changes retain their causal order.

This describes the store's update law. Application-level cross-key constraints
must also permit the intermediate colas, or define visibility/admission at the completed
round. Reading the same pinned round base still matters when workers calculate
their arrows from other keys' state. The algebra does not make independently
computed overlapping writes commute.

## 4. Fingerprints are exact additive changes of state potential

Choose a common abelian group, such as the additive group of the selected ring
$R$. Give each key a state potential
$\phi_k:\mathrm{Ob}(\mathcal C_k)\to R$, with $\phi_k(o_k)=0$.
Then

$$
C(X)=\sum_k\phi_k(X_k),\qquad
\Delta_k(f:x\to y)=\phi_k(y)-\phi_k(x).
$$

The table specialization is
$\phi_k(v)=h_K(k)h_{V,k}(v)$, with zero potential at absence. Value hashing
can depend on the category/schema at that key, with the sort's hash policy
providing the default. The sort supplies key hashing without hashing its dispatch code, and all policies use
the common additive group. For a nonzero initial baseline,
we subtract its local potential when defining $\phi_k$, or carry a separate
finite initial fingerprint.

We obtain the composition law by telescoping:

$$
\Delta_k(1_x)=0,\qquad
\Delta_k(g\circ f)=\Delta_k(g)+\Delta_k(f).
$$

Summing the finite support gives
$\Delta(f:X\to Y)=C(Y)-C(X)$. Thus $\Delta$ is a functor
$\mathcal W\to B(R,+)$, where $B(R,+)$ is the one-object category with
elements of $R$ as arrows and addition as composition. On the nerve this is
the exact additive 1-cocycle $dC$.

Exactness expresses our choice to fingerprint the endpoint cola. An arbitrary
additive functor can instead assign a nonzero value to a loop and thereby
record history. Our endpoint difference assigns zero to every loop. In
particular, **zero delta does not imply an identity arrow**: a category can have
nontrivial loops and distinct arrows with the same endpoints, even without
hash collisions.

The pin owner's existing algebra now applies unchanged: an initial base
contributes its fingerprint, update entries contribute validated deltas, and
a merged entry contributes their sum. Borrowed keys and index-only dependencies contribute
zero. No division is introduced.

We still need a way to compute the delta efficiently. An arbitrary patch and
the hash of its source do not necessarily determine the target hash cheaply.
A category can provide a maintained summary, inspect the affected state, or
pay for evaluation. The additive law alone supplies no such algorithm.

## 5. Navigation cost and evaluation cost separate

With a maintained level bound and one native entry per key per blob, cascading
finds at most $O(\log N)$ stored segments for a key. It bounds key navigation,
including the string-decoding
qualifications in the main design. A query additionally pays to access and
interpret the arrows:

$$
T_{\rm query}(k,Q)=T_{\rm navigation}(k)
+T_{\rm arrow\ access}+T_{\rm observe}(Q,\text{anchor},\text{segments}).
$$

The observation term includes any composition or normalization performed at
query time. A compact diff may save substantial storage and transmission while
requiring substantial work to apply. Constant-time composition by allocating a
node in an expression DAG defers work and retains dependencies; it does not
bound evaluation. One live key can accumulate a large value or program even
when $N=1$. Repeated doubling can have a small program and exponentially
large materialized output.

Full materialization is not compulsory. A splice policy might answer a range
query by splitting it among unchanged source intervals and inserted literals.
Maintaining a summary through a change likewise needs a policy-specific rule
that agrees with querying the changed state. The costs depend on fragments,
dependencies, and requested output; category laws do not make these operations
constant-time.

The logarithmic claim therefore bounds structural navigation. A uniform
worst-case query/update bound also needs bounds on composing,
validating, hashing and observing arrows. Merge scheduling must charge those
operations in its work units and retain continuations when they are lengthy.

## 6. Blobs, pins, checkpoints and strong deletion

We can apply the same key codec to sorted $(k,\text{arrow})$ entries.
Native keys remain unique within a blob, preserving false-borrow handling.
False-borrow flags and fractional-index contexts concern key navigation.
A decoded key frontier is not a source-state anchor for applying a value diff.
The current fixed nine-byte value/tombstone slot is one concrete payload format,
not a general arrow format.

Variable-length inline arrow bytes enter the variable extent of `elias_fano`.
Only fields with a common fixed width across the entire indexed stream can be
subtracted by stride arithmetic; a different fixed width for each sort is not
one global stride.
Alternatively, a fixed descriptor can reference separately stored arrow data;
its reachable payloads must then be included in the pin closure and space costs.

Snapshots retain the exact factorization and dependency graph they use. A new
composite can become shareable before any dependent adopts it, as in the
existing index-repair protocol. Durable checkpoints additionally retain arrow
interpreter versions, source dependencies and any suspended composition or
evaluation state. Publication cannot release these inputs while the output
still references them.

To support [global rebuilding](rebuild.md), each category needs an explicit
compaction contract. A materialized endpoint can replace history when
it is sufficient for every supported future update and observation and the
chosen semantics permits forgetting that history. Otherwise we preserve the
composite arrow, possibly in a normalized representation. A nonidentity loop
cannot be discarded merely because its endpoint fingerprint is unchanged.

For categories with absent/present states, we account for live entries using
$\chi_k(y)-\chi_k(x)$, where $\chi_k$ tests presence. A true
present-to-absent transition earns deletion credit after source validation;
absent-to-absent changes do not. Categories without such a distinction need a
different explicit size measure.

The replacement rebuild proof bounds active key records relative to live keys.
Extending it requires a clean representation and construction bound for the
chosen arrow policies. Essential diff programs, retained source data and
materialization work cannot be charged away solely by counting live keys.

## 7. Sort contracts and larger arrows

The executable replacement, counter and append sorts provide initial state,
application, chronological composition, presence and hashing through the typed
registry. Their tests check heterogeneous dispatch, disjoint contributions,
noncommuting update order and signatures. The physical sort-owned codec supplies
key and value framing. These interfaces make the laws the sort's responsibility;
they do not establish them for an arbitrary user callback.

For richer arrow representations, I separate five contracts:

1. Stable category selection per key and version, with source validation.
2. Identity and composition, semantic equality laws, and optional normalization.
3. Queries and checkpoint/materialization semantics, including what can be forgotten.
4. Fingerprint and presence deltas, with validation and actual work bounds.
5. Dependency enumeration and resumable encoding/composition/evaluation.

The current executor composes encoded values synchronously. External source
dependencies, suspended evaluation and a compaction rule for general categories
need the additional ownership and continuation contracts above. Replacement
rebuilding supplies that compaction rule for the default table.

For each additional category, acceptance should cover invalid intermediate
states, identity insertion, different parenthesizations, loops with zero endpoint
delta, retained snapshots and checkpoints preserving supported observations.
We compare semantic results even when composed bytes differ, and count work in
examples with growing programs and output so record-count bounds cannot conceal
deferred work.
