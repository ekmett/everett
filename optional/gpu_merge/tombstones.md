Conservative tombstones and complete-coverage cleanup
=====================================================

I keep this path separately selectable with the `tombstone` correctness mode.
It uses the explicit bit string/optional-string grammar and compressed mmap
inputs. It does not change the calibrated modes, implement durable publication,
or support arbitrary sort handlers. The historical calibrated modes do not
preserve this new cap contract; their calibration must not select them for a
cap-aware job. No performance result is claimed here.

A replacement tombstone preserves its source retained depth. When equal keys
occur in both inputs and the newer value is absent, the output limit is the
smaller source depth. The output retains the minimum of that limit and its
natural predecessor LCP. These shader positions exclude the constant selector
bit; the CPU record API expresses its limit in complete logical-key bits.
For example, a tombstone capped at five key bits stays capped at five even if
the new predecessor would permit seven. Its literal repeats the extra bits.

The GPU records a key donor separately from the value winner. An equal-key
donor with the smaller retained depth contains the complete suffix needed by
that conservative tombstone. The newest record still supplies the encoded
value. The metadata pass does not reconstruct keys or copy their bytes.

Cleanup authority
-----------------

`merge_coverage::preserve_tombstones` is the default. Elision requires the caller
to select `merge_coverage::complete_older_history` explicitly: the inputs must
cover every older contribution relevant to these keys. Two arbitrary files do
not establish that fact. This standalone program has no catalog integration
that could authorize cleanup on behalf of a running session.

With that authority, the GPU excludes absent winners, scans survivor positions
and recomputes adjacent LCPs, global W15 controls and exact output lengths. If
every key disappears, the CPU emits the constant canonical empty-file framing;
there is no per-record CPU preparation or reconstruction in that branch.

Reading newly exposed inherited bits
------------------------------------

Deleting a predecessor can expose key bits before a survivor's original literal.
The final word writer resolves these bits against the **original source**
retention tree. For the highest remaining bit, the latest owning frame defines
an interval beginning at that frame's retained depth. The interval stops at
the already established upper ownership boundary, even if the physical literal
continues further. The writer handles at most 32 nonempty intervals per output
word segment. It performs metadata tree lookups, not encoded predecessor-frame
walks or full-key materialization. Long adjacent deletion runs may need several
different owners; no constant-work independent-chunk claim follows.

A canceled older owner redirects to its equal-key tombstone's final physical
key donor. For certified input with `r_tombstone <= r_target`, this reads the
newer tombstone literal. Legacy input with the opposite inequality instead uses
the older literal as the conservative virtual donor. That fallback remains
distinct from certified input: it needs the original older mapping pinned.
The lookup follows exactly one cancellation link and one physical donor link;
it never recursively follows an older-to-newer-to-older alias cycle. Value
addresses are never redirected.

The original prefix-owner tree remains available for comparison and LCP. Both
input mappings and the metadata buffers live through GPU completion. Inputs
still obey the prototype's existing literal-first-file contract; this work does
not add externally anchored file starts.

Correctness corpus
------------------

The focused corpus has 24 fixtures, each run with certified and legacy inputs
and both preservation and authorized cleanup: 96 complete-file comparisons.
It covers the five-to-seven cap, a two-owner subbyte output word, long adjacent
deletion ramps, W15 boundaries, mmap-page crossings, source-specific retention,
tombstone resurrection, an empty input and all-deleted output. An independent
logical map and CPU encoder check complete canonical bytes, decoded rows,
navigation and checksums. Preserving outputs also match the production CPU
merger with an explicit unary tombstone predicate; its materialized-key count
stays zero. These are correctness checks, not benchmark samples.

Run the optional CMake build and serial CTest suite as described in
[the experiment guide](design.md). The test is named `gpu_tombstone_cleanup`.
