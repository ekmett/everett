Scanning a table
================

`range(snapshot, lo, hi)` walks the live rows in the half-open interval
`[lo, hi)`. `scan(snapshot)` walks every live row. The connection exposes
`db.range(lo, hi)` for its current snapshot:

```cpp
#include <everett/typed_scan.h>

auto rows = db.range(std::string("users/"), std::string("users0"));
while (auto row = rows.next()) {
  // row->key is an owned string.
  // row->value is the same state type returned by get, present here.
}
```

The scan captures its snapshot. Writes, background merges and closing the
connection leave that selection intact. Returned rows own their keys and
resolved values. A default string row has an optional string value; tombstones
are omitted, so that optional is engaged. Stored empty strings remain rows.

Both bounds are optional. An omitted lower bound starts at the first key of
the sort; an omitted upper bound continues to its last key. Equal bounds yield
an empty range, and reversed bounds throw. Bounds use the sort's encoded
ordering, including binary strings and proper prefixes.

For a registry with several sorts, use `range<MySort>(snapshot, lo, hi)` or
`scan<MySort>(snapshot)`. Equal keys
from different native runs are resolved in chronological order. Replacement
sorts decode only the newest value; general arrow sorts apply their updates
oldest first. Their `present` predicate determines whether to emit a row.

C++ ranges
----------

The result models a C++20 forward range. Iterator copies advance independently,
keep their captured snapshot alive, and support ordinary range algorithms:

```cpp
for (auto row : db.range(std::string("a"), std::string("b"))) {
  // Work with this owned row.
}
auto first_three = db.range() | std::views::take(3);
```

Include `<ranges>` for the view adapters. Iterators retain their snapshot
independently, so this is also a borrowed range: algorithms such as
`std::ranges::find(db.range(), key, projection)` return a usable iterator even
after the temporary range is destroyed. Dereferencing returns an **owning row
by value**, so it copies the key and resolved value. This is a C++20 forward
iterator, whose reference type may be a value; its legacy iterator category is
input. It makes no promise of a stable address for a row inside the cursor.
Use `next()` or `take_row()` to move rows out when those copies matter.

Iterator copies initially share their walk. Advancing a shared copy clones the
per-run cursors and their reconstructed keys; it does not restart the scan or
retain every previously returned row. Advancing an unshared iterator reuses its
buffers. `begin()` starts an independent walk from the result's current cursor
position; it leaves the budgeted cursor unchanged.

Deleting a range
----------------

`db.erase_range(lo, hi)` publishes one atomic batch of exact per-key tombstones.
`db.erase_range_async(lo, hi)` returns the ordinary publication ticket. Selection
and observation capture happen on the calling thread; the ticket covers queued
validation, merging and durable publication.

```cpp
auto before = db.snapshot();
db.erase_range(std::string("users/"), std::string("users0"));
// before still contains those rows.
```

For a contribution from a retained snapshot, use
`everett::erase_range(snapshot, lo, hi)` and pass the result to `db.apply`.
A range cursor also has `erase_remaining()` to delete its not-yet-returned rows.
One atomic contribution must fit the connection's configured work and byte
limits. The default work limit quotes 1024 records; larger atomic ranges need
appropriate [connection options](connection.md). Range deletion does not
silently publish smaller batches to bypass those limits.

These operations require a replacement sort. General composable arrows can be
queried by range, but need a sort-specific deletion operation.

We sweep the selected native runs in order and retain the observed old values.
When the exact native layout is unchanged, those observations need no further
search. Otherwise admission validates them with a second advancing native
frontier. It does not perform a point lookup for each returned row, and it does not trust signatures
as evidence that old values match. Changed or missing observed keys reject the
whole batch before mutation. Disjoint changes can proceed; a concurrently added
key, even inside the interval, survives because it was never observed. This is
an exact deletion of the selected rows, not a persistent range predicate.

Each cursor already holds the winning record's decoded FC frame. Its retained
prefix supplies the tombstone's conservative depth without another random seek.
The current sweep checks that depth again when the contribution comes from an
equivalent physical layout. Ordinary carries preserve those bounds. A
clean-generation handoff during rebuilding admission can remove the target's physical predecessor; subsequent
tombstones then carry the whole key rather than issuing point lookups to repair
their caps. Clean rebuild
and FIFO replay use their already-validated old/new states for accounting.

The selection stores one key, tombstone and borrowed old-value view per selected
row until publication. Its base snapshot pins the bytes referenced by those
views; queue byte reservations include the view metadata without duplicating
old value payloads. Snapshots and saved roots continue pinning their exact inputs. Live counts and signatures
change only with successful publication.

Bounded traversal
-----------------

`next()` advances until a row is ready or the scan ends. A scheduling loop can
instead call `step(record_budget)`, then `has_row()` and `take_row()`. A ready
row holds its position until taken. `step(0)` does nothing; `done()` becomes
true once no more rows remain. A decoding or callback failure poisons that
cursor while its captured snapshot stays available. If `erase_remaining()`
fails during allocation, decoding or a sort callback, its unpublished
contribution is discarded and the cursor is poisoned too. Restart selection
from the retained snapshot; retrying the partly consumed cursor is rejected.

Range initialization follows the fractional cascade once to find the first
native ordinal at or above the lower bound in every run. Without an explicit
lower bound, the sort's prefix is the query, so earlier sorts are skipped. We
keep those native cursors open for the rest of the range; crossing subsequent
codec blocks advances their framing and block offsets rather than starting
another point query. Traversal stops at the upper bound.

This also gives us the first key without decoding its preceding native keys.
Let $p$ precede the selected native key $x$ and let $q$ be the bound. Since
$p < q \leq x$, every prefix shared by $p$ and $x$ is also shared by $q$ and
$x$. The stored retained prefix is no longer than that shared prefix. We seed
the cursor with those bits from $q$ and append the frame's literal suffix.
The first native key retains no prefix; a bound past the last key creates an
already-finished cursor without reading a frame. Conservative backtracking, proper-prefix queries and sort-code transitions
obey the same argument. An equal false borrow can put the native match before
the projected window, so we recover its ordinal before seeding. A missing
borrowed predecessor starts the target at ordinal zero: its keys are all above
the query, and still belong in the range.

For $L$ live catalogs, sampling width $K$ and codec block size $W$, positioning
visits at most $K$ comparison headers per catalog, replays fewer than $W$ framing
headers per projected stream, and at most $W$ per native cursor seed. Key-byte
comparisons, selector dispatch and the first reconstructed key have their own
costs. Passing a `range_positioning_work*` as the fourth argument to
`range(snapshot, lo, hi, &work)` records catalog and comparison-header counts,
compared bits, and the framing-header bound for native seeding. Ordinary ranges
do not count this instrumentation. `consumed()` counts physical records
traversed after positioning.

We maintain a heap of native cursors, one reconstructed key per run and one
resolved row. Each step unit consumes one physical record, including obsolete
versions and tombstones. Heap comparisons and sort callbacks are additional
costs. A changed-layout deletion validation positions its advancing frontier
at the first selected key too. Selection runs on the caller; validation runs
before mutation on the publication worker. These query costs are outside the
mutation-count structural merge charge and queue's contribution bound.

The default codec cursor reuses its current key buffer and borrows value bytes
from the pinned source. Full scans therefore materialize keys that point
queries can compare through retained-prefix context. I keep those two paths
separate: a caller requesting all keys actually needs their contents.
