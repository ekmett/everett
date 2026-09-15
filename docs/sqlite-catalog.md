# Persistent SQLite roots and reservations

I keep small mutable coordination metadata in SQLite and the encoded table in
immutable `.kv` and `.index` files. The optional `sqlite_catalog<P>` implements
an insert-only part of that boundary: I can reserve output identities, record
successful seal receipts, register an exact prepared chain, name an immutable
save, publish a named timeline generation, close the process, and reopen either
root for mmap-backed queries.

The component deliberately has no reclamation operation. Reservations, reader
pins, saved roots and every timeline generation remain durable until an explicit
retirement protocol is implemented. This gives us useful persistent reads without guessing which old
owners have stopped using an external file.

## Build and use

The normal `everett::everett` target remains independent of SQLite. I enable the
adapter explicitly:

```sh
cmake -S . -B build -DEVERETT_ENABLE_SQLITE=ON -DEVERETT_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Both headers and runtime must be SQLite 3.51.3 or later. This includes the
upstream WAL-reset race fix. `runtime_version()` and `source_id()` report the
library actually loaded; a newer header alone does not satisfy the runtime
check. [SQLite WAL fix](https://sqlite.org/wal.html#the_wal_reset_bug).

If several SQLite installations are present, CMake accepts explicit
`SQLite3_INCLUDE_DIR` and `SQLite3_LIBRARY` selections. An installed consumer
requests the separate component:

```cmake
find_package(everett CONFIG REQUIRED COMPONENTS sqlite)
target_link_libraries(my_program PRIVATE everett::sqlite)
```

A consumer that only requests `everett` neither finds nor links SQLite, even
when both components were installed. The adapter is header-only but calls the
linked SQLite C library. It requires a thread-safe build and a serialized
connection; a process-wide single-thread configuration is rejected. All WAL
participants use one host and a local filesystem honoring the selected barriers.
The current durable catalog-creation path uses POSIX exclusive creation and a directory barrier;
unsupported creation platforms report `operation_not_supported`.

## A complete publication path

Here is the small-head case. Larger tables first use `query_root<P>::build` and
persist every pair in its prepared chain, including the empty-native routing
prefix. The mapped reader adopts that already prepared graph.

```cpp
#include <everett/sqlite_catalog.h>
#include <array>

using P = everett::storage_policy<everett::profile_unit::byte>;
using catalog = everett::sqlite_catalog<P>;

void make_save(std::filesystem::path const & directory) {
  // The existing directory and its ancestors are trusted and durable.
  auto metadata = catalog::create(directory,
    everett::object_id("00000000000000000000000000000001"));
  everett::blob_identity ids{
    everett::object_id("00000000000000000000000000000002"),
    everett::object_id("00000000000000000000000000000003")};
  everett::object_attempt_id attempt("00000000000000000000000000000004");
  std::array outputs{
    everett::catalog_object_reservation{ids.native, everett::file_kind::native_blob},
    everett::catalog_object_reservation{ids.index, everett::file_kind::fractional_index}};
  metadata.reserve("reserve-table", attempt, "table-builder", {}, outputs);

  std::array records{everett::profile_record{
    everett::bit_string::from_bytes("alpha"),
    everett::bit_string::from_bytes("first")}};
  auto pair = everett::profile_blob<P>::build(records);
  auto native = everett::encode_native_sections(pair.native());
  auto index = everett::encode_index_sections(pair, ids.native);
  metadata.record_sealed("seal-native", native.seal(directory, ids.native, attempt));
  metadata.record_sealed("seal-index", index.seal(directory, ids.index, attempt));
  auto query = everett::open_mapped_query<P>(directory, ids);
  metadata.register_chain("register-table", query, everett::catalog_admission::scan);
  metadata.save("save-table", "initial", ids);
}

void read_save(std::filesystem::path const & directory) {
  auto metadata = catalog::open(directory);
  auto pin = metadata.acquire_save("read-initial", "initial", "reader-1");
  // The committed reader pin precedes opening any external objects.
  auto query = everett::open_mapped_query<P>(directory, pin.head);
  auto key = everett::bit_string::from_bytes("alpha");
  auto cursor = query.cursor(key.view());
  while (!cursor.done()) {
    cursor.step(1);
    if (cursor.has_match()) {
      auto match = cursor.take_match();
      // Consume match.value; match.source pins the exact pair that supplied it.
      (void)match;
    }
  }
}
```

An application allocates fresh opaque object, attempt and operation identities.
Neither the weak table fingerprint nor CRC32C is an identity allocator. The
catalog prevents successful identities from being reused for different work.
A failed reservation transaction inserted nothing and permits a new attempt;
a committed reservation survives even if its writer never finishes.

`reserve` atomically retains its registered input pairs and fresh output
objects under an attempt owner. Call it **before** creating output files. A
writer receipt records successful OS barriers; `record_sealed` checks that the
object, attempt, canonical path, physical extent and header CRC declaration
agree with that reservation. It reads only the envelope and does not recalculate
the body CRC. The receipt remains a caller-supplied attestation of successful
barriers, not a cryptographic proof or a way to repair a failed sync.

`register_chain` reopens the canonical catalog paths named by the supplied
head, retaining those mappings for admission. It validates the complete exact
native/index target chain and sealed object records, and inserts targets before
dependents. An index identity cannot be registered with different native or
target identities. A save requires a registered head with at most $K$ augmented
entries; registration does not build a routing prefix or silently scan a large
head to prepare it.

Normal `catalog_admission::trusted` checks headers, fixed directories and graph
metadata. It trusts immutable key contents and the exact-sampling contract.
`catalog_admission::scan` additionally invokes the complete mapped chain scan
before acquiring SQLite's write lock: CRCs, ordinary front coding, directory
semantics, false-borrow flags, cut LCPs and every sampled target occurrence.
Neither admission mode makes changes to the original native or index files.

## Named timelines and durable generations

I represent a mutable timeline as an append-only sequence of immutable root
selections. A generation contains the timeline's binary name, its generation
number, the exact native/index head identity, and a permanent owner ID. The
highest generation is current. Publishing a new generation never updates or
releases an old one.

```cpp
void move_timeline(catalog & metadata, everett::blob_identity const & prepared) {
  // `prepared` was already sealed and admitted with register_chain.
  auto first = metadata.create_timeline("create-main", "main", prepared);
  auto branch = metadata.fork_timeline("fork-experiment", "experiment", first);

  // Build and register a candidate before trying publication. This example
  // reuses the same root: even that publication advances the generation.
  auto result = metadata.publish_timeline("publish-main-1", first, prepared);
  if (result.published) {
    // result.head.generation == 1, with a new permanent generation owner.
  } else {
    // result.head is the head observed by this failed comparison.
  }
  (void)branch;
}
```

The interface makes the selection explicit:

| Operation | Result and contract |
| --- | --- |
| `find_timeline(name)` | Optional `catalog_timeline_head`: `name`, `generation`, `head`, `owner`. Reads the current generation through the `(name,generation)` index. |
| `create_timeline(op,name,head)` | Creates generation zero of a fresh name, selecting an already registered prepared head. |
| `fork_timeline(op,new_name,source)` | Creates generation zero from the **exact historical source record**, including name, generation, head and owner. The source may have advanced since it was read. |
| `publish_timeline(op,expected,candidate)` | Compares every field of `expected` with the current record and returns `{published,head}`. Success appends the next generation; a stale comparison records the observed current head. |

A candidate must already be a registered prepared root, including any routing
prefix. These operations touch catalog metadata only: they do not construct a
query graph, open external objects, scan key contents or copy directories.
Publishing to an unknown timeline is an error. When the expected record is stale,
publication
returns a conflict without inspecting whether the candidate is registered; no
candidate is installed. A successful same-root publication still increments
the generation, so returning to an earlier object pair cannot disguise an
intervening publication. Generation numbers range from zero through SQLite's
maximum signed 64-bit integer; an exhausted timeline cannot append again.

The comparison, new owner, exact root pin, generation row and operation outcome
share one `BEGIN IMMEDIATE` transaction. SQLite serializes competing writers;
only one publisher using the same expected generation can succeed. A conflict
is also a committed operation outcome. Repeating its operation ID returns the
**originally observed** head, even after later publications. Successful create,
fork and publish replays likewise return their original generation, not today's
current head. A new attempt after a conflict needs a new operation ID and a
fresh expected record.

Every generation has an immutable `timeline` owner with an injective binary
encoding of its name and generation. Composite foreign keys bind that row to
its exact retained root. `find_timeline` therefore selects an already retained
root: a concurrent publication cannot remove the root before the caller opens
it with `open_mapped_query`. Forking retains its selected historical root under
a separate new owner. Immutable saves and existing reader owners remain
independent. This is conservative retention, **not** pin retirement or garbage
collection; an unbounded publication history retains an unbounded union of
physical objects. [SQLite foreign-key contracts](https://sqlite.org/foreignkeys.html).

New catalogs use schema version 2. Version 1 catalogs still open and support
reservations, seals, graph registration, immutable saves and reader acquisition.
Their timeline methods explicitly reject the unsupported capability.
`schema_version()` exposes the distinction. Opening never migrates an old
catalog, and the old version-1 implementation rejects a version-2 catalog.
The catalog schema version is independent of the immutable file envelope and
section-codec versions.

## Transactions and operation identities

Every successful mutation records its operation kind, **exact canonical request
bytes** and outcome in the same transaction as its rows. Repeating the same ID
with identical arguments returns the previous outcome. Reusing it with different
arguments is rejected. There is no hash-collision shortcut to replay equality.
`lookup_operation` exposes the recorded descriptor and outcome for diagnostics.

Operation IDs, names and owner IDs are arbitrary nonempty byte strings at the
C++ boundary and are bound as SQL BLOBs. We can use embedded NUL bytes without
relying on SQL text-expression behavior. Fixed operation kinds and validated
32-character hexadecimal physical IDs are TEXT. Counts are checked before
conversion to SQLite's signed integer range; the canonical policy and request
fields use explicit little-endian unsigned encodings.

I use short `BEGIN IMMEDIATE` transactions, WAL mode, `synchronous=FULL`, verified
foreign keys, defensive configuration, and full-fsync/checkpoint-full-fsync
settings. Separate connections can contend. `catalog_options::busy_timeout_ms`
bounds SQLite's waiting; an exhausted wait is an error, never an acknowledgment.
A single catalog handle must not receive concurrent calls.
[SQLite transaction behavior](https://sqlite.org/lang_transaction.html),
[SQLite synchronization modes](https://sqlite.org/pragma.html#pragma_synchronous).

Opening validates the required table and trigger definitions without scanning
all catalog rows. Incompatible columns and unexpected triggers on protected
tables are rejected; diagnostic views and indexes are permitted.

The tables are readable with ordinary SQLite tools:

```sql
SELECT name, native_id, index_id FROM saves;
SELECT hex(name), generation, native_id, index_id FROM timeline_generations;
SELECT owner_kind, hex(owner_id), native_id, index_id FROM owner_roots;
SELECT id, kind, attempt, bytes, crc, barrier FROM objects;
SELECT hex(id), kind, length(request), length(outcome) FROM operations;
```

The implemented schema separates `objects`, `pairs`, `owners`, `owner_objects`,
`owner_roots`, `attempts`, `saves`, `timelines`, `timeline_generations` and
`operations`. Foreign keys express the exact
pair relationships. Immutable-row triggers reject updates and deletions; the
one permitted object transition fills all seal metadata once. The catalog is
trusted application metadata, not a sandbox for hostile SQL clients. External
SQL writes that bypass the API's transactional invariants are unsupported.

These retention rows do not add terms to a world fingerprint. I have not chosen
a serialized algebra-element policy for this adapter. The in-memory pin owner
continues to distinguish a file's additive contribution from its own-record
hash, while this component stores physical identities and retention.

## Failure boundaries

A SQLite storage error or unsuccessful COMMIT poisons the handle. Further
operations on it fail. The error carries the operation ID, SQLite result code
and an `outcome_unknown` flag. We inspect autocommit state and attempt rollback
when a transaction remains active, but successful rollback after an error is
not evidence that an earlier synchronization was durable. An allocation or
validation failure before COMMIT rolls back the current operation; a failed
rollback also poisons the handle.

A new connection can inspect whether an operation is visible and perform exact
replay reconciliation after acknowledgment loss. That is **not** a recovery
certificate following a real failed filesystem sync. Scanning readable pages,
reopening SQLite, or observing an operation row cannot prove what survives the
next reboot. Existing durable saves, timeline generations and reservations are
never released by this adapter. Catalog creation also leaves an unsuccessful or uncertain new
catalog name in place instead of automatically deleting and reusing it.

The `sqlite_catalog_ops` COMMIT hook makes acknowledgment failure testable:
one test returns an error without committing, and another executes a real
COMMIT and then reports an error. These tests verify poisoning, operation
matching and conservative retention. They do not simulate torn sectors, VFS
write reordering or power loss.

A forwarding SQLite VFS test also injects errors immediately before and after
each reached `xWrite` and `xSync` in reservation, save and reader acquisition:
114 cases over 57 I/O sites. A fresh connection finds all old roots and pins
unchanged, with each new operation either fully present or absent. Integrity
and foreign-key checks and an existing saved query still succeed. These are
real SQLite write/sync error paths with forwarded locking and shared memory;
the test does not simulate partial writes, reordered persistence or power loss.

A separate process-interruption suite stops a writer at 20 bounded points.
For reserve, each seal-receipt record, chain registration, save and reader
acquisition, it sends `SIGKILL` immediately before COMMIT or immediately after
a successful COMMIT, before the caller receives its result. Four more cuts
occur after sealing an external file and before recording its receipt. Each
process opens its own SQLite connection after `fork`.

Fresh connections verify the exact committed operation prefix, descriptors,
individual pins, target edges and saved queries, then repeat the replay checks
after another close/reopen. The old save remains readable at every cut. These
tests cover process death and lost acknowledgment while the operating system
continues running; physical power-loss behavior remains a separate property.

The timeline suite exercises competing connections, exact historical forks,
binary names and operation IDs, replay after an intervening publication, and
old version-1 catalogs. It injects COMMIT errors before and after the actual
commit for create, fork, successful publication and stale comparison. Eight
additional process cuts kill a child at those same before/after boundaries,
then verify complete outcomes, exact generation pins, old saves and mapped
queries after reopen. A metadata-only fixture temporarily hides every object
file after registration, so an accidental payload reopen in any timeline
operation fails the test. These checks establish the tested transaction and
process-restart behavior, not survival of physical power loss.

The streamed publication suite adds 26 kills while merging real mapped files
and publishing the result. Byte and partial-bit fixtures interrupt private
payload writes, sealing, receipt recording, registration and the timeline
transaction. Reopening checks the exact operation outcome, every retained
source/reader/history/output pin and the old saved values. An interrupted
private output remains unadopted; the test restarts that merge from its inputs.

## Scope

I can persist and reopen prepared mmap query chains, reserve their construction,
retain immutable saves and readers, and compare-and-publish named timeline
heads. This component does not yet release an owner, expire a reader lease,
collect files, resume a merge, migrate a schema or apply categorical updates. The richer
[ownership and publication design](catalog.md) supplies those next contracts;
[the durability model](durability.md) explains why external-file barriers and
catalog commitment remain distinct.
