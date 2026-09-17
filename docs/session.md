A mutable session for immutable worlds
================================

A `session<Engine>` holds the current world and a bounded queue of contributions.
The session is mutable; every snapshot we take from it is immutable. We can keep
using an older snapshot while the session accepts updates or finishes an equivalent
layout in the background.

I use one worker per session. It runs the same charged contribution operation that
we can use synchronously, then publishes the resulting world. The engine remains
a concrete template parameter: there is no virtual backend or erased callback
queue. A session does not choose the registry, record grammar or merge semantics.

Using a session
-----------

Given an engine satisfying the contract below:

```cpp
#include <utility>

import everett;

void update(auto engine, auto contribution) {
  using engine_type = decltype(engine);
  everett::session<engine_type> live(std::move(engine), {
    .work = 1'000'000,
    .bytes = 64 * 1024 * 1024,
    .contributions = 64,
    .maintenance_budget = 128
  });

  auto before = live.snapshot();
  auto receipt = live.submit(std::move(contribution));
  // Accepted into the queue. Readers may still see before here.
  auto after = receipt.get();
  // Published: after->world owns the resulting immutable state.
  live.shutdown();
  // Both before and after retain their own pins after the session is gone.
}
```

`apply(contribution)` is the synchronous convenience: it submits to the same
worker and waits for that contribution's publication. It returns the same
owning publication as `ticket::get()`. Other threads may submit concurrently;
all engine operations run in queue order. Calls that overlap may be ordered
either way. A later contribution can already be visible by the time an earlier
call returns, but its receipt still identifies the earlier publication.

`try_submit(contribution)` returns an empty optional when accepting the input
would exceed a limit. In that case it neither copies nor moves the input.
`submit` waits for capacity. An input larger than a whole work or byte limit
throws `std::length_error` immediately, instead of waiting for capacity that
can never exist. A closed session throws `session_closed`; a failed session rethrows its
worker's exception.

The limits cover accepted contributions, including the one currently running:

- `work`: reserved admission allowances in the engine's structural units.
- `bytes`: retained input bytes quoted by the engine.
- `contributions`: the number of accepted, unfinished inputs.

The reservation is held until the charged operation completes or the input is
cancelled or rejected after failure. Popping the queue does not refund it.
`outstanding()` and `pending_count()` expose these counters. Reservations are
made before the session constructs its private input; the caller's input remains
caller-owned while a blocking submission waits for capacity.

Existing merge debt remains the engine's responsibility. An engine can expose
`admission_ready()` to require service before another queued contribution is
claimed. Its pending work then takes priority, while queued input reservations
remain held. This provides backpressure without charging an old carry to the
next small input. An engine must bound the unfinished work it permits and state
that bound separately; the queue's admission allowance is not a total merge-debt
counter.

These limits do not cover retained snapshots, the engine's working set, temporary
output, total rewrite bytes or elapsed time. In particular, stepping a bounded
number of records does not bound key lengths, value work, allocation, index
finalization or filesystem barriers. The engine must state what its service
units count. `maintenance_budget` uses those units; it is not a duration.

Publication and identity
------------------------

A publication contains `world`, `logical`, `generation` and `revision`.
`snapshot()` obtains them together through one atomic owning pointer.

A completed contribution gets a fresh exact `logical` token and increments
both counters. This is true even when its signature equals the previous
signature. An equivalent background layout retains `logical` and `generation`
and increments only `revision`. The token is a process-local identity, not a
hash or durable identifier. Counters are local to this session; do not compare
numbers from independent sessions as evidence of equal states.

The single worker cannot overwrite a newer contribution with stale background
work: all `contribute`, `advance` and publication operations are serialized.
The engine may return a background result only when it is equivalent to its
current logical state and retains the exact dependencies its indexes describe.
Matching signatures alone cannot establish that fact. Sharing a completed
merge from another branch still requires the engine's exact-input checks.

The session does not replace the catalog's publication protocol. A durable engine
must establish durability before returning its world. If another process can
publish to the same timeline, the engine must compare the complete expected
catalog head and handle conflicts. SQLite timeline generation counts every
publication, including equivalent layouts, so it is not this logical token.
An uncertain commit requires reconciliation under the same operation identity;
retrying it as a new contribution could apply it twice.

Cancellation, shutdown and failures
----------------------------------

`cancel(ticket)` succeeds only while that exact request remains in this session's
queue. Worker claim and cancellation are synchronized. A cancelled ticket stays
valid and throws `session_cancelled` from `get()`. Running or completed work cannot
be cancelled through this interface. Dropping a ticket or waiting elsewhere
does not cancel an accepted update. `ticket::ready()` checks whether its success
or exception is available without waiting. `ticket::wait()` waits for either
outcome without reporting it. Use `ticket::get()` to observe the publication or
the exception; completion alone is not a successful acknowledgment.

`close()` rejects further admissions, wakes blocked submitters and lets accepted
contributions drain. `shutdown()` additionally joins the worker. The destructor
uses the same draining behavior. Optional maintenance stops once the accepted
queue has drained; snapshots remain valid independently of the engine. Shutdown
does not rethrow a worker failure, and an empty queue can include failed or
cancelled requests. Check individual ticket outcomes even after joining.
Shutdown can wait for an active contribution or finalization and has no latency
bound.
Engine callbacks must not wait on their own session's receipts or destroy the session;
blocking submission and shutdown from its worker are rejected. Contribution
constructors and destructors must also avoid waiting for their own session: their
reservation is still held until they return, so a recursive submission or
shutdown could wait on itself. Nonblocking snapshot access remains safe.

By default, an engine exception stops the worker, preserves the last published snapshot,
fails the active and queued tickets, and rejects new submissions. `failure()`
returns its exception pointer, including failures during maintenance when no
contribution ticket was active. Failed private
engine state is destroyed before its reservations are refunded. Each returned
world must therefore own its transitive mappings and pins without relying on the
engine's lifetime. Durable uncertain attempts may still need catalog recovery;
a session exception is not a statement that an uncertain disk commit rolled back.
Existing permanent catalog owners also mean a queue limit does not bound total
retained history or implement garbage collection.

An engine may additionally expose `bool failed() const noexcept`. After an
exception from `contribute`, returning false certifies that the rejected request
left its logical state unchanged and the engine can continue. The session then
fails only that ticket, refunds its reservation and processes later requests.
This requires a world type with a nonthrowing move constructor. Exceptions after
`contribute` returns, or during maintenance, still stop the worker. A durable
engine must report failure after an uncertain publication; it cannot treat that
case as a harmless validation error.

Engine contract
---------------

```cpp
struct engine {
  using world_type = /* owning immutable handle */;
  using contribution_type = /* concrete input */;

  static everett::session_reservation reservation(contribution_type const &);
  world_type snapshot() const;
  world_type contribute(contribution_type);
  bool pending() const;
  bool admission_ready() const; // Optional; defaults to true.
  std::optional<world_type> advance(std::uint64_t budget);
};
```

`reservation` may run concurrently on submitting threads. It must be thread-safe,
use only the input and immutable policy, and conservatively cover its admission
allowance. The caller cannot supply a smaller quote. A quote
that depends on the future height needs a conservative admitted-height bound,
such as the engine's maximum representable level count. It must include the
engine's admission and required index work, not just native records. Previously
created merge debt is serviced through `advance` before admission when the
engine's readiness hook requires it. These are structural charges, not a bound
on byte rewriting, user callbacks or elapsed time.

`contribute` applies one input in queue order, performs its synchronous charged
service and returns a fully searchable owning world. It owns validation of old
values, chronological composition, disjoint partitions and operation replay.
A reservation does not authorize blindly rebasing an update prepared against
another state.

`pending` reports equivalent-layout work. `advance` progresses that
work and returns a world only when an equivalent layout is ready to publish.
It may return an empty optional while more work remains. The worker checks
`pending` again after each step and gives ready queued contributions priority.
If `admission_ready()` returns false, `pending()` must be true. An
engine must make progress for a positive maintenance budget or stop reporting
pending work; otherwise it will spin its worker.

The constructor obtains the initial snapshot and pending flag before starting
the worker. After that, the worker is the engine's sole owner. No engine callback
or contribution destructor runs under the queue mutex. If the engine owns
objects that other threads can mutate, their synchronization is its responsibility.

The tests use a controlled engine to exercise publication order, equivalent
layout identity, backpressure, cancellation, pinned snapshots and failure. They
do not turn the [COLA count model](cola-scheduling.md) into a production scheduler
or prove a byte/I/O service bound for a particular engine.
