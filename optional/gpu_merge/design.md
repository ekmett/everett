GPU construction experiment
===========================

This optional program merges two mapped, strictly sorted native files into a
canonical KV03 file. The GPU emits the final front-compressed payload directly.
It also selects input Elias–Fano offsets, parses input frames, resolves inherited
prefixes, and constructs output navigation. The CPU supplies constant-sized file
metadata, imports mappings, allocates zero-initialized output storage, submits work,
and writes the directory, CRC and envelope. These stages belong in the total
time. The [measurement report](report.md) distinguishes this complete path from
the earlier CPU-reconstructed staging path.

The supported specialization is `string_policy`: one occupied sort with selector
code `0`, string keys, optional string values, replacement composition,
order-zero exponential-Golomb controls, and K = W = 15. Arbitrary C++ selectors,
codecs and composition handlers do not execute on the GPU. One empty input is
supported; the current GPU entry point rejects two empty inputs. This program
does not publish catalog roots or provide durable acknowledgments.

Merge, count, scan, emit
-----------------------

We partition the stable merge by diagonal rank, binary-search each partition's
input positions, and emit short ordered runs of references. This follows the
partitioning idea in [GPU Merge Path](https://davidbader.net/publication/2012-gm-ba/)
and [Modern GPU's merge implementation](https://moderngpu.github.io/merge.html).
Equal keys put the older input first. A neighboring-key comparison and an
exclusive scan retain the final reference in each equal-key run, so the newer
value wins. A newer tombstone remains a record; dropping it would require a
separate proof that no older history remains visible.

The compacted order determines both each predecessor and each output ordinal.
We then calculate exact encoded lengths, scan them into bit offsets, and emit
the payload. No independently encoded chunk resets the FC prefix at its cut.
One GPU invocation owns each final 32-bit word, gathers its constituent bits,
and writes that word once. This avoids overlapping read-modify-write operations
where adjacent records share a word. Unused final bits are zero.

For output record $i$, let $k_i$ be its key length in bits and $l_i$ its exact
bit LCP with the preceding surviving key, with $l_0=0$. The one-bit selector is
held in the file dictionary. The control argument is

$$
c_i=\begin{cases}1+l_i & i\bmod15=0\\k_{i-1}-l_i & \text{otherwise.}\end{cases}
$$

The record contains `EG0(c_i)`, `EG0(k_i-l_i)`, the remaining key bits, and the
complete encoded value. A tombstone is `0`; a present value is `1`, its byte
length in EG0, then its bytes. For the bounded integer arguments used here,
$E(x)=2\lfloor\log_2(x+1)\rfloor+1$, so the record length is
$E(c_i)+E(k_i-l_i)+(k_i-l_i)+v_i$, where $v_i$ includes the value's own framing.
Payload bits are MSB first. There is no padding between records.

The host checks a conservative 64-bit upper bound before any 32-bit GPU scan.
The current caps are fewer than $2^{24}$ input records and a conservative output
bound below 2 Gibit; the retained staging path additionally caps its arena at
1 GiB. Fixed slack proportional to
compressed input alone would not establish these bounds. Exact length scans
also avoid a later pass to compact independently padded payload chunks.

Physical format and navigation
------------------------------

[The CPU encoder](../../include/everett/sort_profile.h) is the wire-format oracle.
KV03 has eight aligned sections: payload; EF low bits, high bits, samples and
sparse exceptions; selector dictionary; dictionary offsets; block seeds. A
single occupied selector needs no seed bits. We sample physical offsets at
ordinals 0, 15, 30, and so on, plus EOF. When every encoded value has equal
width, the EF sequence subtracts that width times the sample ordinal, matching
the CPU encoder exactly.

A block's absolute retained position and selector seed make its records
walkable, but do not reconstruct the inherited key prefix. The GPU prefix-owner
algorithm below supplies that context. Optional locality
restarts in other profile writers are not part of this output specialization.

Compressed-input path
---------------------

The complete path removes CPU key reconstruction and the CPU per-record walk.
The host supplies the existing EF sections and constant-sized layout metadata.
GPU select obtains each physical block offset, including its common-value-width
correction. One GPU invocation parses each block's at most fifteen records.
Its first absolute retained position establishes the key length state;
later controls backspace from that state. The parser skips encoded values by
their own grammar and records their original bit spans. Neither keys nor values
need to be copied to a host arena. CPU frame and full-key reconstruction remain
independent correctness oracles outside timed calls.

There is an exact way to resolve an inherited key bit. Strip the constant
selector and let $r_i$ be record $i$'s retained key length. For a valid bit
position $p$ in that key, its literal owner is

$$j=\max\{t\leq i:r_t\leq p\}.$$

If $p<r_i$, record $i$ inherits it from its predecessor. Following predecessors
therefore stops at precisely the last record whose literal begins at or before
$p$. Each inherited step guarantees that the predecessor contains that bit;
proper-prefix keys do not invalidate the argument. Each input starts with a
literal key, so $r=0$ at its first record and a query never crosses into the
other input.

A GPU minimum tree over retained lengths gives an $O(\log n)$ owner query with
$O(n)$ metadata. Start at the queried record's leaf. If it does not own the bit,
ascend until a left sibling has minimum at most $p$, then descend right-first
through children with minimum at most $p$. Unused leaves contain an infinite
sentinel. Resolving a byte from its highest bit position backward can combine
several literal fragments, so sub-byte FC retention remains exact. GPU full-key
materialization is another possible consumer of the same descriptors; it
trades repeated owner searches during comparison for expanded key storage and
a separate pass. Their relative cost needs measurement.

Output emission has a cheaper route. Consider an output key $x$ selected from
one source. Its original source predecessor $a$ still appears in the merged
set, even if a newer record replaced $a$'s value. The merged predecessor $b$
satisfies $a\leq b<x$. Lexicographic order gives
$\operatorname{LCP}(b,x)\geq\operatorname{LCP}(a,x)\geq r_x$.
For a source's first record, $r_x=0$ supplies the same conclusion. Consequently
the entire output key suffix is a subspan of the selected source record's own
literal. Emission can copy that span directly; it needs no ancestor search.
Encoded replacement values can likewise be copied from the selected record.

This argument assumes that every distinct input key survives the native merge,
including tombstone records. If a cleanup elides keys, its new predecessor can
fall before $a$ and the literal-subspan shortcut may fail. We restrict the
shortcut to the stated replacement specialization; arbitrary composition and
cleanup contracts need their own validation.

The [output EF kernels](ef_output.hlsl) remove the host boundary loop.
An EF entry derives its residual directly from the GPU
record-offset scan, with the final entry using the payload extent. Each
256-entry group computes whether its high-position span is at least 4096 bits.
A GPU scan of the resulting sparse counts assigns exception ranges. The host
needs only constant-sized counts and extents to lay out the sections. Separate
word-owned kernels write packed lows, high bits, sample pairs and sparse
positions directly to the final mapping, including canonical padding. The
[focused checks](ef_output_test.h) compare all four sections against the CPU
encoder, including repeated offsets, sub-word fields and the exact dense/sparse
threshold. The complete-path measurements include these kernels; the earlier
staging measurements retain their original CPU EF cost.

The optimized path uses the [prefix-cache helper](prefix_cache.hlsl).
It resolves eight bytes after the GPU-proven common prefix once per record and
stores two big-endian words in unused descriptor fields. Comparisons check those
words before resolving later bytes. Zero padding plus the original length keeps
proper-prefix order exact; LCP clamps the leading-equal-bit count to the shorter
key. Same-source comparisons can use ordinals because each input is strictly
sorted. This cache needs no expanded key arena. Its fill pass belongs in the
whole-path time in the [measurements](report.md), and the uncached resolver
remains an independent oracle. These whole-path comparisons do not isolate the
cache's contribution from the other emitter changes.

The separate rank construction kernels reproduce the existing packed layouts.
For rank15, each class counts one 15-occurrence group; sixteen classes occupy a
little-endian 64-bit word and every 128 classes have an exclusive checkpoint.
Classes, checkpoints and final padding are checked against CPU output. The
checked fused-origin pass described below extends that experiment; complete
fractional-index construction remains separate work.

The separate [collision experiment](collision_rank.md) uses temporary bitmap
rank to remove older native duplicates and assign survivor positions. That
scratch directory can be discarded after ordering; it is not a replacement
for the final packed rank15 sections or their CPU query implementation. GPU
construction of final rank15 and temporary collision rank serve different
stages of the pipeline.

Explicit shader capabilities
----------------------------

I make GPU support an opt-in property of a concrete sort, selector and merge
operation. [gpu_sort.h](gpu_sort.h) sketches this gate outside the installed
library: unknown types are disabled, and its sole enabled registry is the exact
bit `string_policy` with the known replacement operation. The helper is not
yet wired into runtime selection. A matching key type or `replacement=true`
alone is insufficient evidence about a user-defined callback.

A shader implementation needs an explicit contract for each of these parts:

| Part | Required agreement |
|---|---|
| Record grammar | Key framing, value framing/skip, count codes, units, optional/fixed width and block restart state |
| Order | Full logical key length, proper-prefix order, bit/byte significance and selector ordering |
| Composition | Exact older/newer argument order, key dependence, identity/tombstone handling, output-size and emission rules |
| Hashes | Named key/state hash semantics and versions, or an explicit CPU-only signature path |
| Registry | Actual prefix-free selector codes and schema interpretation, independent of C++ type names |
| Physical output | KV/IX revision, K/W, padding, navigation and descriptor ABI |

The proposed registered shader modules would implement parsing, key
comparison/LCP, composition size and emission. Built-in modules could generate
the HLSL specialization; a custom sort could supply an explicitly registered
HLSL implementation with the same contract and CPU/GPU oracle fixtures. This
would require explicit shader implementations for C++ handlers. A future
dispatcher must select the ordinary CPU implementation for unsupported sorts,
selectors, compositions, devices or input bounds before starting an output. A GPU
execution failure remains a failed private construction, not permission to
publish a partially written file.

The proposed execution policy first invokes the application's version policy to
choose a concrete sort manager. User schema identifiers need not be consecutive
and are distinct from physical format revisions. The GPU manifest names the
selected selector protocol and its exact codes; it does not replace that policy
with a hash of C++ type spelling or force unchanged sort codes to move when a
reserved registry branch is filled.

The first mixed-sort extension should reject a whole job unless every occupied
sort has a registered implementation. A later hybrid may partition at proven
sort-run boundaries, but independent chunk encoding is unsafe: W15 is global,
the retained selector may cross a cut, and the first record needs the preceding
survivor's exact selector/key context. We would dispatch a specialized kernel
over a sort run, then combine lengths in one global scan and apply the actual
transition grammar. Registry dispatch belongs around those loops rather than
an indirect C++ call for each record. No such mixed-sort path is implemented yet.

The current descriptor strips one constant selector bit; it is not a general
mixed-sort ABI. That extension must retain selector-prefix state, full logical
order lengths and each leaf's framing contract. Cross-sort order follows the
actual selector/order bits, not an arbitrary registration ordinal. Fixed-width
or raw keys need their registered grammar rather than an FC-string wrapper.

Replacement preserves encoded arrows and needs no value-hash shader. Any future
shader composition must state whether it preserves all keys. Eliminating an
identity/tombstone invalidates the direct-source-suffix lemma above; that output
must resolve inherited bits again. Noncommutative operations must retain exact
chronology, and a stateful operation needs serialized state in its artifact/job
identity. A generic C++ wrapper around replacement does not inherit eligibility
without an explicit specialization.

One source contract, separate executable artifacts
------------------------------------------------

The proposed generator records a canonical manifest containing the shader module
digests, semantic IDs/versions above, selector codes, K/W, descriptor and resource
binding layout, entry points, integer bounds and compile definitions. Its SHA-256
identifies the source specialization. DXC version, target profile/environment,
flags and the resulting SPIR-V digest identify the Vulkan artifact. Metal adds
SPIRV-Cross version/options, generated MSL digest, Metal compiler/SDK/options and
metallib digest. The backend artifacts share a source contract, not necessarily
identical machine code or capability requirements.

The current optional build already produces validated SPIR-V and derived Metal
artifacts from the same HLSL. Manifest generation and automatic specialization
selection remain proposed. Binding reflection must be checked against the host
ABI; compilation alone does not establish semantic equivalence. Backend probes
and full CPU wire-byte oracles gate enabling an artifact. Calibration chooses
among eligible implementations and cannot make an unsupported sort eligible.

Fractional-index construction
-----------------------------

An index retains its exact native, main and secondary dependencies. Its augmented
stream merges **all** native keys, every fifteenth occurrence of the main target's
local augmented stream, and every fifteenth native key of the secondary target.
The main sampler includes its own borrowed occurrences; the secondary has no
onward index route. Counts are therefore
$V=N+\lceil V_{main}/15\rceil+\lceil N_{secondary}/15\rceil$.
An absent target contributes zero. Sampling starts at ordinal zero.

The stable tie order is native, main borrow, secondary borrow, preserving order
within each input. We keep all occurrences, including repeated main samples and
borrows equal to tombstone-bearing native keys. The native merge's keep-newer
deduplication pass cannot be reused. Each borrowed occurrence retains its route
ordinal; that ordinal times fifteen names the target window.

The [index-rank pass](index_rank.hlsl) takes the completed
augmented origin stream (0/1/2) and constructs both route class sections in one
read of those origins. Each invocation owns two packed 32-bit words and counts
eight 15-occurrence groups. A second pass sums each route's 128-class blocks;
the existing GPU scans and LE64 checkpoint emitter finish the rank directories.
False borrows contribute normally to populations. This helper avoids
two full origin bitmaps and CPU class staging; it does not produce the augmented
order. [Its checks](index_rank_test.h) pass exact CPU class and checkpoint
comparisons for 39 synthetic tails/patterns and four real CPU COLA graph
fixtures, including ties; an invalid origin is rejected. This is construction
correctness evidence, not a measured whole-index speedup.

Completing GPU index reconstruction requires the following additional work:

1. Sample the exact target streams and merge those references with native keys
   under the stable three-way order. Sampling a main target requires its local
   native and two borrowed streams, not just its native file. Reuse retained
   dependencies; do not reopen the transitive graph for every pair.
2. Compute adjacent key LCPs and equal-key run heads. A borrowed occurrence is
   false exactly when its equal-key run begins with a native occurrence. This
   flag concerns key equality, independent of whether the native value is a
   tombstone. Store flags in each route's own borrowed order, LSB first.
3. Compact each route's borrowed references with scans, but never deduplicate
   them. Encode the two key-only FC streams using their separate borrowed
   ordinals and block boundaries. Sampling skips source predecessors, so a
   borrowed suffix may begin before its selected source literal. The native
   direct-literal shortcut is not valid here; use prefix-owner resolution.
4. For each augmented group start $g=15j$ and route $r$, let $p$ be the last
   occurrence of route $r$ strictly before $g$. Store zero if none exists;
   otherwise store $\operatorname{LCP}(key_p,key_g)$. Equivalently this is the
   minimum adjacent LCP over $(p,g]$, including the transition into the cut.
   A prefix-maximum scan finds the preceding borrow and range-min queries give
   the exact cut. These cut LCPs differ from both route FC predecessors and
   native codec restart positions.
5. Emit route navigation and IX03 sections, retaining the exact native/main/
   secondary identities. The host still owns reservation, sealing, barriers
   and publication. GPU completion does not itself establish durability.

The CPU terminal builder already constructs zero-population navigation from the
native count without reading keys. A GPU implementation should keep that cheap
case on the CPU unless measurements justify otherwise. Likewise, the existing
index pipeline passes bounded samples from the target outward; independently
materializing every augmented level on the GPU could lose that advantage.
Measure sampling, ordering, borrowed emission, rank/cuts, transfer and complete
index construction separately against the existing builder, including its SIMD
rank queries where the workload actually uses them. Rank15 class construction
itself is a scalar packing loop; its SIMD query helpers are a different cost.
Compare owning and preallocated class builders, and use the production SIMD
popcount helper when comparing bitmap-derived rank construction. A rank-only
test starts from the same origin or class input on both sides and cannot
establish a whole-index win.

Memory, tools and validation
---------------------------

The Metal probe imports page-rounded file mappings with
[`makeBuffer(bytesNoCopy:...)`](https://developer.apple.com/documentation/metal/mtldevice/makebuffer%28bytesnocopy%3Alength%3Aoptions%3Adeallocator%3A%29).
Mappings stay alive through command completion. The merge output is written
directly through that imported mapping; the complete path also borrows its input
mappings without a reconstructed arena. The probe tests this host's behavior rather than establishing a portable
file-mapping guarantee. [Metal storage modes](https://developer.apple.com/documentation/metal/setting-resource-storage-modes)
and completion synchronization remain part of the resource contract.

The standalone build uses HLSL 2021 → DXC SPIR-V → `spirv-val` →
SPIRV-Cross MSL → Metal compiler pipeline. Tool locations are explicit build
arguments; Everett's ordinary CMake targets gain no GPU dependency. This package
supplies the Metal host. A separate Windows Vulkan adapter has
[qualified the earlier frozen implementation](vulkan-qualification.md); it is
not included here and has no performance qualification yet. Vulkan's
[host-memory import extension](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html)
has alignment, lifetime and synchronization requirements and permits
platform-specific import rejection; a successful Metal probe does not prove
that Vulkan can import the same mapping.

[The adversarial checks](adversarial.h) compare complete output bytes with the
compressed CPU merge and serializer, perform a mapped format scan, and compare
decoded rows with an independent chronological map. They include empty and
proper-prefix keys, binary bytes, sub-byte LCPs, duplicate partition boundaries,
newer tombstones, constant-width values and skewed inputs. Two one-record
payloads also have independently derived expected bits. Benchmarks report CPU
merge and complete-file construction separately, alongside GPU command time
and total host-plus-GPU work. File creation here is not a durability benchmark.
