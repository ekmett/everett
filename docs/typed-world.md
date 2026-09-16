Typed worlds and contributions
=============================

`typed_engine<>` connects the encoded runtime to a usable string table. Its
snapshots provide `get`; its engine applies `put`, `erase` and general changes
and maintains the live count and additive signature. A [session](session.md) runs that same
engine behind a mutable current link.

The default is bit-first. `string_registry` assigns code zero to
`unsorted<std::optional<std::string>>` and reserves code one for a later sort.
`string_policy` uses that registry with 15:1 sampling and the normal exponential-
Golomb backspace choice. One occupied sort lets us omit explicit sort arguments.

```cpp
#include <everett/typed_world.h>

int main() {
  everett::typed_engine<> engine;
  auto first = engine.contribute(engine.put("name", std::string("Everett")));
  engine.contribute(engine.put("description", std::string("Persistent snapshots")));
  auto third = engine.contribute(engine.erase("name"));
  if (first.get("name") != "Everett" || third.get("name")) return 1;
  return third.live_count() != 1;
}
```

Snapshots own their complete query graphs. Updating or destroying the engine
does not change an earlier snapshot. `pending()` and `advance(budget)` expose
background layout work; an equivalent result retains the same signature, live
count and schema identity.

For a mutable async handle:

```cpp
using engine_type = everett::typed_engine<>;
everett::session<engine_type> current(engine_type{}, {
  .work = 1'000'000, .bytes = 64 * 1024 * 1024,
  .contributions = 64, .maintenance_budget = 4096
});
auto after = current.apply(engine_type::put("name", std::string("Everett")));
// apply waits for publication; submit returns a ticket instead.
current.shutdown();
```

Contributions and batches
-------------------------

A contribution owns its encoded changed records. The engine's static `put`,
`erase`, `change` and `batch` factories create ordinary mutable commands. They
are evaluated against the current state when the engine applies them. Two
queued writes to the same key therefore take effect in queue order.

A snapshot's factories additionally retain that snapshot as a conditional base.
Before applying one of these contributions, I compare each affected key's current
logical value with that key's value in the base. The complete batch is validated
before changing the executor. A stale old value fails; an unchanged key need not
be rejected merely because another key changed in the meantime.

This gives us disjoint contributions from a shared base:

```cpp
auto base = engine.snapshot();
auto left = base.put("left", std::string("one"));
auto right = base.put("right", std::string("two"));
engine.contribute(std::move(left));
engine.contribute(std::move(right));
```

Their logical result and additive signature are independent of application
order. This is old-value validation, not a general merge of conflicting edits.
It also is not an operation replay ledger: applying an arrow twice requires the
application's operation identity protocol to reject the replay where necessary.

A batch can contain several sorts. `batch.put<S>`, `batch.erase<S>` and
`batch.change<S>` accumulate records; `std::move(batch).finish()` sorts their
canonical encoded keys and rejects duplicates. A snapshot's single-key helpers
build the same contribution type. A default-constructed batch is unconditional;
`snapshot.batch()` binds one to a base. Deleting an absent key fails, including an
explicit absent replacement passed through `put`, so it cannot manufacture a
live-count decrement or a deletion credit.

Preflight rejection leaves the engine and its current publication unchanged.
`failed()` remains false, so a session can reject that ticket and accept the next
command. An exception after runtime execution begins poisons the engine instead;
its previous immutable snapshot remains readable.

Sort-owned semantics
--------------------

`sort_codec<S>` selects the key and arrow codecs. `sort_semantics<S>` defaults
to the static operations declared by `S`; it can also be specialized. The
optional-string sort has a supplied replacement specialization.

A general sort supplies:

- `state_type`, the result returned by `get<S>`.
- `initial(key)`, its absent starting state.
- `apply(key, state, arrow)`, the effect of an update.
- `compose(key, older, newer)`, chronological arrow composition.
- `present(key, state)`, whether the logical binding contributes to live size.
- `hash_key(key)` and `hash_value(key, state)`, returning 64-bit hash values.

The key and arrow types come from `sort_codec<S>::key_codec::value_type` and
`sort_codec<S>::value_codec::value_type`. Codecs own their representations;
semantics own their interpretation. `present(key, initial(key))` must be false.
Composition must be associative and satisfy

$$
\mathrm{apply}(k,s,\mathrm{compose}(k,a,b))
=\mathrm{apply}(k,\mathrm{apply}(k,s,a),b).
$$

The engine dispatches through the complete registry during merging. It does
not assume the last sort passed to `put` is the only sort that existing blobs
contain. A sort with `replacement = true` promises that its newest occurrence
fully determines the state; it permits the first-match read optimization and
`put`/`erase` helpers. Its `erase(key)` supplies the tombstone arrow. Other sorts
use `change<S>` and fold all matching arrows from oldest to newest, including
already composed arrows produced by merges.

For replacement sorts, deletion admission also captures the matching record's
physical FC retained position during the validating lookup. The new tombstone
uses that limit to keep the canceled literal available to a parallel merger.
The current target is checked even when the contribution came from an
equivalent snapshot with another encoding. Ordinary reads do not request this
metadata. The [native merge guide](native-merges.md#conservative-tombstone-literals)
describes the encoding and its space-accounting boundary.

For example, an append sort can use strings for both its state and arrow,
concatenation for `apply` and `compose`, the empty string for `initial`, and
nonemptiness for `present`. It must supply hashes for the resulting state.
That query reconstructs its state from the arrows; a logarithmic number of runs
does not bound the size of that reconstruction.

Fingerprint and checkpoint metadata
-----------------------------------

Each affected binding contributes

$$
h_{K,S}(k)h_{V,S}(k,v),
$$

with zero contribution when absent. A change subtracts its old binding and adds
its new binding. The default algebra wraps unsigned 64-bit addition and
multiplication; `typed_engine<P,A>` can use another compatible algebra.
**The sort's dispatch bits are never hashed.** Rebalancing a registry changes
those codes without changing its sort-owned logical hash functions.

`typed_world_metadata<A>` contains `signature`, `live_count` and `schema_id`.
For the default 64-bit algebra, `encode` stores the two 64-bit little-endian
numbers followed by the nonempty schema ID; `decode` recovers that compact
payload. The runtime's admission intervals are a separate part of a durable
checkpoint. No table scan is needed to save this metadata.

The default schema ID is `everett.optional-string/code0/v1`. The explicit tagless
byte policy uses `everett.optional-string/tagless/byte-profile-v2`
with the [byte string transport](byte-transport.md). Other registries require a
caller-supplied stable schema ID when constructing the engine. This identifier
must name the codecs and semantic policies needed to interpret the saved data;
it is not a compiler type name, tree fingerprint or additive state signature.

`typed_world<P,A>::restore(runtime, metadata, expected_schema)` checks the expected
schema and that the live count fits the admitted history. The graph and metadata
must already have been admitted together: this function does not rehash every
record or establish the authenticity of supplied metadata. `typed_engine::from_snapshot`
then resumes the active runtime from that immutable frontier. Hash equality is
never used as a substitute for an exact snapshot or file identity.

Work and transport boundaries
-----------------------------

The default binary runtime retains one pending carry chain. `admission_ready()` exposes that
backpressure; the session services existing work before claiming another queued
contribution. Direct synchronous `contribute` drains prior work before admission.
A multi-record batch can also drain carries between its records.

`reservation` quotes **ready-admission allowances** and owned encoded record
bytes. It does not quote the total work of an old carry or of every intermediate
carry in a batch. `work()` reports the runtime's executed structural accounting.
The default engine limits each imported or published root to 256 main-chain
nodes, including empty routing ancestors. Its third template parameter changes
that explicit support limit. Each input record reserves
$2K + 128 + D + 32$ ready-admission units, with at most 64 runtime runs and root
depth at most D. Before admission, the engine checks the actual metadata-derived
charge and depth against those limits. It also checks the completed root before
publishing a contribution or an `advance` result. An over-limit imported layout
is rejected before changing a healthy engine; execution that exceeds the limit
poisons the engine and leaves its previous snapshot available. This enforced
support bound is separate from a complete deamortized COLA proof or latency bound.

The fourth engine parameter selects a runtime family. To use the redundant
schedule:

```cpp
#include <everett/redundant_runtime.h>
#include <everett/typed_world.h>
using engine = everett::typed_engine<everett::string_policy,
  everett::wrapping_fingerprint_algebra, 256,
  everett::redundant_runtime_family<everett::string_policy>>;
```

This path waits for admission readiness, rather than settling every higher
merge. Each admitted record offers the current logarithmic service allowance.
The final batch checkpoint captures hidden completed outputs as well as the
visible query graph. Before admitting another record, the runtime enforces its
remaining service obligation. Reopened partial workers receive recovery service
before admitting new changes.

The queue reservation includes a conservative ready-admission bound plus the
largest service allowance over 64 levels. Actual service uses the current
admission count. This gives a state-independent quote that submission threads
can compute safely; it is larger than the work normally executed by a small
table. See the [redundant executor](redundant-runtime.md) for its cost model.

A conditional contribution also pins its base snapshot. The byte reservation
counts its new encoded records, not the transitive old files kept alive by that
snapshot.
The session's contribution-count limit bounds the number of accepted base pins;
retained graph bytes and saved history need their own storage accounting.

The binary family and the byte-profile redundant family transport ordered
sort-qualified keys and encoded arrows through the opaque FC profile.
Ordered strings escape zero bytes and
terminate with a separate zero escape. Bit profiles store the arrow grammar
exactly; the explicit byte profile adds and validates at most seven zero padding
bits around an arrow.

The [sort-owned runtime](sort-runtime.md) instead uses each sort's physical key
and value grammar directly in its native files. Ordinary bit-profile
[connections](connection.md) select its streamed redundant family, with
replacement rebuilding for the default optional-string registry. These storage
families share typed validation, chronological composition and signature
accounting; their physical encodings differ.
