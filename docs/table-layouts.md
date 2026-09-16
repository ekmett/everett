# Physical table layouts

I want three physical representations behind the same world and session API.
The string format remains useful, but a fixed-width key should not pay for
front-coded string navigation. This is the format direction; `.kv` is the
implemented persistent representation, while `.fv` and `.ff` are additions to
build.

| File | Keys | Values | Native offset directory |
| --- | --- | --- | --- |
| `.kv` | Front coded or sort-owned mixed grammar | Sort-owned grammar | Sampled Elias–Fano record offsets |
| `.fv` | Fixed-stride key array | Variable-size packed payloads | Elias–Fano value boundaries |
| `.ff` | Fixed-stride key array | Fixed-stride value array | Arithmetic addresses |

For `.fv`, key ordinal $i$ has an arithmetic address. A monotone sequence of
$N+1$ value boundaries selects the half-open payload interval for that ordinal,
including the real end-of-file boundary. Repeated offsets can represent empty
payloads; the sort must still distinguish empty values from absence. The
Elias–Fano universe includes the value payload extent, not the fixed key array.
This directory samples every value, so its select latency and storage tradeoff
differ from the current every-$W$ record directory.

The [fixed-key Metal experiment](../optional/fixed_gpu_merge/README.md) constructs
complete native outputs for candidate `.fv` and `.ff` layouts. Its fixture
envelope is experimental; these formats are not yet persistent runtime choices.

For `.ff`, both addresses are arithmetic and there is no native Elias–Fano
directory. Each fixed-stride region needs one physical slot width. How mixed
sorts share those regions belongs to the wire-format design; per-sort widths
alone do not establish one common stride.

Fractional indexes
------------------

The fixed-key formats still sample the downstream **augmented** native-plus-index
catalog. Their fractional indexes hold fixed-width borrowed keys, grouped rank,
and equality/false-borrow information. They need no index-side Elias–Fano
directory and no inherited FC comparison context. Once rank identifies the
native or borrowed ordinal, its key address is arithmetic. COLA main and
terminal secondary routes retain their existing meanings.

The `.kv` representation retains its sampled native offset directory and a
separate directory for each front-coded borrowed stream. Changing the physical
key representation does not change what the downstream sampling refers to.

Logical keys and shared machinery
--------------------------------

Fixed keys can be structured IDs, preserving useful entity/component or region
order, or hashes of stable logical identities. Content addressing is a separate
choice: a stable logical key can refer to a changing content hash. The weak
world signature is not a substitute for an identity or collision policy.

Sort dispatch, logical ordering, composition, signatures, pins, snapshots,
sessions, durability and merge scheduling stay shared. Physical access and merge
kernels specialize for the table representation. Fixed-width keys permit direct
parallel input cuts; variable output payload sizes still require sizing,
prefix sums and copying. Tombstone elision still requires complete older-history
coverage, regardless of key encoding.

The current Elias–Fano implementation is tuned for sampled offsets. Before
choosing its `.fv` layout, I want to measure finer select directories and paired
adjacent-boundary access for per-value navigation. Sequential merge traversal
also needs its own baseline: repeatedly selecting independent offsets can cost
more than walking the encoded sequence once.
