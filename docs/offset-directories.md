Per-file offset directories
==========================

Design proposal; the production formats currently store Elias–Fano offsets.
I want the representation to be a choice for each file, independent of its sort
registry. I have not selected a replacement default. The next comparison must
qualify runtime format selection against the complete-query measurements below.

Why make this a file choice?
---------------------------

The [same-offset measurements](../optional/select_compare/results/2026-09-16-m2max/report.md)
compare EF, direct arrays and packed absolute positions. Direct loads are much
faster in that isolated operation. The extra bytes can still be a small part of
a native file because we store one offset per physical codec block, not one per
record. Those measurements do not establish the best complete-file choice or
the resulting query speed.

The [whole-query comparison](../optional/search_compare/report.md) measures
mapped native/index chains with identical record streams. Packed absolute
positions retain most of direct offsets' measured throughput gain for much
less complete-file growth. The geometric-size panel also increases the query
working set. These experiments specialize each executable to one representation;
the eventual per-file dispatch still needs measurement.

The same schema should be able to read a small file with direct offsets and a
large file with EF offsets. A native file and its fractional index can make
different choices. The two borrowed streams inside a two-target index can also
choose independently. None of these choices changes the logical keys, values,
sort codes, fingerprints, or the exact downstream files an index pins.

The sequence stays the same
--------------------------

Let a stream contain $n$ records with physical codec block width $W$. It stores
$m=\lceil n/W\rceil+1$ sampled positions, including EOF. The last position uses
the actual record count, even when the last block is short. An empty stream
still has one EOF position.

If every value has fixed width $w$, the stored position at ordinal $i$ is the
residual $r_i=o_i-iw$, where $o_i$ is its physical position. The offset codec
selects $r_i$; the profile adds $iw$ back. This arithmetic stays outside the
offset codec. The terminal residual is its inclusive universe $U$.

The units come from the file's existing profile: bits for a bit stream, bytes
for a byte stream. Thus direct32 is eligible when $U\leq2^{32}-1$, not merely
when the complete file is smaller than four gigabytes. Conversely, a large
fixed-value file can have residuals small enough for direct32. A bit universe
reaches the same numeric limit at one eighth the byte extent.

Repeated residuals and all-zero sequences remain valid. The offset codec has
no special knowledge of 15:1 virtual sampling; $W$ and the virtual sampling
interval $K$ remain separate choices.

Representations and API
-----------------------

I would start with four concrete representations:

| Kind | Stored positions | Eligibility |
| --- | --- | --- |
| `elias_fano` | Existing low bits, high bits, samples and sparse exceptions | Existing EF admission bounds |
| `direct32` | Little-endian unsigned 32-bit residuals | $U\leq2^{32}-1$ |
| `direct64` | Little-endian unsigned 64-bit residuals | Supported file extents |
| `packed` | Absolute residuals at width `bit_width(U)` | Width 0–64, with explicit handling of both endpoints |

These names and signatures are proposed, not existing APIs:

```cpp
enum class offset_format { elias_fano, direct32, direct64, packed };

struct offset_view {
  std::uint64_t size() const;
  std::uint64_t universe() const;
  std::uint64_t select(std::uint64_t ordinal) const;
  offset_cursor cursor(std::uint64_t ordinal = 0) const;
};

struct offset_cursor {
  bool done() const;
  std::uint64_t ordinal() const;
  std::uint64_t next();
};
```

The view is a small tagged, non-owning value over mapped sections. The cursor
retains the existing EF forward-decoding state when appropriate; direct and
packed cursors advance an ordinal. A cursor can start at a selected ordinal for
range positioning. Copies retain independent cursor positions.
There is no virtual allocation or registry-specific offset decoder.

An owning directory builder accepts a monotone residual sequence and a format
choice. The writer's preference is separate from the storage policy and the
schema identity. A file records the resolved representation, not a mutable
machine-dependent “automatic” hint. Explicit direct32 requests must reject an
ineligible universe; an automatic chooser may select direct64 or EF instead.

Tags, versions and persistence
------------------------------

The body directory needs an explicit offset-format tag and its parameter, such
as the packed width. EF's current low-width field is not a substitute for a
format tag. Reinterpreting “EF with width 32” as direct32 would misdescribe the
file and invalidate existing validation.

I would reserve tag zero for the existing EF layout, allocate distinct tags for
the alternatives, and introduce a recognized body-directory revision for the
tagged layout. Readers can continue accepting existing EF revisions as tag zero.
Unknown revisions, tags and parameters fail before interpreting offset bytes.
This is a physical-format extension, not a schema migration; the outer sort and
value-policy identity remains unchanged. The `.kv` and `.index` extensions do
not change. A different internal EF select layout would likewise need its own
recognized representation version.

The existing directories have room for small tags without another mapped page:

| Current grammar | Offset parameter | Available tag bytes |
| --- | --- | --- |
| Raw native KV02 / single-target IX02 | Byte 32 | Byte 34 onward; byte 33 is the IX02 target flag |
| Sort-owned KV03 | Byte 48 | Byte 49 onward |
| Two-target IX03 | Bytes 80–81 | Bytes 84–85, one per borrowed stream |

These locations describe the current layouts, not an assigned new wire format.
The new revision must preserve which native grammar the directory identifies;
raw and sort-owned records must not acquire an ambiguous shared magic.

A minimal layout can retain the four existing offset-section descriptors. EF
uses all four; direct and packed formats use the first for positions and encode
the other three as empty. Direct32 contains exactly $4m$ position bytes; any
following alignment gap is separate padding. Its final odd entry needs a
bounded 32-bit load, not an unconditional 64-bit load. Packed words use a
specified bit order and zero unused tail bits.

New merges may choose a different representation from either input. Existing
snapshots continue pinning the original files. Re-encoding a directory produces
a new physical object; it does not silently replace a pinned file. Native-merge
reuse must distinguish a preferred format from a hard requested format: an
equivalent existing file is sufficient for the former, while the latter needs
a matching recipe or explicit re-encoding.

Validation and sequential traversal
-----------------------------------

Opening remains metadata-only. It checks the tag, parameter range, section
extents, inferred sample count, fixed-value stride and residual universe without
scanning every position. Query-time selection checks its ordinal and decoded
value; subsequent record parsing retains its existing extent checks.

An explicit recovery scan checks monotonicity, the initial zero and terminal
residual, canonical padding, and agreement with positions reconstructed from
the actual record stream. Today `scan_profile` rebuilds EF and compares its
sections. That becomes a comparison with the selected representation's canonical
encoding; it must not reject a valid direct directory merely because it is not
EF. Duplicate residuals are permitted, so validation is nondecreasing rather
than strictly increasing.

Sequential cursors must retain forward traversal. Replacing every cursor step
with random EF `select` would lose an existing optimization while adding the
new formats. Construction and iteration remain bounded for malformed files;
packed widths 0 and 64 must never become shifts by 64.

Implementation boundary
-----------------------

The work crosses both owned and mapped representations:

- `profile.h` and `sort_profile.h`: offset views and forward cursors.
- `sections.h`, `sort_profile_file.h` and `cola_sections.h`: native and borrowed
  directory tags, serialization, metadata admission and recovery scans.
- `profile_file_output.h` and `cola_file_index.h`: streaming output and exact
  section lengths. In-memory serialization alone does not cover these writers.
- Native/profile builders: selection from the complete residual sequence, once
  the final universe is known.
- Adaptive output accounting: charge the actual retained representation and
  any temporary conversion buffers, rather than summing EF members unconditionally.

A first read-path implementation could keep in-memory builders producing EF and
convert only when sealing a file. That is a bounded implementation step, but its
construction measurement must include building EF and converting it. The full
design builds the selected representation directly.

What decides the default?
------------------------

For each identical logical fixture and physical record layout, I want both

$$
100\frac{B_{\mathrm{alternative}}-B_{\mathrm{EF}}}{B_{\mathrm{EF}}}
\qquad\text{and}\qquad
\frac{t_{\mathrm{EF}}}{t_{\mathrm{alternative}}}.
$$

Here $B$ is the complete native or index file, including descriptors, padding
and checksums; $t$ is a complete query, including routing, key comparison and
the same value handling. Report native and borrowed directories separately,
then the complete pinned chain, counting each shared file once. Directory-only
percentages use a different denominator and remain useful diagnostics.

The size grid needs small directories, cache-sized directories and larger
resident chains, byte and bit units, fixed and variable values, hit and miss
queries, and independent versus dependent requests. It must expose direct32's
universe limit rather than quietly dropping the cases that require direct64.
Construction cost and sequential traversal also belong in the comparison.

The existing offset benchmark motivates the alternatives. It does not yet
choose a universal size threshold, default codec, or complete-query tradeoff.
