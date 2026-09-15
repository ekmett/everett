# Sorts and stringlike keys

Everett's logical key is a pair

\[
\kappa=(s,x),
\]

where the **sort** \(s\) chooses how to represent the stringlike component \(x\)
and which key/value hashing strategies to use. A sort can select bit strings,
byte strings, or strings of sixteen-bit code units. The category of allowed
updates may depend on the complete pair \((s,x)\), not just on the sort.

This is the sort/key design contract. Both byte-at-a-time and bit-at-a-time
profiles are implementation targets for the first package; the sixteen-bit
code-unit profile and sort registry remain design work. The existing
`front.h` prototype is byte-only. Consult the [implementation ledger](implementation.md)
for completed codec work, the [store design](design.md) for blobs, and
[per-key arrows](arrows.md) for update semantics.

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

Choose canonical bit encodings \(E_S(s)\) and \(E_s(x)\). Both the sort-code
family and each sort's key-code family are prefix-free: no complete codeword
is a proper prefix of another. Encode the pair by direct concatenation:

\[
E(s,x)=E_S(s)\,\Vert\,E_s(x).
\]

The concatenation is also prefix-free. Decoding the sort determines which
key decoder to use and where its bits begin. **There is no implicit byte
alignment between the two components.** A three-bit sort code can be followed
immediately by the first bit of a byte-oriented key code.

The canonical comparison order is lexicographic order on these encoded bits,
with \(0<1\). Prefix freedom makes every sort's range contiguous: different
sort codes differ before either code ends, so every extension of one sorts
entirely before or after every extension of the other.

Prefix freedom and order preservation are separate obligations. Prefix freedom
makes boundaries unambiguous; it does not make a chosen encoding preserve a
pre-existing numeric or string order. If a sort promises unsigned code-unit
lexicographic order, its key encoding must preserve that order. A length prefix
placed before the contents can instead produce length-first order. Sort IDs
likewise need not appear in numeric order unless their code assignment promises it.

## 2. Byte and bit profiles

The initial **byte profile** forces byte-at-a-time representation of both keys
and values. Prefix and backspace counts are in bytes, and physical offsets are
in bytes. Its framed pair encoding must admit the byte-oriented reader; the
abstract bit-level contract is not permission to feed an unaligned bit format
to that reader.

The **bit profile** uses meaningful bits for both keys and values. Backspace
counts and physical offsets are in bits. A versioned Golomb or exponential-Golomb
code encodes the backspace bit count; order-zero Exp-Golomb is sufficient for
the initial implementation. The chosen code and any parameters are metadata,
not something readers infer from the first few bits. Sixteen-bit code-unit
handling remains a later profile/codec extension.

A backspace count is the number of units removed from a predecessor, not the
number retained. If its length is \(\ell\) units and the retained prefix is
\(p\), the backspace count is \(\ell-p\). The existing `front.h` stores \(p\)
as a byte count instead. The new profiles must make the distinction explicit,
including when a fractional search supplies a different prefix-compatible
anchor from the physical predecessor.

Every encoded stream needs explicit interpretation metadata:

- Profile and format version, key/value units, and count-code parameters.
- Prefix/backspace units and physical offset units.
- Record count, meaningful encoded extent, bit order, and padding rules.
- Either a proven common fixed value width across the **whole indexed stream**,
  or a variable-value declaration.

A reader rejects mismatched units or unsupported profiles before interpreting
counts or applying offset arithmetic. Byte and bit readers are separate concrete
contracts even if they share navigation algorithms. Publication, index rebuilding,
and checkpoints retain the profile metadata with the encoded stream.

## 3. A framing illustration, not a wire format

For example, the sort codes `0`, `10`, and `11` form a prefix-free set. Suppose
they select one-bit, eight-bit, and sixteen-bit unsigned units respectively.
For a sequence of \(w\)-bit units, one simple key code is

```text
for each unit:  1 followed by its w bits, most significant bit first
end of key:    0
```

This key-code family is prefix-free and preserves unsigned-unit lexicographic
order. A proper prefix sorts first because its end marker `0` precedes the
next unit's marker `1`. The empty key is the single end marker. A byte key
under sort `10` therefore begins two bits into the logical pair encoding.

This illustration spends one marker bit per unit plus a terminator. It proves
that the required framing and ordering can coexist; it does not choose the
final compression, sort IDs, or record layout. A denser codec must establish
the same properties and specify a canonical encoding for every supported key.

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

Sixteen-bit units are unsigned code units, not necessarily Unicode characters.
No normalization, surrogate interpretation, locale collation, or case folding
is implied. If a sort requires those semantics, it must specify them explicitly.
For numerical code-unit order, most-significant-bit-first serialization can
preserve the order; dumping native little-endian words cannot. Any byte-oriented
container for the units needs a specified byte order independent of the host.

The encoding and hashing contracts must distinguish these units from physical
storage bytes. A key of seventeen meaningful bits is not a three-byte logical
key merely because its packed representation occupies three bytes.

## 5. Front coding across sorts

The native stream and the borrowed stream remain separately front-coded. Their
common comparator is the canonical encoded-key order. A key decoder must never
interpret a predecessor's retained count using the next sort's different unit
width.

There are two coherent implementation choices:

1. **Encode prefixes in a common bit alphabet.** Retained and suffix lengths
   count bits of the complete canonical pair encoding. This works across sort
   changes, including a prefix ending inside a sort code or key code unit.
   Reconstruct the encoded prefix before interpreting the selected sort.
2. **Use sort-specific unit counts.** Within one sort, count complete logical
   units according to that codec. At a sort change, reset the unit context and
   emit a self-contained sort/key record, or explicitly switch to a common
   encoded-bit prefix mode. The record or pinned codec must distinguish modes.

These are requirements for the eventual mixed-sort codec, rather than a claim
that the existing byte reader handles both conventions. With sort-specific
counts, the codec must translate a unit prefix into its encoded bit extent. Multiplication by \(w\) is insufficient when framing or escaping
adds bits. For the illustration above, a prefix of \(m\) complete units occupies
\(m(w+1)\) key-code bits, excluding the final marker and the sort code.

The conservative boundary rule also needs one declared alphabet. Its retained
prefix is a copy length, which may be shorter than an exact LCP. The known
frontier must supply those exact encoded bits, or an equivalent complete-unit
prefix under the selected codec. Resetting at a codec boundary is safe; silently
reinterpreting counts is not.

A common encoded-bit order preserves the interval argument used by cascading:
if \(a\le b\le c\), the middle encoding shares the common prefix of the outer
two. Native/borrowed equality compares the whole pair, so a borrowed `(s,x)` is
false only when the native stream contains that same pair. The same `x` in a
different sort does not qualify.

## 6. Fifteen entries still mean fifteen entries

`rank15` describes the virtual interleaving at fifteen-entry boundaries.
`select15` locates groups beginning at physical records `0, 15, 30, ...`, plus
the actual-length end sentinel. These counts are independent of whether keys
use bits, bytes, or sixteen-bit units. A virtual group may cross a sort boundary;
neither navigation primitive grants permission to use the wrong codec there.

Physical addressing still needs an explicit unit:

- If record starts remain byte-aligned, `select15` can continue to encode byte
  offsets. The sort/key boundary inside a record may nevertheless be unaligned.
  Encoded bit lengths and any padding belong to that record's framing.
- If records themselves are packed without byte alignment, locating them needs
  bit positions, or byte positions with sufficient bit-remainder metadata.
  For bit position \(p\), the containing byte is \(\lfloor p/8\rfloor\) and
  the bit offset is \(p\bmod8\). A byte position alone loses information.

Elias–Fano can represent either monotone position sequence, but its universe,
fixed-stride adjustment, and readers must use the same unit. The present
`select15`/`front.h` integration uses **byte offsets**. The bit profile instead
supplies bit positions and bit-width contributions to the integer codec.
For unchanged positions, expressing \(U\) in bits multiplies it by eight;
that changes the Elias–Fano encoding, adding about three low bits per marked
offset in the usual space expression. Actual bit packing may also change the
layout, so the new universe must be measured rather than relabeled.

Only a genuinely uniform per-record stride can be removed by the existing
arithmetic. With fixed value width \(v\) bytes, restore \(iv\) to a normalized
byte offset at ordinal \(i\). For bit offsets the same contribution is \(8iv\).
The writer tracks whether **all** values have the same physical width,
including empty values, tombstones where supported, every represented sort,
and the final partial group. At sampled group \(j\), the entry ordinal is
\(15j\); the final sentinel instead uses the actual record count. A common
fixed width of zero is valid and differs from variable width.

If each sort has a different fixed value width, there is no single global
\(v\): the initial rule treats that stream as variable-width. Correct cumulative
contributions, separate uniform-width streams, or a globally fixed descriptor
are possible later layout choices. A per-sort promise alone does not authorize
subtracting one stride from mixed-sort offsets.
Variable inline values/arrows, framing, and retained padding count toward the
variable storage extent; fixed descriptors retain their external payload pins.

Work budgets must also name their units. Charge encoded bits/bytes read or
written, code units compared, framing work, and requested output separately
where needed. Fifteen candidates do not bound the lengths of their keys.

## 7. Hash policies share a composite algebra

Each sort selects its key and value hashing strategies. A strategy can receive
the complete logical key when the chosen category or value interpretation
requires it. Different sorts may use different algorithms without changing the
outer additive state fingerprint.

For the table specialization, choose a common ring \(R\) and write

\[
\phi_{s,x}(v)=h_K(s,x)\,h_{V,s,x}(v),\qquad
\phi_{s,x}(\operatorname{absent})=0.
\]

The world fingerprint is the finite sum of these potentials, and an update
from \(v\) to \(v'\) contributes
\(\phi_{s,x}(v')-\phi_{s,x}(v)\). The [arrow design](arrows.md) states the
more general potential law and composition requirements. Absence has zero
potential for every sort; **no sort number is reserved to mean absence**.
An absent-to-absent transition does not earn deletion credit.

There are two distinct ways to combine hashing strategies:

- Hash functions can produce raw bits which are explicitly mapped into the
  common \(R\), with all potential arithmetic then performed there.
- If a sort already computes potentials and deltas in its own additive group
  \(R_s\), transporting them into the composite sum requires a specified
  additive homomorphism \(R_s\to R\). Moving multiplication across that map
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

The initial implementation uses wrapping unsigned 64-bit arithmetic. A default
hash design should avoid the literal-unit parity trap of characteristic two:
plain addition there satisfies \(a+a=0\), so an unweighted sum of repeated
units forgets even multiplicities. This does not invalidate binary fields or
well-designed position-sensitive hashes. The binary-field test remains a valid
check that the additive/multiplicative interface needs no division; it does not
establish the quality of every hash construction over that field.

## 8. Pin interpretation with the data

A world pins the sort registry and interpretation versions needed by its blobs.
The registry determines sort codes, accepted key units, canonical encoding and
comparison, hashing strategies, and how the category is selected from `(s,x)`.
Hash seeds or domains that affect a state fingerprint are part of that context.

A merge, borrowed index, replay, or checkpoint must retain the exact versions
under which its keys and contributions were interpreted. An unknown sort cannot
be guessed to mean bytes. Changing ordering, encoding, hashing, or category
selection requires an explicit migration with the appropriate re-encoding,
index reconstruction, and contribution recomputation. Compatibility may be
established explicitly; merely reusing the same numeric sort ID is insufficient.

## 9. Implementation boundary and checks

The existing `front.h`/`blob.h` prototype has `std::string`/`std::string_view`
keys, unsigned-byte comparison, byte-count prefixes and suffixes, and a
nine-byte native value slot for an optional unsigned 64-bit value. The byte
and bit profile implementation is being developed separately, including
value lengths, actual backspace counts, and profile-specific offsets. Neither
that work nor the existing prototype supplies a sort registry, sixteen-bit
key-unit codec, or the final prefix-free pair encoder.

The reference world accepts one value type and one hashing-policy object per
instantiation. Its existing `hash.value(value)` call does not receive the key
or sort. Generic per-key value potentials and sort-dependent dispatch therefore
remain design work. Supplying caller-encoded composite bytes can exercise the
byte interface, but does not implement the broader key contract.

Codec acceptance must cover:

- Empty keys and equal components in different sorts; no cross-sort false borrow.
- Prefix-related unit sequences, exact termination, and prefix-free sort codes.
- Bit keys of every tail length, including sort/key joins inside a byte.
- Sixteen-bit code-unit order independent of host byte order.
- Agreement between semantic comparison and canonical encoded-bit order.
- Groups and conservative prefix contexts crossing sort boundaries.
- Meaningful-length and padding rules in comparison, hashing, and checkpoints.
- Compatible and incompatible registry versions during merge and replay.
- Heterogeneous hashing strategies with a common additive contribution algebra.
- Physical byte/bit position conversion and fixed-stride accounting at partial tails.

These are codec acceptance requirements; consult the implementation ledger for
which profiles and integration paths have passed them. The mixed-sort registry
and sixteen-bit codec must not be inferred from byte or bit profile tests.
