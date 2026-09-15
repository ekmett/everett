Space accounting
================

I compare occupied encoded arrays separately from reserved capacity, transient
merge space, and snapshot retention. This is a storage accounting exercise, not a
claim that these structures implement identical query, update, or durability APIs.

Sources and reference models
----------------------------

[COLA, §§3–4](https://people.cs.georgetown.edu/~jfineman/papers/sbtree.pdf):
The theoretical layout samples eighth cells and reserves fourth cells for duplicate
left/right lookahead information; half of each main array is reserved for actual
items. The deamortized variant has main, secondary, and shadow arrays; main arrays
route to both next-level arrays, with eighth/sixteenth sampling. Secondary arrays
reserve half their space for actual records but contain no lookahead information.
The experimental **amortized** layout instead uses 32-byte slots for 8-byte keys
and 8-byte values, including an 8-byte pointer. Its redundant-slot parameter is
$p=0.1$; level $l>0$ has stated size $2(g-1)g^{l-1}$ and
$\lfloor2p(g-1)g^{l-1}\rfloor$ additional redundant elements. This is not a claim
that redundant/native count is always 0.1. Occupancy varies with merges. Its merge
implementation reuses freed space, requiring one additional element. I do not
charge it a second full immutable output.

[COSB-tree, §4, Theorem 8](https://people.csail.mit.edu/bradley/papers/BenderFaKu06.pdf):
The dynamic structure has compressed keydata, per-key hashdata, and a sparse
centroid tree, each held in packed-memory arrays. Keydata's representation bound is
$(1+\varepsilon)F+N$ bits. Hashdata includes a key pointer, an LCP fingerprint,
and a following character. Word counts, alignment, and PMA density constants are
not fixed. The overview samples $\Theta(N/\log N)$ centroid keys; the hashdata
subsection says $\Theta(N/B)$, so I leave the top-layer cost explicit rather than
choose between these statements. There are no published exact byte totals here.

Our measurements
----------------

The harness [space_accounting.cc](space_accounting.cc) measures actual occupied
arrays, including all EF low/high/select-sample/sparse arrays, rank checkpoints,
cut LCPs, false-borrow flags, and the synthetic routing root. It excludes C++
object/vector capacity, mapped-file headers/alignment, filesystem allocation, and
catalog database pages. No files are written by the C++ harness.

Two source snapshots are used:

- Single route: `6474321ddb564634791e66a457dbc35a03667627`.
- Accepted dual route: `ff0588a`, using its complete committed `include/` snapshot.

The single-route fixture has four disjoint sorted native runs of sizes
$n,n/4,n/16,n/64$. The dual-route fixture has four such levels, each with one
main and one secondary of equal size. This exercises both routing layouts but is not a
fully populated binary-growth scheduler snapshot. Each key is an eight-byte
identifier, optionally behind a 64-byte shared prefix. Identifiers are either
sequential or passed through an invertible 64-bit mixing function. Fixed 8-byte
values use both byte and bit policies; 128-byte values use a variable byte policy
(which can recognize their common width). Counts are 4096 and 65536 per largest
run. These are reproducible synthetic distributions, not application traces.

Current block starts encode the retained position directly. There is no separate
predecessor-length checkpoint array to charge. Terminal lengths and other scalar
profile metadata are part of the excluded per-stream descriptors. Native and
borrowed streams both use ordinary FC.

Measured results
----------------

At 65536 records per largest run, fixed eight-byte values:

| Keys | Byte, single route | Byte, dual route | Bit, dual route |
|---|---:|---:|---:|
| Sequential eight-byte IDs | 11.946 | 12.017 | 10.953 |
| Mixed eight-byte IDs | 18.072 | 18.112 | 18.400 |
| 64-byte prefix + sequential IDs | 11.952 | 12.024 | 11.031 |
| 64-byte prefix + mixed IDs | 18.078 | 18.118 | 18.506 |

All figures are **occupied array bytes per native occurrence**, including values.
The dual fixture has 174080 native occurrences versus 87040 in the single fixture;
these columns are separate workload measurements, not a controlled route-only
before/after speed or space test. With eight-byte keys, the dual figures are
43.4% smaller (mixed byte profile) or 62.4% smaller (sequential byte profile) than
the experimental COLA's 32-byte native-slot floor. This comparison does not apply
the eight-byte-key paper layout to our 72-byte-key rows. The paper's redundant
slots increase its occupied cost; allocation slack is still excluded on both sides.

For mixed eight-byte keys in the dual byte fixture, EF costs 0.088 bytes per
native occurrence, rank plus cut LCPs 0.653, and flags 0.009. Cut LCPs dominate
these navigation arrays. The measured borrowed count is 12440: approximately
1/14 of native count. The small increase over 174080/14 comes from ceiling and
root effects, not another full layer of data replication.

The earlier 19.5-byte hand calculation was conservative for this random-key
fixture: sorting gives adjacent random identifiers some shared prefix too.
Measurements also include actual EF sample/sparse overhead and array rounding.
There is no single universal space ratio: values dominate at large widths, and
partitioning a key set across runs changes its compression relative to one fully
merged dictionary.

Raw results: [single route](results/space_accounting_single.csv),
[dual route](results/space_accounting_dual.csv), and
[provenance](results/space_accounting.json).

Dual-route accounting
---------------------

Each main index now has **two borrowed FC streams**, each with EF offsets,
false-borrow flags, rank classes/checkpoints, and cut LCPs. The native stream retains
its own EF directory: three physical EF directories per main pair, one residing
with its native data. Secondary targets terminate at native-only data.

For $G=\lceil M/K\rceil$ shared virtual cuts, with $K=15$:

- Two four-bit rank classes cost about $G$ bytes, plus two 64-bit checkpoints
  per 128 cuts: about $1.125G$ bytes before array rounding.
- Two 64-bit cut LCPs cost exactly $16G$ bytes.
- Combined: about $17.125/15=1.141667$ bytes per **main virtual occurrence**.
- Flags cost one bit per borrowed occurrence, rounded separately per route.
- EF and borrowed FC depend on lengths, common prefixes, and universe sizes.

This doubles rank/LCP cost per main virtual occurrence, not total store size.
Secondary native entries do not each carry those two route directories.

Let $N$ count all native occurrences in one nonsharing rooted routing tree, $I$
all borrowed occurrences, $R$ root virtual size, and $E$ its target edges. Since
each non-root target is sampled once,

$$
 I=\sum_{e}\left\lceil T_e/K\right\rceil
   =(N+I-R)/K+\delta.
$$

For a nonempty edge set, $0\le\delta<E$; with no edges, $\delta=0$.
Thus $I=(N-R+K\delta)/(K-1)$: the large-tree ratio remains approximately
$1/(K-1)$, even with two routes. The finite-size ceilings are measured, not hidden.
This identity assumes no multiple incoming references inside the counted tree;
shared snapshots require counting the union of physical objects instead.

For a different, **per-level capacity** bound, suppose native main and secondary
capacities are at most $B_i$, with $B_{i+1}\le2B_i$. Then

$$
 M_i\le B_i+\lceil M_{i+1}/K\rceil+\lceil B_{i+1}/K\rceil
 \le \frac{K+2}{K-2}B_i+2.
$$

At $K=15$ this is $17B_i/13+2$. It is neither a bytes-per-record factor nor a
live-data amplification ratio. The additive two is stable using the integer bound
$\lceil x/K\rceil\le(x+K-1)/K$: the inherited $2/K$ and
the two rounding terms $2(K-1)/K$ sum to two.

Parameterized comparison
------------------------

[space_accounting.py](space_accounting.py) reports measured Diet array bytes and
explicit scenarios. Its defaults are choices, not inferred paper constants.
For the experimental COLA, with observed redundant/native ratio $r$:

$$
 S_{\rm occupied}=32N(1+r).
$$

With overall occupied-slot fraction $\rho$, capacity is $S_{\rm occupied}/\rho$.
The rigorously comparable native-slot floor is $32N$ for the paper's 8+8 workload.
For different key/value sizes, that exact experimental slot figure does not apply.

For COSB, let $h$ be hashdata words per key, $t$ top-layer bytes per key,
$V$ value bytes, and $F$ the front-compressed key bytes:

$$
 S_{\rm occupied}\le (1+\varepsilon)F+N/8+NV+8hN+tN.
$$

This is a conditional model using a representation upper bound, **not a lower
bound establishing Diet's savings**. The script uses our native key encoding as
an explicitly named FC proxy; it is not the paper's specified bit-exact framing.
PMA occupancies should ideally be separate for the three layers; the script's
single density is a deliberately simplified sensitivity parameter. Test $h=2,3,4$
and densities $0.5,0.75,1$ rather than present one alleged COSB byte count.

Capacity, updates, snapshots
---------------------------

- Occupied encoded arrays: measured here; spare vector capacity excluded.
- Filesystem capacity: add object headers, section alignment, per-file allocation
  rounding, and catalog pages; do not silently call array bytes disk usage.
- Active merges: add all distinct unfinished output extents and retained source
  objects. A three-slot scheduler limits scheduler ownership; it does not bound
  snapshots retaining previous generations. The paper's amortized in-place
  experiment has a different lifetime contract.
- Snapshots: count the union of pinned physical files and index versions. Identical
  shared objects count once; equally sized but separately encoded objects do not.
  Arbitrarily many retained versions have no bound in terms of current live $N$.
- Deletes and repeated updates: native occurrence count is not necessarily live
  key count. Report both before drawing storage conclusions.

Reproduction
------------

Compile C++20 with `-O2 -I include`, then run `space_accounting.cc`. For the dual
build, use `-DDIET_DUAL` and the include snapshot of `ff0588a`. Capture stdout to
CSV. The host resource gate should wrap compilation and execution; no runtime
benchmark is required. Run the Python calculator with `--measurements PATH` and
explicit chosen scenario parameters. Raw array figures do not depend on compiler
speed or processor ISA.
