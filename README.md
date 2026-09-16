Diet: A Reduced COLA
====================

Diet is a C++20 library for compact string-keyed tables, cheap snapshots and
independent branches. Connect to a named table, read and write strings, and keep
an earlier state whenever you need one.

I call a logical state a **cola**, its backing store a **fridge**, and a mutable
connection a **tap**. A tap follows your latest cola as writes and background
merges produce new versions. Older snapshots keep their data.

```cpp
auto pantry = diet::fridge<>::create("data");
auto db = pantry.connect("earth-616");
db.put("name", "Diet");
auto saved = db.snapshot();
db.put("name", "A reduced COLA");
// saved.get("name") still returns "Diet".
```

Include `<diet/connection.h>` and link `diet::sqlite`. The default uses compact
bit encoding, **15:1 index sampling** and **order-zero exponential-Golomb**
backspaces. Ordinary keys and values are `std::string`; embedded zero bytes work.
There are no codec parameters to choose before getting started.

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
#include <diet/connection.h>

int main() {
  auto pantry = diet::fridge<>::create("data");
  auto db = pantry.connect("earth-616");
  db.put("name", "Diet");
  auto name = db.get("name");
  return name != "Diet";
}
```

`create` establishes missing directories and flushes their new names. `connect`
opens the latest state of the named tap, creating an empty table if needed.
Run the program again and it reopens the same table. An existing fridge can
also be opened with `diet::fridge pantry("data")`.

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

The original tap, saved snapshot and new branch can now evolve independently.
`load` returns an optional snapshot; it leaves the live tap where it is. Save
names are immutable, and a fork needs an unused tap name. `save(name)` and
`fork(name)` use the current snapshot.

Snapshots also expose `live_count()` and `signature()`. The signature describes
logical contents independently of the physical merge layout. I use it to
notice disagreement between replicas; it is a sanity check, not a cryptographic
commitment.

Current catalogs retain historical generations and pins. Automatic reclamation
is still being implemented, so disk use includes retained history.

Concurrent Updates
------------------

Several threads can submit writes through one connection. Its worker applies
them in queue order and keeps merging between updates. Asynchronous writes
return tickets:

```cpp
auto first = db.put_async("left", "L");
auto second = db.put_async("right", "R");
second.get(); // Wait for this write's durable publication.
```

The queue has bounded admission capacity and applies backpressure when it fills
or earlier merge work needs service. A ticket keeps its own result even after
the live tap advances. Separate connections writing the same tap compete through
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
[typed update guide](docs/typed-cola.md).

Building
--------

Use CMake 3.20 or later and a C++20 compiler. For the named table API:

```cmake
set(DIET_ENABLE_SQLITE ON CACHE BOOL "Build the persistent catalog")
add_subdirectory(path/to/diet)
target_link_libraries(your_target PRIVATE diet::sqlite)
```

The catalog requires SQLite 3.51.3 or later. An installed package supports:

```cmake
find_package(diet CONFIG REQUIRED COMPONENTS sqlite)
target_link_libraries(your_target PRIVATE diet::sqlite)
```

Diet is header-only. The lower-level codecs, indexes and in-memory engines use
`diet::diet` and do not require SQLite. The supported persistent file writer
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

Diet keeps data and fractional indexes separate. We can build a new index while
older readers retain the exact files their indexes describe. A completed merge
changes the representation without changing the table's contents.

The [design](docs/design.md) develops the accounting. The
[detailed usage guide](docs/usage.md) covers lower-level construction, packed
bits, byte profiles and policy choices. [Benchmarks](bench/README.md) and the
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
[GitHub issues](https://github.com/ekmett/diet/issues) for reproducible problems
or design discussion, or contact me at [ekmett@gmail.com](mailto:ekmett@gmail.com).

-Edward Kmett
