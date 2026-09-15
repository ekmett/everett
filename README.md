Diet: A Reduced COLA
====================

Diet is a C++20 library for compact string-keyed tables and cheap snapshots.
Keep a large table available to readers, build a small batch of changes, and
combine them when you're ready. Older snapshots keep their data.

I call one logical state a **cola**, and its backing store a **fridge**.
A fridge holds immutable files that we can mmap and share between readers.
Saving a state retains its files; it doesn't copy the table.

Use Diet when you want to:

- Store sorted byte-string or packed-bit keys with compact values.
- Query a saved table without loading it all into memory.
- Keep older states available while building and publishing new ones.
- Prepare disjoint batches independently and combine their changes.

The library is header-only, with an optional SQLite catalog for named saves
and timelines. Version 0.1.0 is experimental: applications currently drive
batch construction and merges, and APIs and file formats may change. The
[detailed usage guide](docs/usage.md) covers that workflow; the
[implementation ledger](docs/implementation.md) records its current limits.

[Quick start](#quick-start) · [Saved tables](#saved-tables) ·
[Building](#building) · [Detailed usage](docs/usage.md)

Building
--------

Use CMake 3.20 or later and a C++20 compiler. With Diet in your source tree:

```cmake
add_subdirectory(path/to/diet)
target_link_libraries(your_target PRIVATE diet::diet)
```

For named saves and timelines, enable the optional catalog and link
`diet::sqlite` instead:

```cmake
set(DIET_ENABLE_SQLITE ON CACHE BOOL "Build the persistent catalog")
add_subdirectory(path/to/diet)
target_link_libraries(your_target PRIVATE diet::sqlite)
```

The catalog requires SQLite 3.51.3 or later. An installed package also supports
`find_package(diet CONFIG REQUIRED)` or
`find_package(diet CONFIG REQUIRED COMPONENTS sqlite)`. See
[building and testing](docs/usage.md#building-and-testing) for installation, tests and
Doxygen commands.

Quick Start
-----------

Start with the byte policy for ordinary strings. Its defaults give us
variable-sized values and **15:1 index sampling**. The bit policy uses the
same sampling and block sizes, with **order-zero exponential-Golomb**
backspaces. There's no need to choose codec parameters to get started.

```cpp
#include <diet/fridge.h>

using bytes = diet::storage_policy<diet::profile_unit::byte>;
using bits = diet::storage_policy<diet::profile_unit::bit>;
using store_type = diet::fridge<bytes>;
```

The fridge's associated types carry that policy for us. Here's a complete
program that builds a table and looks up a value:

```cpp
#include <diet/fridge.h>
#include <array>
#include <memory>

int main() {
  using policy = diet::storage_policy<diet::profile_unit::byte>;
  using store_type = diet::fridge<policy>;
  auto text = [](char const * s) { return diet::bit_string::from_bytes(s); };

  // Input keys are unique and sorted.
  std::array records{
    diet::profile_record{text("alpha"), text("one")},
    diet::profile_record{text("beta"), text("two")},
    diet::profile_record{text("gamma"), text("three")}};
  auto table = std::make_shared<store_type::blob const>(
    store_type::blob::build(records));
  auto saved = store_type::query_root::build(table);

  auto key = text("beta");
  auto query = saved.cursor(key.view());
  while (!query.done()) {
    query.step(1);
    if (query.has_match())
      return query.take_match().value == text("two") ? 0 : 1;
  }
  return 2; // Not found.
}
```

`query_root::build` prepares the table for lookup, including larger tables.
The root retains the data it queries, and `take_match()` returns an owned
value. The same cursor interface works over mapped files.

Despite its name, `bit_string::from_bytes` stores ordinary bytes, including
embedded zeros; a byte policy compares keys in unsigned-byte order. For packed
bits, select `profile_unit::bit` and construct keys and values with
`bit_string::from_bits("101101")`. The query workflow stays the same. Fixed-size
values and other representation choices are covered in
[policy configuration](docs/usage.md#choose-byte-or-bit-units).

Saved Tables
------------

A `fridge<policy>` opens an existing backing directory. The catalog records
which files belong to a named save, and a reader pin keeps that selection
available. Once we've [created a save](docs/usage.md#persistent-tables-and-saves),
we can close the process and read it again:

```cpp
#include <diet/fridge.h>
#include <diet/sqlite_catalog.h>

int main(int argc, char ** argv) {
  if (argc != 2) return 64; // Pass the backing directory.
  using policy = diet::storage_policy<diet::profile_unit::byte>;
  diet::fridge<policy> store(argv[1]);
  auto catalog = diet::sqlite_catalog<policy>::open(store.root());
  auto pin = catalog.acquire_save("read-initial", "initial", "example-reader");
  auto saved = store.open_query(pin.head);

  auto key = diet::bit_string::from_bytes("alpha");
  auto query = saved.cursor(key.view());
  while (!query.done()) {
    query.step(1);
    if (query.has_match())
      return query.take_match().value == diet::bit_string::from_bytes("one") ? 0 : 1;
  }
  return 2;
}
```

Link this program with `diet::sqlite`. The example reuses the same operation
and reader identities when rerun; applications allocate a new operation identity
for new work. Opening reads metadata and maps files; full integrity scans are
an explicit recovery operation.

The snapshot operations live on the catalog:

| Operation | Use it to |
| --- | --- |
| `save(operation, name, head)` | Retain a prepared root under an immutable name. |
| `acquire_save(operation, name, reader)` | Pin a saved root before opening its files. |
| `create_timeline(operation, name, head)` | Start a named sequence of states. |
| `fork_timeline(operation, name, source)` | Start another sequence from an existing generation. |
| `publish_timeline(operation, expected, head)` | Publish a prepared state if the current generation still matches. |

These operations retain existing data without copying it. Building new data
and indexes happens before publication, so readers can continue using the old
root. Current catalog saves, generations and reader pins remain retained;
retirement and automatic reclamation are still being implemented.

For the complete write-and-reopen path, follow the
[usage guide](docs/usage.md#persistent-tables-and-saves). The
[timeline example](docs/streamed-timeline.md) adds streamed updates and
publication; the [catalog guide](docs/sqlite-catalog.md) explains replay and
competing publishers.

Updates and Snapshots
---------------------

We build changes as new sorted data, then merge them with an older table.
The default native merge keeps the newer value for an equal key. A custom
composition function can combine edits instead. Existing snapshots retain
their inputs while the next state is built.

A query across several unmerged tables returns each matching contribution.
The application decides which updates replace or compose with which; query
traversal order alone doesn't establish their age. The
[merge guide](docs/usage.md#incremental-native-merges) shows the default case.

Disjoint batches can be prepared by separate workers. The
[partitioned-update example](docs/usage.md#fork-a-cola-and-apply-disjoint-updates)
uses the in-memory `reference_cola` to show admission, snapshots and matching
content fingerprints when batches arrive in either order. That model is also
useful for checking an application's update rules.

Why a COLA?
-----------

A cache-oblivious lookahead array, or **COLA**, spreads a table across sorted
runs. Small updates start small; merging combines them into larger runs.
Sampling lets a lookup carry its position from one run into the next, so it
only has to search a small window at each step. Shared prefixes keep string
keys compact, and immutable runs make snapshots inexpensive.

Diet keeps data and indexes separate, so we can build a new index while older
readers retain theirs. The [design](docs/design.md) develops the accounting;
the [usage guide's field guide](docs/usage.md#field-guide) explains the storage
layout. [Benchmarks](bench/README.md) and the [Lean model](proof/README.md)
record the measurements and correctness arguments.

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
