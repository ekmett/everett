Connecting to a named session
========================

Most applications can start with `connect`, then use the result as a mutable
string table:

```cpp
#include <everett/connection.h>

int main(int argc, char ** argv) {
  if (argc != 2) return 64;
  auto storage = everett::multiverse<>::create(argv[1]);
  auto db = storage.connect("earth-616");
  db.put("name", "Everett");
  auto name = db.get("name");
  return name != "Everett";
}
```

`connect` opens the latest version of that named session, creating an empty one
when needed. Its default key and value type is `std::string`, including
embedded zero bytes. A missing key returns an empty `std::optional`; a stored
empty string remains present. The default uses the byte profile, 15:1 sampling,
and byte-counted front coding. Its single string sort needs no sort code.
The free function `everett::connect(directory, name)` provides the same operation
without keeping a multiverse object.

`multiverse<>` uses `storage_policy<>`: the [byte string profile](byte-transport.md)
with raw string payloads and byte tombstone tags. For an explicit bit table,
use `multiverse<string_policy>`. That registry assigns code zero to strings,
reserves code one, and uses order-zero exponential-Golomb backspaces. Both
profiles provide the same connection API. The policy and schema must match
when reopening a named table.

The ordinary `active_engine<P>` uses the redundant scheduler. Bit registries
write sort-owned native records, retaining small private merge and index
outputs under a shared allowance and streaming larger ones to files.
Publication seals every retained output needed by the durable snapshot.
Byte registries use the same scheduler with byte-aligned
opaque records. Each admitted item pays structural merge and index work. A
registry containing only the optional-string sort also [rebuilds obsolete history](replacement-rebuild.md),
so deletion reduces the current table's physical generation as well as its live
count. Saved snapshots keep their own files.
[The storage context](sort-runtime-context.md) describes its buffers and barriers.

Enable the SQLite component when building Everett, then link its CMake target:

```cmake
target_link_libraries(my_application PRIVATE everett::sqlite)
```

`multiverse<>::create` creates missing directories and syncs each new directory and
its parent. Existing ancestors must already be durable and trusted; the ordinary
`multiverse` constructor and free `connect` require an existing backing directory.
I create `catalog.sqlite3` exclusively, and never format or replace an existing
catalog. A competing creator can make the initial connection fail; an incomplete
or incompatible catalog requires explicit attention. With
`connection_options{.create_if_missing = false}`, both the catalog and the named
session must already exist.

Reads, writes and snapshots
---------------------------

`put`, `erase`, and `change` wait until their contribution has been published
durably. They return an owning snapshot of their result. `get` reads whichever
durable snapshot was current when the read began.

`begin()` starts a [transaction](transactions.md) with a private nursery. It
supports read-your-writes snapshots, branches, private flushes and one checked
durable commit for the resulting state.

```cpp
auto before = db.snapshot();
auto after = db.put("name", "Persistent snapshots");
// before.get("name") still returns "Everett".
// after.get("name") returns "Persistent snapshots".
db.erase("name");
// db.get("name") is now absent; both snapshots remain readable.
```

A snapshot retains mapped files, rather than copying their records. It remains
readable after further writes or after the connection closes. Each snapshot also
has `signature()` and `live_count()`. Equivalent background merges preserve
both; the signature is an algebraic sanity check, not cryptographic identity.

The [streaming scan](typed-scan.md) walks the live rows of a captured snapshot
in key order, resolving older contributions without collecting the whole table.
Use `db.range(lo, hi)` for a half-open interval, and `db.erase_range(lo, hi)` to
publish a checked batch of tombstones for its selected rows. The range guide
explains iterator copies, concurrent insertions and fractional-cascade positioning.

Deleting an absent key fails. The connection stays usable after that rejected
command: we do not manufacture a deletion credit for something that was never
present.

`save` gives a snapshot an immutable durable name:

```cpp
db.save("before-edit", before);
auto saved = db.load("before-edit"); // optional owning snapshot
auto branch = db.fork("earth-617", *saved);
branch.put("name", "Another branch");
```

The two-argument forms select a particular snapshot. `save(name)` and
`fork(name)` capture the connection's current snapshot. Saving and forking pin
existing objects; neither operation rewrites their data or indexes. Saved names
and newly forked session names must be unused. `load` retrieves a save without
changing the live session. The current catalog retains historical generations and
pins; automatic reclamation is still separate work.

Each durable snapshot carries its exact catalog generation in `head()`. The
root and its semantic checkpoint are published together. A save made while a
background merge completes still refers to the captured generation, rather than
silently following the live session to its next representation.

Asynchronous writes
--------------------

`put_async`, `erase_async`, and `change_async` enqueue mutable commands and
return tickets. Returning a ticket means acceptance into the in-memory queue,
not durable publication. A ticket's `get()` waits for its outcome: it returns
the owning durable publication on success, or throws on rejection or failure:

```cpp
auto first = db.put_async("name", "one");
auto second = db.put_async("name", "two");
auto result = second.get();
// result->world.get("name") == "two"
first.get(); // Check this command's outcome too.
```

`wait()` and `ready()` report completion, including cancellation or failure;
they do not check whether the command succeeded. The queue itself is not a
durable input log. After a crash, reopening recovers the published state and its
merge frontier, not commands that were only waiting in memory.

Mutable commands are interpreted against the state reached by earlier queued
commands. They do not secretly capture the state at submission time. This lets
several threads submit writes to the same key; each returned snapshot describes
that command's position in the queue.

For prepared contributions, use `submit`, `try_submit`, or `apply`.
`try_submit` returns an empty optional when admission capacity is unavailable;
the other submission path waits for capacity. `cancel(ticket)` can cancel a
queued command before the worker claims it. Dropping a ticket does not cancel it.

Group related writes into one batch to validate and publish them together:

```cpp
auto changes = decltype(db)::core_type::batch();
changes.put("left", "L");
changes.put("right", "R");
auto together = db.apply(std::move(changes).finish());
```

Batch keys must be distinct; `finish()` sorts them and rejects duplicates.
`submit` accepts the same finished batch for asynchronous publication. The
ordinary bit-profile table constructs a power-of-two prefix of an initial batch
directly, then admits any remaining records through ordinary charged admission.
The complete batch is validated first and published together. Existing tables
use ordinary charged admission throughout. The
[runtime guide](redundant-runtime.md#initial-sorted-batches) gives the construction
and its work accounting.

With the built-in engines, an empty batch still validates its schema. Once
admitted, it returns this handle's existing durable snapshot without charging merge work or creating a
new catalog generation. It does not refresh a stale handle or act as a
compare-and-swap barrier against another writer. Local ticket generation and
revision counters can still advance, and the worker can independently service
pending merges before or after the empty command.

The defaults allow 64 outstanding contributions, 64 MiB of retained encoded
input, and the selected engine's structural quote for 1024 records. Those records
can be grouped into batches; the queue reserves their total until each command
finishes. For the ordinary string table this is 41,330,608,128 structural units.
It is a conservative work allowance, not a count of disk operations or bytes.

`connection_options::limits` is optional. An omitted value selects these
engine-specific defaults once when connecting; `db.limits()` returns the resolved
values. Supplying a `session_limits` value preserves every explicit limit exactly,
including a zero work or byte limit. A fork retains its parent's resolved limits.
The limits cover admitted queue input and its structural reservation, including
the command currently executing. They do not
bound mapped snapshots retained by readers, rewritten bytes, user-defined
callbacks, fsync latency, or the total size of the table.

The worker advances pending merges between contributions and applies
backpressure before another admission can violate the runtime's frontier
invariants. Partial builders remain alive between service steps. When an
equivalent representation is complete, it is sealed and published durably,
then replaces the current link. The detailed `publication()` view distinguishes
a logical generation from an equivalent representation revision; tickets retain
their own publication even after that replacement.

`close()` stops new submissions and lets accepted commands finish unless the
worker fails. `shutdown()` also joins the worker; destruction does the same.
Joining does not rethrow a worker failure or check individual command outcomes.
Even after shutdown or when `pending_count()` is zero, use each ticket's `get()`
to check its result. `failure()` reports an error that stopped the worker; a
healthy validation rejection is reported only by its ticket. Optional background
maintenance can stop at shutdown because the durable frontier records how to restart its
carry. Reopening maps that frontier and restarts an unfinished private carry;
the checkpoint does not preserve its partially built output. It does not decode
the whole table to construct an in-memory replacement.

Conditional partition updates
------------------------------

The connection's mutable helpers use the engine's unbased command factories.
A snapshot's factories instead retain it as a conditional base:

```cpp
auto base = db.snapshot();
auto left = base.put("left", "L");
auto right = base.put("right", "R");
db.apply(std::move(left));
db.apply(std::move(right));
```

Each contribution validates its affected keys against their values in the
base. Disjoint contributions can therefore arrive in either order and produce
the same contents and signature. A changed value at an affected key rejects
that contribution. Unrelated changes do not invalidate the entire snapshot.
The [typed-world guide](typed-world.md) describes batches and general composable
arrows.

Custom sorts and synchronous engines
------------------------------------

Pass an explicit typed core to `connect` when using another registry:

```cpp
using core = everett::active_engine<my_policy>;
auto db = everett::connect<core>(directory, "records", {
  .schema_id = "my-application/records/v1"
});
db.change<my_sort>(key, arrow);
```

The schema identity is checked on reopen. It describes the registry's ordering,
codecs, hash functions, and semantics; changing it does not perform a migration.
The built-in bit string registry uses
`everett.optional-string/code0/sort-profile-v1` automatically.
Other registries require an explicit stable identity. The durable metadata
codec currently supports the 64-bit fingerprint element.

For a synchronous caller that owns its scheduling loop,
`persistent_engine<Core>` supplies `contribute`, `snapshot`, `pending`,
`admission_ready`, and `advance(budget)`. It implements the same engine contract
as the generic [session](session.md). Contributions and completed layouts are persisted
before returning. The public result is mapped; when the runtime becomes
settled, the engine releases its owning construction buffers and continues from
those mappings. While a carry is pending it retains its private builders, so
publication does not throw away work already performed.

Failure and competing writers
------------------------------

One connection serializes its callers. Separate connections writing the same
named session compete through catalog compare-and-publish. A loser fails rather
than overwriting the winner. Reconnect to inspect the new durable state before
deciding whether to prepare another contribution. Fork a session when independent
writable histories are intended.

An input rejection that leaves the typed core healthy rejects only its ticket.
An execution, durability, publication, or restoration failure stops the writer;
earlier snapshots remain readable. A failure can happen after the new generation
commits but before the caller receives its snapshot. Reopening discovers that
generation with its matching checkpoint. I do not blindly retry failed writes:
replaying a general arrow could apply it twice. `failure()` reports the worker's
exception, and the synchronous engine exposes `failed()` and `last_operation()`.
A catalog failure retains its actual operation ID, including failures from the
streamed builder's separate catalog connection, so acknowledgment loss can be
reconciled against the operation that failed.

Normal reopening checks metadata and schema compatibility. Full integrity scans
remain explicit recovery operations. These guarantees use the file sealing and
SQLite publication rules in the [runtime storage guide](runtime-store.md) and
[catalog guide](sqlite-catalog.md).

Extending a registry
--------------------

A reserved sort-code hole can acquire new sorts while keeping the existing sort
codes and record grammars compatible. Open the expanded registry with the same
explicit schema identity. A generic `active_engine` accepts the old string
table's replacement checkpoint, validates its generation against the stored
admission mass, and keeps its signature, live count and schema. Pending physical
merge jobs remain part of the restored frontier.

The expanded generic executor does not claim the string-only cleanup bound.
Its next publication stores ordinary typed metadata, shedding the old rebuild
generation. Named saves and forks can still retain and reopen the earlier
replacement checkpoint through this forward adapter. I distinguish the two
metadata layouts by their exact length for the expected schema, not by guessing
from the table fingerprint. An ordinary typed checkpoint cannot be opened as a
replacement generation without its clean-base and mutation accounting.
