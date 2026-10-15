# Immutable edited source images

`persistence/EditedSourceImage.h` implements the existing `FarEditedSourceView`
seam with owned density, material and authoritative water vectors. Its accessors
perform no file reads and retain no mutable chunks, save paths or world callbacks.
An image remains readable after later edits, eviction, transaction abort or world
closure. Generation comes from snapshot membership, never a voxel revision.

`CaptureCommittedEditedSource` recovers an interrupted transaction before
validation, verifies the requested snapshot generation and member fingerprints,
and copies relevant authoritative lod-0 records, including disk-only edits.
Unsaved live and parked values cannot substitute for committed records. Missing
expected authority refuses capture. Present malformed or future records refuse;
absence of the snapshot index adopts validated legacy authority as generation
zero. Valid new-format plant, creature and ground records without an index are
validated on their own merits for capture; an absent index alone does not reject
them. The ordinary load readers retain their existing index requirements.
Capture does not stamp the save, invent active regions or fabricate edited
content. The next successful existing snapshot transaction stamps adoption.

`CapturePreparedEditedSource` requires the open transaction on its owning thread.
It combines staged authoritative records with unchanged committed members and
retains a producer-created publication token. Prepared images can be baked but
are never renderer eligible. At durable commit the transaction compares captured
dependencies with published membership, including additions, removals and changes.
`PublishCommittedEditedSource` then creates a separate committed receipt sharing
the immutable payload. It cannot publish an aborted or subsequently changed image;
the original image permanently retains its Prepared identity. Each transaction
has 64 publication slots; expired tokens can reuse slots.

Selections contain canonical, unique tile keys and complete spacing-aligned,
closed tile supports. All five tiers are supported. Contributors intersect the
sample support or one of six axial normal supports displaced by a tier spacing.
This includes upper chunk boundaries and negative coordinates without rounding
through floating point. Every authoritative lattice has the full lod-0 shape,
finite densities and either no material channel or an equally sized channel.
Overlapping copied nodes must agree in density and material before quantization.
Interpolation, material ties, quantization and FSV1 remain the existing bake
consumer's responsibility.

Inventory entries carry chunk identity, closed footprint, member descriptor
index, contributing member generation and a digest of the authoritative record
header and compressed payload. Dependencies include verified absence and permit
consumers to reconcile before/after inventories, including removal of the last
edit. Water copies retain the existing integer state, available float mirrors,
resolution and sleep continuation; they are separate from terrain-only FSV1.
Capture reuses the strict regional water grid validator for legal resolutions
(4, 8, 16 or 32), complete array shapes, nonnegative integer depths, finite
mirrors and valid continuation counters/delta. Legacy capture and preview also
accept a complete float-only grid with all three integer arrays empty; they do
not accept partial integer storage. An optional rest-level mirror must be empty
or complete and finite.

The legacy disk decoder supports these compatibility forms: continuation fields
at the chunk's top level when `water_state` is absent; absent `water_depth_mm`
and `water_bed_mm` (the coherent float-only form); and absent `water_edge_flux`
(restored by that decoder as two zero flux entries per integer depth cell).
Capture retains these decoded representations, including empty integer arrays,
without converting float-only water into invented integer authority. A missing
material channel is also accepted. Complete integer grids with float mirrors
remain the indexed representation. No missing water data is turned into a
fabricated dry observation.

All eight authoritative singleton paths are preflighted together: world metadata,
energy, world manifest, plants, creatures, ground objects, field pages and active
regions. In addition to individual format and membership/digest checks, capture
runs the same cross-file checks as normal world loading over the installed
candidate set, including staged files during prepared capture. These check ground
ownership, allocator bounds and spawn times; ledger and energy ages; field-page,
weather and scent ownership and future ages; shared plant/creature identities,
owners and targets; and required plant soil/irrigation cells. A baseline validation
cache cannot certify a prepared candidate.

`GameSession::CaptureFarEditedSources` runs on the session owner thread, excludes
reentry and waits for streaming writers without publishing additional simulation
work. It holds the existing worldgen sampling scope through synchronous capture.
The host must call it between mutation operations on that same thread. Optional
live and parked preview inputs are copied into separate immutable layers, with
live identities taking precedence over parked identities. These volatile layers
cannot be supplied as durable bake sources. Preview composition and finite
pristine/terrain/water sampling remain the frozen-observation consumer's job.

All limits are required caller admission values, with zero admitting no work in
that category. Capture bounds tile selection, members, directory visits, records,
decoded bytes, validated samples, contributor probes and shared-node probes.
For recovery, capture first admits the journal and index lengths, then parses
only the admitted journal and stats every replace destination and referenced undo
payload before calling recovery. Their cumulative lengths must fit both file and
decode caps and a conservative 256-times scratch envelope; entry counts must fit
both member and record caps. Directory visits and rollback/hash control storage
are bounded separately. This admission applies only to capture; ordinary recovery
retains its existing behavior. Recovery and subsequent snapshot decoding are
separate admission phases.

Snapshot singleton input lengths accumulate against file/decode caps before any
validator follows sibling dependencies. The combined singleton projections use a
256-times scratch envelope plus fixed hash/ground input buffers. Region input
lengths accumulate with singleton lengths against the file cap; lod-0 raw sizes
accumulate with singleton lengths against the decode cap. Record headers, record
counts and decoded JSON scratch are admitted before decompression. Index and
header JSON reads also use conservative scratch envelopes. Terrain sample,
selection-probe and boundary-probe counters are shared across both preview
layers, so changing layers cannot reset a work cap. Ordinary persistence
reads and writes retain their existing behavior.

The shared `FarVolumeMemoryPool` is required. Each image reserves its declared
`max_image_bytes` before payload allocation and keeps that conservative reservation
for the allocation lifetime. Scratch reserves `max_scratch_bytes` while capture
runs. Previous output remains charged during replacement. Live and parked layers
each hold an image reservation; all layers share the same pool. Committed receipt
handles add a separate 512-byte control reservation while sharing payload storage.
Aliases retain the reservation and pool; every refusal preserves previous output
and releases newly acquired reservations. Callers must include these reservations
in their complete CPU/GPU lifecycle budget and choose caps appropriate to the
selection rather than assuming that bake scratch accounts for source images.

Capture is observational: it does not generate chunks, activate regions, advance
water or entities, change simulation flags or add hash inputs. Derived overlay
publication, catalog currency and renderer scheduling are separate consumers.

## Durable derived publication

`ClassifySnapshotMember` derives membership from the relative path. Existing
authoritative paths retain their class. The only derived paths are canonical
`far/v<tier>.<tx>.<tz>.lmr` names within codec bounds and `far/catalog.json`.
Index encoding and decoding reject a class that disagrees with the path. Existing
member and journal encodings retain their bytes. Derived files never enter an
authoritative fingerprint, world hash or simulation policy.

`SaveTransaction::BeginDerivedGroup` declares disjoint, bounded paths and durably
publishes undo copies and the journal before returning a handle. Derived writes
and removals require that handle; generic authoritative writers refuse derived
paths. Existing files are admitted before undo capture, and replacement candidates
before installation, using the existing 64 MiB overlay and 16 MiB catalog caps.
Admission refusal preserves authority and existing journal protection. All
transaction operations, finalizers, rollback and acknowledgements
belong to the transaction's owner thread. Workers retain immutable inputs and
receipts and perform no transaction mutation.

`WriteDerivedMember` and `RemoveDerivedMember` restore the complete optional group
on a replacement failure, including a failure reported after rename. Commit also
validates staged derived records and restores an invalid group. A restored group
keeps its previous index membership, including unreceipted absence, while staged
authority may commit with reconstruction due. Undo records remain protected until
the outer transaction finishes. A failed durable restoration poisons the outer
commit and leaves recovery protection when outer rollback also cannot finish.

`AddFinalizerOnce(key, callback)` retains the first registration for a key.
Finalizers run in registration order after authoritative staging and before index
assembly; a false result or exception aborts the transaction. An optional consumer
must return success after safely restoring failed derived work. Registration is
independent of dirty counts, destination and save entry point, so first saves,
incremental saves, metadata saves and zero-dirty retries use the same commit seam.
The consumer owns registration for its lifecycle and any capture/bake scheduling.

The index is protected even for a zero-dirty commit and is always published last.
Unchanged membership preserves its original generation; changed content and
recorded removal receive the new snapshot generation. Only the durable index
publication enables acknowledgements and prepared-source publication. An immutable
`SnapshotCommitReceipt` carries the committed generation, transaction identity and
fingerprint of the exact index bytes. `SaveTransaction::receipt()` and
`EditedSourceImage::PublicationReceipt()` share that receipt after dependency
validation. Aborts expose no receipt. Prepared images keep their original identity
even after a separate committed handle is created.

The owner-thread session open boundary recovers a pending journal before loading
and validating installed records. Recovery and transaction begin share a process
gate: recovery refuses any transaction live on another thread or its own thread.
Rollback restores the index first and retains undo payloads until all affected
directories are durable, so interruption and repeated replay are safe. Far staging
temporaries are swept alongside existing persistence temporaries during recovery.
`load_world`, `validate_save` and saved-world catalog inspection are read-only;
pending recovery and stray staging files remain intact during inspection.
`inspect_derived_member` reports `RecoveryPending` for a pending journal.

Derived inspection validates structure before membership currency. A missing file
is `Missing`; a valid file with no present index member is `Unreceipted`; differing
valid bytes are `Stale`; matching bytes are `Receipted`. That last state proves
membership only: full currency additionally requires the catalog consumer's
dependency, removal, halo and content/terrain/numeric/cave checks. Older unchanged
member/source generations remain permitted. A malformed overlay, future format
or source generation beyond an indexed snapshot refuses, even if stale or
unreceipted. Filename recognition is independent of publication eligibility: a
recognized overlay name outside codec coordinate bounds also refuses loading.
Legacy authority without an index permits validated unreceipted
overlays without treating their generation as proof of a committed snapshot.

Catalog contents remain the catalog consumer's responsibility. It supplies a
strict `MemberDigestFn` to `BeginDerivedGroup`, derived inspection/verification,
and the optional catalog-validator argument on `load_world` or `validate_save`.
A present catalog without a validator refuses; absent catalogs need no validator.
This seam does not invent a catalog format or accept an unvalidated catalog as
current. Derived inspection admits canonical paths and existing count/byte caps.

Validated legacy records can still be adopted by an explicitly opened snapshot
transaction without enabling distant simulation or creating active-region state.
Open alone does not stamp an index or fabricate optional content. Cache absence
does not invalidate authoritative membership. Cache reconstruction, inventory
reconciliation, renderer handoff and client activation remain consumer work.
