Transactions and private nurseries
=================================

Use a transaction to collect edits and publish them together:

```cpp
auto tx = db.begin();
tx.put("name", "Everett");
tx.put("version", "one");
auto before = tx.snapshot();
tx.put("version", "two");
// before.get("version") is still "one".
// db sees none of these edits yet.
auto committed = tx.commit();
```

`begin` retains the connection's current snapshot. Reads see the transaction's
own edits over that original state. `commit` returns its durably published
snapshot; `abort` discards the transaction's ownership. Destroying an uncommitted
transaction also releases its ownership. Ordinary connection writes remain
individually durable; see the [connection guide](connection.md).

Persistent snapshots
--------------------

The in-memory nursery is an ordered `nursery_map`. Mutable nodes share an edit
token. Freezing closes that token once and retains the root; later edits use a
fresh token and copy only paths they change. A node is editable in place only
when its token belongs to that editor and remains open. A child with one direct
reference can still be reachable through a shared ancestor, so its reference
count alone is not an ownership proof.

The token follows the approach in my
[transient maps](https://github.com/ekmett/transients/blob/ef82ff108ac2ec05bde5ef2336c825f2ed58ab6f/src/Data/Transient/WordMap/Internal.hs).
Here an AVL tree bounds lookup and edit paths by $O(\log m)$ for $m$ buffered
keys. Snapshot capture retains a root and closes a token in constant time;
reclaiming an unshared tree can still visit its nodes.

Any retained snapshot can start another editor:

```cpp
auto branch = before.branch();
branch.put("version", "another possibility");
// before remains readable, even if its original transaction aborts.
```

This is full persistence: an older snapshot remains editable through a new
branch. A transaction has one mutable owner; frozen snapshots can be shared.
Each branch retains the same original publication base, so two competing
branches cannot silently overwrite one another. To create independent durable
names, use the connection's `fork` operation on a published snapshot.

Repeated replacement edits retain their net change from the staged state.
Inserting then deleting a previously absent key removes its nursery entry.
Deleting an already absent key fails. General sort arrows compose in
chronological order and keep their sort's validation and application rules.

Private flushes
---------------

A transaction may flush without committing:

```cpp
auto tx = db.begin();
tx.put("first", "batch");
auto staged = tx.flush();
tx.put("second", "batch");
tx.commit();
```

`flush` turns the sorted nursery into an ordinary typed contribution. Admission
charges and executes merge work through the existing runtime. The private
frontier advances, its complete file graph is sealed, and a fresh nursery
accepts later edits. The returned snapshot includes the flushed edits. Empty
flushes allocate no execution core, construction scope or output file.

Private output currently uses ordinary `.kv` and `.index` files. These files can
exist durably before any named session refers to them. Construction pins retain
their exact dependencies, including hidden completed merge artifacts. The
original published state remains retained as the rollback base. Private
construction does not create permanent advisory merge-cache owners.

A flush runs synchronously in its transaction. Separate transactions can work
independently; one transaction cannot mutate its nursery concurrently with its
own flush. The [fixed-key layouts](table-layouts.md) describe additional physical
formats under consideration, not alternative transaction formats already built.

Commit and conflicts
--------------------

`commit_async` first completes the private flush synchronously, then queues
publication and returns the usual connection ticket. `commit` waits for that
ticket. The complete graph passes its file barriers before SQLite installs the
new named root. That SQL commit is the external visibility point. The new
published ownership is acquired before old execution state and private
construction ownership are released.

A prepared commit consumes one queue contribution slot. Its private execution
has already paid structural work, so it reserves no additional input bytes or
work credits. Nursery and private execution memory lies outside the session's
accepted-input byte bound. Submission transfers the candidate execution core to
the queued request and then the worker.

The connection tracks logical changes separately from equivalent background
merges. A transaction can commit across background layout changes, but a
competing logical write on the same connection causes `transaction_conflict`.
This rejection leaves the connection usable. An independent connection or
process still competes through SQLite's checked head publication and the
existing stale-writer failure contract. Transactions do not automatically
rebase their reads or merge concurrent edits.

An empty transaction is a local no-op after checking its original logical base.
It creates no file or publication generation. It is not a freshness barrier
against another process's writes.

Uncertain publication has the same rules as ordinary writes: an error after SQL
commit can mean that the new state is durable even though acknowledgment failed.
Reconnect and reconcile before retrying. A failed private execution poisons that
transaction; abort it. It does not publish its partial state.

Ownership and recovery
----------------------

Private snapshots, branches, queued publication and the adopted execution core
share a construction lease. The installed core retains its lease even after
its pending merge work settles, until that core is replaced or destroyed.
Aborting one editor does not release files still needed by another owner. The
final lease release attempts to append a catalog release event; a cleanup
failure leaves recovery to retry. Effective ownership views exclude a released
scope's construction pins; seal evidence and published owners remain intact.

Each lease has a locked sidecar name. Its name is flushed before the scope is
registered, and removed only after durable release. Explicit recovery skips
live locks and releases abandoned construction:

```cpp
auto storage = everett::multiverse<>("data");
auto recovered = storage.recover_transactions();
```

The result counts newly released abandoned scopes. Recovery also removes
unlocked orphan lease names left before scope registration or after release.
An active catalog scope with a missing lease is an error. Ordinary `connect`
does not scan the directory for recovery.

Releasing construction ownership does not physically collect immutable files.
Published generations and ordinary durable owners still retain their history.
File collection and retirement of that history remain separate work; see the
[implementation ledger](implementation.md) and [catalog guide](sqlite-catalog.md).
