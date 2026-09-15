# Merge resumption

I distinguish a merge that yields its CPU budget from one that survives a
process restart. The first already works. The second needs an explicit
continuation and durable ownership of everything that continuation names.
An absolute retained position at a physical block start lets us parse its
records independently; it does not supply the inherited key bytes.

## Yielding in the current process

`native_merge_builder::step(budget)` stops between distinct output keys. We
keep the builder and call `step` again. Its source owners, cursors, comparison
frontiers, composition policy and output encoder remain live. Yielding does
not copy keys, serialize state or replay records. Moving the builder transfers
that state; it does not make it persistent.

The encoded merge keeps each current input key as a sequence of views into
immutable input literals. A retained boundary trims that sequence, and the next
literal extends it. Those views also let us validate strict input order when
an explicit restart or smaller retained prefix makes the encoded prefix less
than the actual LCP. Key-aware composition uses the reconstructing cursor
instead. Neither representation can save its process addresses in a checkpoint.

## A checkpoint boundary

I would capture a continuation only after a successful `step`, with no
composition callback or output append in progress. At that point each input
ordinal names its next unconsumed record, or EOF. The input key context belongs
to that current record. It is not the key before the cursor.

This gives us the following state to bind in one versioned continuation:

| State | What must be retained |
| --- | --- |
| Recipe | Comparator and codec versions, complete storage policy, chronological composition order and composition-state codec |
| Inputs | Exact immutable native object identities in older/newer order, record ordinals and current key contexts |
| Selection | Each head's LCP with the last emitted key, in explicitly named policy units |
| Progress | Consumed input records, emitted distinct keys and any separately accounted work |
| Output | Attempt/generation identity, verified sealed extents, encoder position, partial tail, offset-builder continuation and previous output length |
| Composition | The policy's logical state, or an explicit stateless recipe |

Swapping the two input identities changes replacement semantics. An equivalent
logical table or algebraic fingerprint is not an interchangeable physical
input: offsets and literal references belong to the exact encoded object.
The checkpoint's owner must keep those objects reachable until a later durable
checkpoint or completed publication replaces that ownership.

## Saving the input context

There are two useful representations. Both do their extra work when we capture
or restore a checkpoint, rather than on each merged record.

**Copy the current key.** Save its meaningful bit length and canonical bytes.
On restart, those bytes can form one owned context span. This costs the full
current key size at capture and restore, but it is simple and avoids a long
descriptor list. Byte policies still record their units explicitly; bit keys
must preserve their non-byte-aligned endpoint.

**Save literal references.** For each surviving span, save its bit offset and
meaningful bit length relative to the named input's FC section. Concatenating
the spans describes the current key; cumulative positions can be derived on
restore. No key bytes need copying. The descriptors must not contain pointers,
mapping addresses or native `bit_view` object bytes. The input object identity
is shared by the whole list.

The span representation is not always smaller. A chain that adds one key unit
at a time can leave one descriptor per unit. I would compare actual encoded
descriptor bytes with the full-key alternative at capture, rather than assume
that referencing input data makes checkpoint space constant.

After reopening an exact input, `encoded_at(ordinal)` locates the current frame through
the physical offset directory and at most `W - 1` preceding controls. Absolute
block-start framing avoids an earlier length checkpoint. A future cursor restore
constructor can use that frame. Restoring saved key
bytes or rebinding saved spans then supplies the context needed to compare the
next input record. EOF needs no current-key context. We do not replay the
entire consumed input prefix.

Bounds checks alone do not prove that a descriptor list is the right key.
Restore must check offset/length arithmetic, policy alignment, frame position,
total key length and every referenced extent. It must also establish that the
checkpoint itself is the verified continuation of this exact input and recipe.
A valid range could otherwise name value bytes or another key's literal.
Without trusted checkpoint integrity, we must independently reconstruct or
revalidate its provenance; absolute block controls do not make that validation
free or bound it to `W` records.

## The output continuation

The current merge's internal `native_output` keeps encoded bytes and sampled
offsets in memory. `finish` constructs Elias–Fano metadata and returns an owning
array. It has no detach/restore interface. Copying that entire object would cost
space proportional to output already produced, so it is not the compact
checkpoint we want.

A resumable output sink needs an explicit boundary between verified immutable
output and an unfinished tail. Its continuation must preserve:

- Meaningful output extent and emitted record count, including the position
  within the physical `W`-record block.
- The previous output key length, common-value-width choice, and partial byte
  when using bit framing. The current encoded merge does not need a third full
  key if its two comparison frontiers are saved exactly.
- Sampled residual offsets already produced, plus whatever state an
  incremental offset encoder requires. Final Elias–Fano output is not yet a
  resumable builder interface.
- CRC/integrity state and the exact byte range it covers. An unfinished bit
  tail must not be confused with canonical padding in a completed object.
- Exact sealed extents and their independently verified lengths/digests,
  together with the current output attempt and failure generation.

`object_writer::seal` currently accepts a complete body with a known extent,
writes its envelope and requests persistence barriers. It does not reopen an
unfinished append stream. A future sink must avoid rewriting a sealed physical
update unit merely to finish a partial byte or page. It can keep that tail
private or continue in fresh immutable storage, with the associated framing
and ownership recorded explicitly.

After an uncertain write, a later successful sync does not certify the failed
generation. Recovery must verify the selected checkpoint and sealed extents,
retain uncertain objects, and continue under the failure protocol with a fresh
output generation where required. See [object sealing](object-writer.md) and
the [catalog checkpoint contract](catalog.md#7-checkpoints-and-rebuilding-progress).

## Composition and publication

Default replacement is stateless. A custom `Compose` may have mutable state,
captured references or external effects. A restart recipe needs a defined
encoding of its logical state and a rule for replaying callbacks; copying a
C++ callable or recording its address is insufficient. Checkpoint boundaries
avoid suspended callbacks, but they do not by themselves give external effects
exactly-once semantics.

A builder poisoned by a failed step is not a new checkpoint candidate. Recovery
starts from the last independently verified continuation or the original inputs.

The existing `merge_publication` and `merge_checkpoint` types in `durability.h`
are protocol models. Their opaque strings are not a serialized continuation
of `native_merge_builder`. Likewise, the implemented SQLite catalog retains
objects, pairs, attempts, saves and reader ownership; its proposed job and
checkpoint tables are not implemented yet.

I would therefore implement process restart together with the output-sink
continuation and catalog job transaction. The transaction can name already
verified extents, retain exact inputs and atomically select a checkpoint.
A source-only bookmark is useful internally, but publishing it as merge
resumption would leave most of the contract unresolved.
