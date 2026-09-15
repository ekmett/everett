# Querying an immutable catalog chain

The query layer connects the encoded window operations into a complete search.
It returns every matching native entry, together with its exact source pair and
native ordinal. The caller decides how those entries compose. An index link
describes navigation, so its position alone does not establish an update's age.

`query_root<P>`, `query_root_builder<P>` and `query_cursor<P>` share the same
storage policy as their blobs. They are also available through the corresponding
aliases in `multiverse<P>`. The [README example](../README.md#query-the-whole-chain)
builds a chain, prepares its root and retrieves two entries for the same key.

## Preparing the first window

A bounded window search needs its group and an incoming key context. At the
start of a chain, neither is known. If the head contains at most `P::group_size`
augmented occurrences, group zero covers it and an empty context suffices:
the first physical record in each stream is literal.

For a larger head, I build empty-native routing catalogs above it. Each contains
every `K`th occurrence of its exact target's augmented catalog. Repeating that
sampling gives a small head without modifying the existing native or index
bytes. The prefix uses the same streaming `index_pipeline` as other index work.
It does not store a separate array of full boundary keys.

Let $A$ be the original head's augmented size and $K=P::group_size$. The added
catalog sizes follow

$$
a_0=A,\qquad a_{i+1}=\left\lceil\frac{a_i}{K}\right\rceil.
$$

We stop as soon as $a_i\le K$. There are $O(\log_K A)$ added catalogs, containing
at most $A/(K-1)+O(\log_K A)$ occurrences in total. This counts occurrences;
sampling long strings does not promise the same fraction of their encoded bytes.
The [sampling analysis](sampling.md) describes the distinction.

`query_root_builder<P>(head)` validates the chain's navigation metadata and
sets up this prefix. A head already within the bound keeps its exact shared
pointer and needs no sampler. An explicit empty blob is a valid empty root;
a null input is rejected.

Preparation can pause:

```cpp
query_root_builder<policy> prepare(head);
while (!prepare.done()) prepare.step(64);
auto root = prepare.finish();
```

The constructor's validation is proportional to the existing chain depth.
For a larger head, the pipeline samples that head once, then sends diminishing
streams through the added stages. Its initialization may decode the current
source records. `step(quanta)` inherits the pipeline's entry-work accounting,
and `finish()` separately builds the rank/select directories. None of these
entry counts is a byte budget, an allocation bound or a time deadline.
The current pipeline may inspect every added stage while choosing the next
quantum, so the entry-work count is not a CPU-instruction bound either.

`finish()` requires completion and returns the same prepared head on repeated
calls. `query_root<P>::build(head)` is the eager convenience. Retain and reuse
the result: preparation is indexing work, rather than a hidden cost of each
query. This does not make attaching a tiny batch to a large head cheap; see
[network admission](network-admission.md).

## Following the chain

`root.cursor(query)` copies the query key and initially pins the root. Each
successful `step(catalog_budget)` visits at most that many catalogs, stopping
when it has a native match or no remaining route. The default budget is one.
The cursor uses `search_window` at each catalog, carrying the sampled predecessor's
group and query-bound comparison context into the exact target. This context
stores exact common-prefix agreement in bits, full key length in policy units,
and comparison direction. It shares the owned immutable query. No inherited
key prefix is reconstructed during traversal.

The full key length remains distinct from prefix agreement. A short query may
agree with only part of a much longer boundary; endpoints and backspace parsing
still need the actual lengths.

There are three states to observe:

| State | Behavior |
| --- | --- |
| Active, no pending match | `step(budget)` visits catalogs and returns how many it visited. A zero budget does no work. |
| Pending match | `has_match()` is true. Further steps return zero until `take_match()` consumes the result. |
| Finished | `done()` is true only after the route ends and the final pending match is taken. Further steps return zero. |

`take_match()` returns a `query_match<P>` containing an owned value, native
ordinal and shared pointer to its exact source pair. Calling it without a
pending match is an error. A search/decoding exception marks the cursor failed;
it cannot be resumed as though the failing operation succeeded.

The cursor pins the unvisited suffix as it progresses. It can release an already
visited prefix when nothing else retains it. Pending and returned matches keep
their own source pairs alive, and each pair retains its exact target dependency.
The input key, original root handle and original construction handles may be
released without invalidating the traversal. Copying a cursor shares the immutable
query and copies its comparison state and pending value into an independent
traversal at the same point.

Native equality does not terminate the search. A matching borrowed occurrence
still carries routing information, and its false-borrow flag can recover a native
match before the projected window. Equal borrowed occurrences remain distinct
when they cross group boundaries.

No borrowed predecessor means the query precedes the target's minimum, because
the first sample is its first augmented occurrence. Descent can therefore stop.
An empty prepared root is finished without attempting to search group zero.

## Work and ownership bounds

For an existing chain of depth $D$, a query visits at most
$D+O(\log_K A)$ catalogs. At each, the projected native and borrowed ranges
share one budget of at most $K$ occurrences. Recovering the preceding borrowed
context and a false native match adds bounded access work. Sparse-offset seeks
and group-header scans retain their existing bounds.

With physical block width $W$, this gives $O(K+W)$ entry/header work per visited
catalog, plus literal comparison, query copying and returned-value copying.
With fixed policy widths, it
is linear in visited depth for fixed-size keys and values. There is no independent
binary search in each data file, and traversal does not call backward full-key
reconstruction.

The [comparison-state design](comparison-fc.md) describes ordinary FC and the
exact cut-LCP scalar needed for an
outgoing borrowed predecessor before the window.

The bound does not establish $D=O(\log N)$ for arbitrary user-built chains.
That is the redundant-level scheduler's responsibility. It also does not cover
disk faults, durable publication or the cost of applying arbitrary categorical
arrows. This layer operates on the current in-memory encoded pairs.

## Construction and trust

The root builder checks the chain once for null input, cycles, size mismatches,
target/sample cardinality and the cut-LCP array's extent. Nonempty borrowed
streams require an exact target. Native-only leaves and links to empty targets
are supported.

These are navigation-shape checks. They do not compare every sampled key with
the target and cannot certify arbitrary equal-count samples manually supplied
to `index_builder`. The content precondition remains the builder's existing one:
samples must come from that exact target's trusted sampler. `index_pipeline`
supplies them directly. Untrusted serialized data needs its separate validation
before it can enter this API.

All retained blobs must remain immutable through every alias. A `shared_ptr`
to const supplies read-only access through that handle; it cannot stop a caller
from changing the same object through some other mutable handle.

This query root is a physical navigation owner. It does not publish a durable
world, resolve replacements, infer chronology or evaluate arrows. Keeping those
choices with the caller lets the same query machinery retrieve fragments for
different per-key categories.

The [complete-query benchmark](../bench/query_compare.md) records preparation and
query timings, visited catalogs and returned matches with an independent
integer-key oracle. Its source, raw trials and reproduction commands are bundled
with the documentation.
