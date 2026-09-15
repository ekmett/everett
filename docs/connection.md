Connecting to a named tap
========================

Most applications can start with `connect`, then use the result as a mutable
string table:

```cpp
#include <diet/connection.h>

int main(int argc, char ** argv) {
  if (argc != 2) return 64;
  auto pantry = diet::fridge<>::create(argv[1]);
  auto db = pantry.connect("earth-616");
  db.put("name", "Diet");
  auto name = db.get("name");
  return name != "Diet";
}
```

`connect` opens the latest version of that named tap, creating an empty one
when needed. Its default key and value type is `std::string`, including
embedded zero bytes. A missing key returns an empty `std::optional`; a stored
empty string remains present. The default uses the bit profile, 15:1 sampling,
and order-zero exponential-Golomb backspaces. Sort code zero names the string
table and code one remains reserved.
The free function `diet::connect(directory, name)` provides the same operation
without keeping a fridge object.

Enable the SQLite component when building Diet, then link its CMake target:

```cmake
target_link_libraries(my_application PRIVATE diet::sqlite)
```

`fridge<>::create` creates missing directories and syncs each new directory and
its parent. Existing ancestors must already be durable and trusted; the ordinary
`fridge` constructor and free `connect` require an existing backing directory.
I create `catalog.sqlite3` exclusively, and never format or replace an existing
catalog. A competing creator can make the initial connection fail; an incomplete
or incompatible catalog requires explicit attention. With
`connection_options{.create_if_missing = false}`, both the catalog and the named
tap must already exist.

Reads, writes and snapshots
---------------------------

`put`, `erase`, and `change` wait until their contribution has been published
durably. They return an owning snapshot of their result. `get` reads whichever
durable snapshot was current when the read began.

```cpp
auto before = db.snapshot();
auto after = db.put("name", "A reduced COLA");
// before.get("name") still returns "Diet".
// after.get("name") returns "A reduced COLA".
db.erase("name");
// db.get("name") is now absent; both snapshots remain readable.
```

A snapshot retains mapped files, rather than copying their records. It remains
readable after further writes or after the connection closes. Each snapshot also
has `signature()` and `live_count()`. Equivalent background merges preserve
both; the signature is an algebraic sanity check, not cryptographic identity.

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
and newly forked tap names must be unused. `load` retrieves a save without
changing the live tap. The current catalog retains historical generations and
pins; automatic reclamation is still separate work.

Each durable snapshot carries its exact catalog generation in `head()`. The
root and its semantic checkpoint are published together. A save made while a
background merge completes still refers to the captured generation, rather than
silently following the live tap to its next representation.

Asynchronous writes
--------------------

`put_async`, `erase_async`, and `change_async` enqueue mutable commands and
return tickets. A ticket's `get()` waits for its owning publication:

```cpp
auto first = db.put_async("name", "one");
auto second = db.put_async("name", "two");
auto result = second.get();
// result->cola.get("name") == "two"
```

Mutable commands are interpreted against the state reached by earlier queued
commands. They do not secretly capture the state at submission time. This lets
several threads submit writes to the same key; each returned snapshot describes
that command's position in the queue.

For prepared contributions, use `submit`, `try_submit`, or `apply`.
`try_submit` returns an empty optional when admission capacity is unavailable;
the other submission path waits for capacity. `cancel(ticket)` can cancel a
queued command before the worker claims it. Dropping a ticket does not cancel it.

The defaults allow 64 outstanding contributions, 64 MiB of retained encoded
input, and one million structural work units. These can be changed in
`connection_options::limits`. The limits cover admitted queue input and its
structural reservation, including the command currently executing. They do not
bound mapped snapshots retained by readers, rewritten bytes, user-defined
callbacks, fsync latency, or the total size of the table.

The worker advances pending merges between contributions and applies
backpressure before another admission can violate the runtime's frontier
invariants. Partial builders remain alive between service steps. When an
equivalent representation is complete, it is sealed and published durably,
then replaces the current link. The detailed `publication()` view distinguishes
a logical generation from an equivalent representation revision; tickets retain
their own publication even after that replacement.

`close()` stops new submissions and drains accepted commands. `shutdown()` also
joins the worker; destruction does the same. Optional background maintenance
can stop at shutdown because the durable frontier records how to restart its
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
The [typed-cola guide](typed-cola.md) describes batches and general composable
arrows.

Custom sorts and synchronous engines
------------------------------------

Pass an explicit typed core to `connect` when using another registry:

```cpp
using core = diet::typed_engine<my_policy>;
auto db = diet::connect<core>(directory, "records", {
  .schema_id = "my-application/records/v1"
});
db.change<my_sort>(key, arrow);
```

The schema identity is checked on reopen. It describes the registry's ordering,
codecs, hash functions, and semantics; changing it does not perform a migration.
The built-in string registry uses `diet.optional-string/code0/v1` automatically.
Other registries require an explicit stable identity. The durable metadata
codec currently supports the 64-bit fingerprint element.

For a synchronous caller that owns its scheduling loop,
`persistent_engine<Core>` supplies `contribute`, `snapshot`, `pending`,
`admission_ready`, and `advance(budget)`. It implements the same engine contract
as the generic [tap](tap.md). Contributions and completed layouts are persisted
before returning. The public result is mapped; when the runtime becomes
settled, the engine releases its owning construction buffers and continues from
those mappings. While a carry is pending it retains its private builders, so
publication does not throw away work already performed.

Failure and competing writers
------------------------------

One connection serializes its callers. Separate connections writing the same
named tap compete through catalog compare-and-publish. A loser fails rather
than overwriting the winner. Reconnect to inspect the new durable state before
deciding whether to prepare another contribution. Fork a tap when independent
writable histories are intended.

An input rejection that leaves the typed core healthy rejects only its ticket.
An execution, durability, publication, or restoration failure stops the writer;
earlier snapshots remain readable. A failure can happen after the new generation
commits but before the caller receives its snapshot. Reopening discovers that
generation with its matching checkpoint. I do not blindly retry failed writes:
replaying a general arrow could apply it twice. `failure()` reports the worker's
exception, and the synchronous engine exposes `failed()` and `last_operation()`.

Normal reopening checks metadata and schema compatibility. Full integrity scans
remain explicit recovery operations. These guarantees use the file sealing and
SQLite publication rules in the [runtime storage guide](runtime-store.md) and
[catalog guide](sqlite-catalog.md).
