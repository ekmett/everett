Everett: Persistent Storage
==========================

Everett is a C++20 library for compact string-keyed tables, cheap snapshots and
independent branches. Connect to a named table, read and write strings, and keep
an earlier state whenever you need one.

A **world** is an immutable logical table state. A **multiverse** owns its
backing storage, and a mutable **session** follows the current world as writes
and background merges produce new versions. Older snapshots keep their data.

```cpp
auto storage = everett::multiverse<>::create("data");
auto db = storage.connect("earth-616");
db.put("name", "Everett");
auto saved = db.snapshot();
db.put("name", "Persistent snapshots");
// saved.get("name") still returns "Everett".
```

Include `<everett/connection.h>` and link `everett::sqlite`. The default stores
byte strings with **15:1 index sampling** and byte-counted front coding, which
stores only the changed suffix of each sorted key. Keys and values are
`std::string`; embedded zero bytes work. There are no codec parameters to choose
before getting started.

I use the byte path for ordinary tables. The
[byte table guide](docs/byte-transport.md) explains its framing and measured
space and merge costs. Explicit bit policies remain available for applications
with bit-oriented keys.

Version 0.1.0 is experimental; APIs and file formats may change. The
[connection guide](docs/connection.md) covers the mutable API and its failure
rules. The [implementation ledger](docs/implementation.md) records tested
boundaries, including retention and work accounting.

[Quick start](#quick-start) · [Saved tables](#saved-tables) ·
[Building](#building) · [Detailed usage](docs/usage.md)

Quick Start
-----------

Here is a complete program:

```cpp
#include <everett/connection.h>

int main() {
  auto storage = everett::multiverse<>::create("data");
  auto db = storage.connect("earth-616");
  db.put("name", "Everett");
  auto name = db.get("name");
  return name != "Everett";
}
```

`create` establishes missing directories and flushes their new names. `connect`
opens the latest state of the named session, creating an empty table if needed.
Run the program again and it reopens the same table. An existing multiverse can
also be opened with `everett::multiverse storage("data")`.

`get` returns `std::optional<std::string>`. A missing key and a stored empty
string are distinct. `put` replaces a value; `erase` removes an existing key.
Writes return after their new state is durably published. Readers use immutable
snapshots and mmap the table files. Writes also pay for merges and cleanup of
deleted or overwritten records in the active table.

Saved Tables
------------

A snapshot retains existing files without copying their records. Give one a
name to keep it across process restarts:

```cpp
auto before = db.snapshot();
db.save("before-edit", before);
db.put("name", "Something else");

auto saved = db.load("before-edit");
auto branch = db.fork("earth-617", *saved);
branch.put("name", "Another branch");
```

The two sessions can now evolve independently while the saved snapshot stays fixed.
`load` returns an optional snapshot; it leaves the live session where it is. Save
names are immutable, and a fork needs an unused session name. `save(name)` and
`fork(name)` use the current snapshot.

Snapshots also expose `live_count()` and `signature()`. The signature describes
logical contents independently of the physical merge layout. I use it to
notice disagreement between replicas; it is a sanity check, not a cryptographic
commitment.

Current catalogs retain historical generations and pins, so disk use includes
retained history. Automatic reclamation remains future work.

Ranges
------

Walk the live rows between two keys, including the lower bound and excluding
the upper bound:

```cpp
for (auto row : db.range("a", "b")) {
  // Use row.key and *row.value.
}
db.erase_range("a", "b");
```

The range keeps its snapshot alive and provides C++20 forward iterators. It
also works with range adaptors such as `std::views::take`. Range deletion
publishes one batch of tombstones for the observed rows; earlier snapshots
retain them. The [range guide](docs/typed-scan.md) covers bounds, streaming,
concurrent changes and traversal costs.

Transactions
------------

Use a transaction to publish several edits together:

```cpp
auto tx = db.begin();
tx.put("name", "Everett");
tx.put("version", "one");
auto before = tx.snapshot();
tx.put("version", "two");
tx.commit();
// before.get("version") is still "one".
```

Transaction reads see its own edits. Subsequent reads through `db` see the
committed edits; retained snapshots stay fixed and can branch. The [transaction guide](docs/transactions.md)
covers private flushes, conflicts, abort and recovery.

Concurrent Updates
------------------

Several threads can submit writes through one connection. Its worker applies
them in queue order and keeps merging between updates. Asynchronous writes
return tickets:

```cpp
auto first = db.put_async("left", "L");
auto second = db.put_async("right", "R");
second.get(); // Wait for this write's durable publication.
first.get();  // Check the earlier write's outcome too.
```

The queue has bounded admission capacity and applies backpressure when it fills
or earlier merge work needs service. A ticket keeps its own result even after
the live session advances. Separate connections writing the same session compete through
checked publication; a stale writer fails instead of overwriting another writer.

For work partitioned by key, prepare contributions from a common snapshot:

```cpp
auto base = db.snapshot();
auto left = base.put("partition/a", "updated A");
auto right = base.put("partition/b", "updated B");
db.apply(std::move(right));
db.apply(std::move(left));
```

These disjoint changes can arrive in either order and give the same contents
and signature. Each contribution checks its affected old values. Custom sorts
can compose changes instead of replacing values; see the
[typed update guide](docs/typed-world.md).

Building
--------

Use CMake 3.20 or later and a C++20 compiler. For the named table API:

```cmake
set(EVERETT_ENABLE_SQLITE ON CACHE BOOL "Build the persistent catalog")
add_subdirectory(path/to/everett)
target_link_libraries(your_target PRIVATE everett::sqlite)
```

The catalog requires SQLite 3.51.3 or later. An installed package supports:

```cmake
find_package(everett CONFIG REQUIRED COMPONENTS sqlite)
target_link_libraries(your_target PRIVATE everett::sqlite)
```

Everett is header-only. The lower-level codecs, indexes and in-memory engines use
`everett::everett` and do not require SQLite. The supported persistent file writer
uses POSIX operations on macOS and Linux. See
[building and testing](docs/usage.md#building-and-testing) for installation,
sanitizers and Doxygen commands.

Why a COLA?
-----------

A cache-oblivious lookahead array, or **COLA**, spreads a table across sorted
runs. Small updates start small; merging combines them into larger runs.
Sampling lets a lookup carry its position from one run into the next, so it
only searches a small window at each step. Shared prefixes keep string keys
compact, and immutable runs make snapshots inexpensive.

Everett keeps data and fractional indexes separate. We can build a new index while
older readers retain the exact files their indexes describe. A completed merge
changes the representation without changing the table's contents.

The [design](docs/design.md) develops the accounting. The
[detailed usage guide](docs/usage.md) covers lower-level construction, byte
profiles, explicit bit policies and tuning. [Benchmarks](bench/README.md) and the
[Lean model](proof/README.md) record measurements and correctness arguments.

License
-------

Copyright 2026 Edward Kmett. My code is available under
[BSD-2-Clause](LICENSES/BSD-2-Clause.txt) **OR**
[Apache-2.0](LICENSES/Apache-2.0.txt), at your option. Both complete license texts
are included in source and installed packages; see [LICENSE](./LICENSE).
The CRC generator and its output retain Peter Cawley's **MIT OR zlib** terms;
the [third-party notices](THIRD_PARTY.md) and complete upstream licenses ship
with the package.

Contact Information
-------------------

Contributions, examples, and bug reports are welcome. Please use
[GitHub issues](https://github.com/ekmett/everett/issues) for reproducible problems
or design discussion, or contact me at [ekmett@gmail.com](mailto:ekmett@gmail.com).

-Edward Kmett
