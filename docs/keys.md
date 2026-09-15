# Sorts and stringlike keys

Represent a logical key in Everett as a pair

$$
\kappa=(s,x),
$$

where the **sort** $s$ chooses how to represent the stringlike component $x$
and which key/value hashing strategies to use. Keys use bit strings or byte
strings according to the shared storage policy. The category of allowed
updates may depend on the complete pair $(s,x)$, not just on the sort.

The contract below lets these choices coexist in one key space. Concrete typed
codecs for byte-at-a-time and bit-at-a-time profiles are in `profile.h`.
The sort registry remains to be built. Consult the
[implementation ledger](implementation.md) for completed codec work, the
[store design](design.md) for blobs, and [per-key arrows](arrows.md) for update
semantics.

## 1. One canonical identity and order

The following operations must agree on the complete logical key:

- Comparison, equality, duplicate detection, and native/borrowed merging.
- False-borrow flags and fractional-index boundary selection.
- Partition assignment and validation of an update's source state.
- Key hashing and selection of the value-potential policy.

A partition policy may deliberately group several sorts or many keys together,
but it receives the complete key. Equal stringlike components in different
sorts are distinct keys. An empty stringlike component is a valid key; it does
not denote an absent binding.

We choose canonical bit encodings $E_S(s)$ and $E_s(x)$. Both the sort-code
family and each sort's key-code family are prefix-free: no complete codeword
is a proper prefix of another. Encode the pair by direct concatenation:

$$
E(s,x)=E_S(s)\,\Vert\,E_s(x).
$$

The concatenation is also prefix-free. Decoding the sort determines which
key decoder to use and where its bits begin. Under a bit policy, **there is no
implicit byte alignment between the two components**: a three-bit sort code
can be followed immediately by the first bit of a byte-oriented key code.
A byte policy requires both components to be encoded in whole bytes.

Lexicographic order on the encoded bits, with $0<1$, gives us a canonical
comparison order. Prefix freedom makes every sort's range contiguous: different
sort codes differ before either code ends, so every extension of one sorts
entirely before or after every extension of the other.

Prefix freedom and order preservation are separate obligations. Prefix freedom
makes boundaries unambiguous; it does not make a chosen encoding preserve a
pre-existing numeric or string order. If a sort promises unsigned code-unit
lexicographic order, its key encoding must preserve that order. A length prefix
placed before the contents can instead produce length-first order. Sort IDs
likewise need not appear in numeric order unless their code assignment promises it.

## 2. Byte and bit profiles

The storage policy belongs in the type:

```cpp
using bytes = everett::storage_policy<
  everett::profile_unit::byte, everett::fixed_values<8>, 15>;
using bits = everett::storage_policy<
  everett::profile_unit::bit, everett::variable_values, 7, everett::golomb<3>>;
```

`fixed_values<N>` measures `N` in the selected profile's units; zero is valid.
The same policy type belongs to the multiverse and its associated sorts,
worlds, timelines and blobs. A sort cannot silently select a conflicting unit
or value-layout policy. A variable-value policy can discover that all values
in one stream have equal width and exploit that encoding optimization; it does
not thereby make a type-level fixed-width promise. The `borrowed` stream role
retains the parent policy type while requiring zero value payload.

The **byte profile** encodes both keys and values byte at a time. Prefix
and backspace counts are in bytes, and physical offsets are in bytes. Its framed
pair encoding must admit the byte-oriented reader; the abstract bit-level
contract is not permission to feed an unaligned bit format to that reader.

The **bit profile** uses meaningful bits for both keys and values. Backspace
counts and physical offsets are in bits. The policy chooses Golomb or
exponential-Golomb encoding for the backspace count; other bit-profile counts
use order-zero exponential-Golomb. The byte profile uses canonical unsigned
base-128 varints. The chosen code and its parameter are explicit metadata,
not something readers infer from the first few bits.

A backspace count is the number of units removed from a predecessor, not the
number retained. If its length is $\ell$ units and the retained prefix is
$p$, the backspace count is $\ell-p$. It refers to the physical predecessor's
length, even when a fractional search supplies a different prefix-compatible
anchor.

The fourth policy parameter defaults to `exponential_golomb<0>`.
`golomb<M>` requires $M>0$; `exponential_golomb<Order>` accepts orders 0 through
63. Byte policies retain the default parameter and encode counts with varints.
All associated types retain the same choice as the multiverse.

For a backspace of $b$ bits, `golomb<M>` encodes the quotient $\lfloor b/M\rfloor$
as that many zero bits followed by one, then encodes $b\bmod M$ in truncated
binary. A power-of-two $M$ makes the remainder a fixed-width Rice code.
`exponential_golomb<Order>` encodes $b\mathbin{\gg}\mathrm{Order}$ with the
order-zero code, followed by the low `Order` bits. With order zero, the counts
0, 1, 2 and 3 encode as `1`, `010`, `011` and `00100`.

A fixed Golomb modulus can be compact for a concentrated backspace distribution,
but a large backspace costs a long unary quotient. Exponential-Golomb gives
logarithmic code lengths as the count grows. LPFC restart decisions and sampled
offsets use the actual encoded extent, including the selected count code.
In particular, a short key following a long predecessor can spend
$\Theta(b/M)$ bits just to backspace, even when emitted literally. Under a
Golomb policy, reconstruction work must include those codeword bits; it cannot
be bounded solely by the reconstructed key's length.
Golomb's [Run-Length Encodings](https://compression.ru/download/articles/low_entropy/golomb_1966_run-length-encodings.pdf)
develops the quotient/remainder construction for geometrically distributed lengths.

Each encoded stream must carry explicit interpretation metadata:

- Profile and format version, key/value units, and count-code parameters.
- Prefix/backspace units and physical offset units.
- Sampling group size, record count, meaningful encoded extent, bit order,
  and padding rules.
- Either a proven common fixed value width across the **whole indexed stream**,
  or a variable-value declaration.

A reader rejects mismatched units or unsupported profiles before interpreting
counts or applying offset arithmetic. Byte and bit readers are separate concrete
contracts even if they share navigation algorithms. Publication, index rebuilding,
and checkpoints retain the profile metadata with the encoded stream.

## 3. A framing illustration, not a wire format

For example, the sort codes `0`, `10`, and `11` form a prefix-free set. They can
identify three sorts under one bit profile. Suppose their logical keys are
byte strings. For a sequence of bytes, one simple key code is

```text
for each byte:  1 followed by its 8 bits, most significant bit first
end of key:    0
```

This key-code family is prefix-free and preserves unsigned-byte lexicographic
order. A proper prefix sorts first because its end marker `0` precedes the
next byte's marker `1`. The empty key is the single end marker. A byte key
under sort `10` therefore begins two bits into the logical pair encoding.

This illustration spends one marker bit per byte plus a terminator. It shows
that the required framing and ordering can coexist. Final compression, sort IDs
and record layout are still open choices. A denser codec must establish the same
properties and specify a canonical encoding for every supported key.

## 4. Bit lengths, units, and padding

A bit-string value includes its meaningful bit length. The strings `0` and
`00` are different even if a storage buffer zero-pads both to the same byte.
Unused tail bits are not part of the logical value. A codec must specify bit
order, tail handling, and the validity or canonicalization of padding before
keys are compared or hashed. A packed buffer's allocation size is not its
logical length.

For byte strings, bytes are unsigned units. Embedded zero bytes are allowed;
a terminator-based codec must encode them unambiguously rather than excluding
them from the key space.

The encoding and hashing contracts must distinguish these units from physical
storage bytes. A key of seventeen meaningful bits is not a three-byte logical
key merely because its packed representation occupies three bytes.

## 5. Front coding across sorts

The native and borrowed streams are front-coded separately, both in canonical
encoded-key order. Backspaces and suffix lengths count the shared policy's
units over the complete encoded pair. Changing sorts changes the interpretation
of a logical key, but never changes those units.

Under a bit policy, a retained prefix may end inside the sort code or inside
the encoding of a logical byte. Reconstruct those encoded bits before decoding
the pair. Under a byte policy, retained prefixes end at byte boundaries and
the pair codec must preserve that alignment. Framing contributes to the encoded
length: in the illustration above, $m$ logical bytes occupy $9m$ key-code bits,
excluding the final marker and sort code.

The conservative boundary rule also needs one declared alphabet. Its retained
prefix is a copy length, which may be shorter than an exact LCP. The known
frontier must supply those exact encoded bits, or an equivalent complete-unit
prefix under the selected policy. Counts retain that interpretation across sort
boundaries.

A common encoded-bit order preserves the interval argument used by cascading:
if $a\le b\le c$, the middle encoding shares the common prefix of the outer
two. Native/borrowed equality compares the whole pair, so a borrowed `(s,x)` is
false only when the native stream contains that same pair. The same `x` in a
different sort does not qualify.

## 6. Sampling counts entries, addressing counts profile units

The policy selects a group size $K=2^n-1$. The current generic codecs accept
$K\ge3$, including 3, 7, 15 and 31; 15 is the default. The class width is
$n=\log_2(K+1)$ bits because a full group's borrowed population ranges from
zero through $K$. `rank_groups<K>` describes the virtual interleaving at
these boundaries. `select_groups<K>` locates physical records `0, K, 2K, ...`,
plus the actual-length end sentinel.

These counts are independent of whether keys use bits or bytes. Increasing
$K$ lowers class metadata per entry and samples fewer physical offsets;
it also scans more records per projected window. The Elias–Fano sample count
shrinks; its universe remains the measured residual stream extent, which can
also change when group framing changes. Neither navigation primitive grants
permission to interpret a sort with the wrong codec. Local codec correctness
at $K=3$ is separate from the chosen redundant-level schedule's catalog-size
and merge-work bounds.

Physical addressing still needs an explicit unit:

- The byte profile uses byte-aligned records and `select_groups<K>` byte offsets.
- The bit profile packs records without inter-record padding and uses bit positions.
  For bit position $p$, the containing byte is $\lfloor p/8\rfloor$ and
  the bit offset is $p\bmod8$. A byte position alone loses information.

Elias–Fano can represent either monotone position sequence, but its universe,
fixed-stride adjustment, and readers must use the same policy unit.
For unchanged positions, expressing $U$ in bits multiplies it by eight;
that changes the Elias–Fano encoding, adding about three low bits per marked
offset in the usual space expression. Actual bit packing may also change the
layout, so the new universe must be measured rather than relabeled.

Only a genuinely uniform per-record stride can be removed by the existing
arithmetic. With fixed value width $v$ bytes, restore $iv$ to a normalized
byte offset at ordinal $i$. For bit offsets the same contribution is $8iv$.
The writer tracks whether **all** values have the same physical width,
including empty values, tombstones where supported, every represented sort,
and the final partial group. At sampled group $j$, the entry ordinal is
$Kj$; the final sentinel instead uses the actual record count. A common
fixed width of zero is valid and differs from variable width. This removes
the direct value-payload contribution. A restart rule that measures physical
encoded distance can still emit more literal keys when larger values separate
them, indirectly changing the residual key/framing extent.

If each sort has a different fixed value width, there is no single global
$v$: the initial rule treats that stream as variable-width. Correct cumulative
contributions, separate uniform-width streams, or a globally fixed descriptor
are possible later layout choices. A per-sort promise alone does not authorize
subtracting one stride from mixed-sort offsets.
Variable inline values/arrows, framing, and retained padding count toward the
variable storage extent; fixed descriptors retain their external payload pins.

Work budgets must also name their units. Charge encoded bits/bytes read or
written, code units compared, framing work, and requested output separately
where needed. $K$ candidates do not bound the lengths of their keys.

## 7. Hash policies share a composite algebra

Each sort selects its key and value hashing strategies. A strategy can receive
the complete logical key when the chosen category or value interpretation
requires it. Different sorts may use different algorithms without changing the
outer additive state fingerprint.

For the table specialization, we choose a common ring $R$ and write

$$
\phi_{s,x}(v)=h_K(s,x)\,h_{V,s,x}(v),\qquad
\phi_{s,x}(\mathrm{absent})=0.
$$

The world fingerprint is the finite sum of these potentials, and an update
from $v$ to $v'$ contributes
$\phi_{s,x}(v')-\phi_{s,x}(v)$. The [arrow design](arrows.md) states the
more general potential law and composition requirements. Absence has zero
potential for every sort; **no sort number is reserved to mean absence**.
An absent-to-absent transition does not earn deletion credit.

There are two distinct ways to combine hashing strategies:

- Hash functions can produce raw bits which are explicitly mapped into the
  common $R$, with all potential arithmetic then performed there.
- If a sort already computes potentials and deltas in its own additive group
  $R_s$, transporting them into the composite sum requires a specified
  additive homomorphism $R_s\to R$. Moving multiplication across that map
  additionally requires the relevant multiplicative compatibility.

An arbitrary cast between ring representations does not establish those laws.
In particular, choosing a different prime or binary field for each sort does
not automatically make their precomputed deltas compatible with one scalar sum.
A hash function itself need not be a homomorphism; the requirement concerns
transport of already-computed additive contributions.

Domain separation must distinguish the sort. One strategy hashes the complete
canonical pair encoding. Another supplies an explicit sort/schema domain to a
sort-specific hash over `x`. Either way, deliberately omitting sort information
would make identical components in different sorts structurally collide. Hash
any meaningful bit lengths or framing through the declared canonical codec;
allocation padding must not silently affect the result.

The initial implementation uses wrapping unsigned 64-bit arithmetic. There is
a trap worth avoiding when choosing a default hash in characteristic two:
plain addition satisfies $a+a=0$, so an unweighted sum of repeated units forgets
even multiplicities. This does not invalidate binary fields or
well-designed position-sensitive hashes. The binary-field test remains a valid
check that the additive/multiplicative interface needs no division; it does not
establish the quality of every hash construction over that field.

## 8. Pin interpretation with the data

A world must pin the sort registry and interpretation versions its blobs need.
The registry records the shared storage policy and determines sort codes,
canonical encoding and comparison, hashing strategies, and how the category
is selected from `(s,x)`.
Hash seeds or domains that affect a state fingerprint are part of that context.

A merge, borrowed index, replay, or checkpoint must retain the exact versions
under which its keys and contributions were interpreted. An unknown sort cannot
be guessed to mean bytes. Changing ordering, encoding, hashing, or category
selection requires an explicit migration with the appropriate re-encoding,
index reconstruction, and contribution recomputation. Compatibility may be
established explicitly; merely reusing the same numeric sort ID is insufficient.

## 9. Implementation boundary and checks

`profile_array<P, Role>` and `profile_view<P, Role>` implement both typed byte
and bit streams. They support fixed/variable values, actual backspace counts,
partial-prefix windows and native LPFC reconstruction. A sort registry and
final prefix-free pair encoder remain to be supplied.

Each physical group starts with one count-coded **actual predecessor key
length**, followed by up to $K$ records. A record stores its backspace count,
suffix length, a value length when values are variable, then suffix and value
payloads. The group checkpoint lets a sampled lookup recover actual predecessor
lengths while scanning at most $K-1$ headers; there is no full-key-length or
record-offset array with one machine word per key. Group checkpoints are part
of the residual Elias–Fano extent.

In the byte profile all counts and payloads use byte positions. In the bit
profile the policy selects the backspace code; other counts use order-zero
exponential-Golomb. Payloads concatenate without inter-record padding, and
physical offsets count bits. Bits are most significant
first; unused low bits of the final storage byte must be zero. Metadata records
and checks the profile, policy, role and group size. It is a logical descriptor,
not a finalized portable serialization of the header and its sections.

The native builder accepts an LPFC restart factor and emits a literal key when
the previous literal becomes too distant in encoded profile units. A partial
query reconstructs only its requested prefix; parsing its counts still costs
the selected codeword lengths. The separately supplied borrowed
prefix ceilings can shorten a copy prefix and re-emit erased units, without
changing native bytes. The decoded retained prefix is an absolute copy length;
it is not presumed to equal LCP with an incoming surrogate frontier. Values
returned by independent reconstruction are copied in full; view callbacks borrow
their encoded value span and an ephemeral key-prefix scratch buffer.

`encoded_at` parses from a bounded group checkpoint. Full traversal additionally
checks predecessor continuity, sampled group offsets and the terminal extent. The borrowed views check
metadata, section bounds, padding and parsed counts; they do not authenticate
objects or prove an arbitrary caller-supplied anchor shares the required prefix.

`multiverse<P>` supplies the current read side of the backing store: it holds
an existing object directory and opens checked `file<P>` envelopes under
canonical object-ID paths. Its `sort`, `blob` and `file` aliases retain the same
policy. Its `world`, `timeline` and `branch_point` aliases name forward-declared
aggregate types, not working persistent runtimes. `sort<P>` checks an individual
code's packing and policy alignment; it does not validate a whole prefix-free
registry. Object access validates headers by default; `file_open_mode::trusted`
defers that validation until an explicit metadata request or scan. The reader
performs no directory creation or durable writes. SQLite
integration for worlds, pins and progress remains separate implementation work.

The reference world currently takes one value type and one hashing-policy object
per instantiation. Its existing `hash.value(value)` call does not receive the
key or sort. Generic per-key value potentials and sort-dependent dispatch
therefore remain design work. Supplying caller-encoded composite bytes can
exercise the byte interface, but does not implement the broader key contract.

Codec acceptance must cover:

- Empty keys and equal components in different sorts; no cross-sort false borrow.
- Prefix-related unit sequences, exact termination, and prefix-free sort codes.
- Bit keys of every tail length, including sort/key joins inside a byte.
- Agreement between semantic comparison and canonical encoded-bit order.
- Groups and conservative prefix contexts crossing sort boundaries.
- Meaningful-length and padding rules in comparison, hashing, and checkpoints.
- Compatible and incompatible registry versions during merge and replay.
- Heterogeneous hashing strategies with a common additive contribution algebra.
- Physical byte/bit position conversion and fixed-stride accounting at partial tails.

The implementation ledger records which profiles and integration paths have
passed these requirements. The byte and bit profile tests do not establish a
working mixed-sort registry.
