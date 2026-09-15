Immutable object sealing
========================

`object_writer<P>` writes an already encoded body under a reserved physical
`object_id`. I keep this operation separate from publishing a cola: sealing
does not reserve identities, retain inputs, adopt objects in SQLite, select a
recovery root, or authorize reclamation. CRC32C checks accidental corruption;
it does not turn an opaque object identity into a content address.

The caller supplies an existing, durably established object directory, an
object identity, and a stable `object_attempt_id`. Both identities must already
be reserved and must never identify different work. The caller also retains the
verified source inputs and owns reconciliation after an uncertain result.

```cpp
using P = diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>>;
diet::file_header<P> header{
  diet::file_kind::native_blob, encoded_body.size(), record_count, std::nullopt};

auto sealed = diet::object_writer<P>::seal(
  object_directory, reserved_object_id, reserved_attempt_id, header,
  std::span<std::byte const>(encoded_body));
```

`fridge<P>::seal_object` forwards the same operation through its existing
root; `fridge<P>::object_writer` names the policy-bound writer type.

The body can instead be a span of borrowed byte spans, including empty spans.
Their bytes must remain readable and immutable for the whole call. A retained
mapping is suitable; the writer does not allocate or reconstruct the body. It
checks the total physical extent and canonical bit-profile padding before I/O.
It does not validate kind-specific codec sections or exact index dependencies.

The receipt records the identities, final path, byte count, body CRC and barrier
used. It acknowledges successfully submitted bytes and completed OS persistence
operations. It is not `persistence_result::durable_verified`, which has broader
allocator, dependency and recovery-root obligations. An explicit later
`file<P>::scan()` checks readable envelope/body bytes; it is not evidence that
an earlier failed persistence attempt recovered.

Appending an unfinished body
---------------------------

`object_stream<P>` keeps the same private attempt open across calls. I use it
when the final extent and header are not known until construction finishes:

```cpp
diet::object_stream<P> output(object_directory, reserved_object_id,
  reserved_attempt_id, diet::file_kind::native_blob);
output.append(first_chunk);
output.append(second_chunk);
auto sealed = output.finish(completed_header);
```

Each `append` consumes its borrowed span synchronously, completes short writes,
and updates CRC32C from the same bytes. It keeps no body-sized allocation. A
chunk is a physical byte sequence; the caller retains an unfinished bit byte
until its contents are final. `finish` checks the final extent and bit padding,
then writes the envelope and performs the sealing sequence below.

For a directory whose contents become known at the end, the constructor also
accepts a reserved prefix size after the file kind. The stream initially writes
that many zero body bytes. `finish(header, directory)` replaces exactly that
prefix while the output remains private. We adjust the accumulated CRC using
`crc32c_combine` and the suffix length, so backpatching the directory does not
require reading the body again. `body_bytes()` includes the reserved prefix;
before finalization, `body_crc32c()` includes its provisional zero contents.

The stream is neither copyable nor movable. It owns its attempt and descriptors;
the borrowed-operations constructor additionally requires the supplied operation
object to outlive it. `fridge<P>::object_stream` names the same policy-bound
type. Metadata rejection before final I/O leaves the stream active. An I/O error
poisons it: later `append` and `finish` calls fail without issuing more writes.
Destruction closes handles and preserves surviving names for reconciliation.

This supports construction that pauses in the current process. It does not
reopen an interrupted append stream or make an unsealed prefix a durable
checkpoint. [Merge resumption](merge-resumption.md) describes the additional
ownership and continuation state needed for that.

Sealing sequence
----------------

We create the two shard directories from `object_path(id, kind)` as needed,
opening each relative to its parent directory handle without following a
symlink. The final object is never replaced, even if existing bytes happen to
match. The private file is created exclusively in the destination shard:

```text
ab/cd/<remaining-id>.kv
ab/cd/.<remaining-id>.kv.attempt-<attempt-id>
```

The operation then:

1. Reserves the 96-byte header and writes borrowed body chunks, completing short
   writes and writes interrupted before progress. It computes CRC32C over those
   unchanged source chunks and overwrites only the private header with the
   canonical envelope.
2. Makes the private file owner-readable and non-writable, then synchronizes
   its contents and metadata.
3. Installs the final name with `linkat`, which fails if that name already
   exists. No existing object is truncated or rewritten.
4. Synchronizes the leaf directory, first shard and supplied root, including
   ancestors that were already present.
5. Removes the private name, synchronizes its removal and synchronizes the
   object again. Finally it closes each descriptor once and returns a receipt.

Linux uses `fsync` for files and directories. macOS uses `F_FULLFSYNC` for the
file barriers, including the final barrier after directory writeback requests;
directory barriers use `fsync`. There is no silent fallback to a weaker file
barrier. Unsupported platforms, including Windows in this implementation, fail
before creating an output. Filesystems without the required hard-link or
directory-sync support return an error.

Linux explicitly requires directory synchronization in addition to file
synchronization for name persistence. [Linux fsync manual](https://man7.org/linux/man-pages/man2/fsync.2.html)
`linkat` creates a second name for an existing file without overwriting the
destination. [Linux link manual](https://man7.org/linux/man-pages/man2/link.2.html)
Apple describes `F_FULLFSYNC` as requesting a drive-buffer flush after file
synchronization. [Apple fcntl manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)

Failure and limits
------------------

An I/O failure throws `object_write_error` with both identities, both paths,
the failed operation, and the last acknowledged stage. The stage is not a claim
about what reached persistent storage: an installation can take effect and
still return an error. No subsequent write, sync, install or unlink is attempted.
Only remaining handles are closed. Surviving private and final outputs remain
available for caller-directed recovery; even a partially written private file
is retained. If cleanup fails after the private name was removed, the final
name remains.

There is no retry or adopt-existing API. A fresh process must reconcile the
reserved identities through its recovery owner. Successful cached readback,
reopening, or retrying a failed flush is not general recovery evidence. See
[the failure protocol](durability.md). Closing is never retried because an
error can occur after the descriptor was released.
[Linux close manual](https://man7.org/linux/man-pages/man2/close.2.html)

The root and shards must share one supported local filesystem/device. The root
ancestry, filesystem and concurrent users must be trusted. Directory
handles reject shard symlinks but do not prevent another actor from renaming
directories, changing mounts or modifying an existing object. Owner-only
read-only permissions are a guardrail, not an operating-system immutable-file
guarantee. The caller establishes the supplied root's own durable parent entry.

The tests inject syscall failures, partial/interrupted writes, failed-close
acknowledgments and a link failure that still installs a name. They also seal
real mmap-backed byte and bit bodies, reject name collisions and symlinks,
and check a guarded input tail. These checks establish implementation behavior
and syscall ordering on the tested host. They are not physical power-loss tests
or certification of a particular filesystem, device or storage stack.
