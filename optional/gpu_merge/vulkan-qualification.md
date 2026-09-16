Vulkan correctness qualification
================================

I ran a separate Windows Vulkan adapter on an NVIDIA GeForce RTX 4090 against
the frozen compressed-merge source `f4c26bd0b86e006e39581357afb0ee092f8da3f6`.
All **75 canonical cases passed twice in fresh processes**, for **150 successful
runs**. Each output matched the complete canonical file bytes and hash; the
input bytes and every allocation's prefix and suffix guards stayed intact.
Vulkan API and synchronization validation were enabled, with zero warnings or
errors. Source, shader, binary and fixture fingerprints stayed unchanged.

The [qualification record](results/vulkan_qualification.json) retains each case's
counts, output fingerprint and result, together with the original report and
adapter identities. The baseline uses MSVC 19.44.35228.0 and portable CRC32C.
The raw Vulkan driver version is 2559967232.

These are guarded correctness runs, not performance measurements. The adapter
stages mapped input into device memory and reads the complete output back; it
does not assume zero-copy host memory. Its eventual complete-path measurements
must include allocations, initialization, transfers, scalar waits, output
mapping and CRC. Pipeline compilation, fixture validation, fsync and catalog
publication remain outside that construction measurement.

This qualifies the earlier frozen compressed implementation. It does not
establish the performance of the current Metal optimizations on a discrete GPU,
or qualify an integrated durable Vulkan backend. The Windows adapter is not yet
part of this optional package. I keep its results separate from the
[Metal calibration](cutover-report.md).
