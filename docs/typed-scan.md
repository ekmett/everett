Scanning a table
================

`scan(snapshot)` walks the live rows of a typed snapshot in key order:

```cpp
#include <diet/typed_scan.h>

auto rows = diet::scan(db.snapshot());
while (auto row = rows.next()) {
  // row->key is an owned string.
  // row->value is the same state type returned by get, present here.
}
```

The scan captures its snapshot. Writes, background merges and closing the
connection leave that selection intact. Returned rows own their keys and
resolved values. A default string row has an optional string value; tombstones
are omitted, so that optional is engaged. Stored empty strings remain rows.

For a registry with several sorts, use `scan<MySort>(snapshot)`. Equal keys
from different native runs are resolved in chronological order. Replacement
sorts decode only the newest value; general arrow sorts apply their updates
oldest first. Their `present` predicate determines whether to emit a row.

Bounded traversal
-----------------

`next()` advances until a row is ready or the scan ends. A scheduling loop can
instead call `step(record_budget)`, then `has_row()` and `take_row()`. A ready
row holds its position until taken. `step(0)` does nothing; `done()` becomes
true once no more rows remain. A decoding or callback failure poisons that
cursor while its captured snapshot stays available.

This is a full scan over native data, not an indexed range seek. Selecting a
later sort also walks past earlier sorts. We maintain a heap of native cursors,
one reconstructed key per run, and one resolved row; we do not build an
in-memory copy of the whole table. Construction decodes each run's first key.
Each step unit consumes one physical record, including obsolete versions and
tombstones. String bytes, heap comparisons and sort callbacks have their own
costs. `consumed()` reports the physical records traversed.

The default codec cursor reuses its current key buffer and borrows value bytes
from the pinned source. Full scans therefore materialize keys that point
queries can compare through retained-prefix context. I keep those two paths
separate: a caller requesting all keys actually needs their contents.
