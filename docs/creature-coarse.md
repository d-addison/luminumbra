# Integer coarse creatures

The coarse kernel and region streams implement algorithm version 1 of the
adopted distant policy. They are pure functions; callers supply integer
projections of the one authoritative registry and publish the result only after
the complete pass succeeds. There is no separate mutable coarse store and no
creature ResumeHook. `CreatureCoarseComponent` and `CreatureRegionComponent`
already persist every counter, position, owner, pending destination and tick
stamp used here. This package adds no save block or required field. Existing
absent-block adoption and present-malformed refusal rules remain unchanged.

`StepCoarseCreatures` accepts a sealed previous snapshot, the scheduler's due
owners, a separate region-record map, world seed and absolute tick. The map supplies
existence, frozen state, cadence shift and phase independently of due eligibility.
Due owners must exist in that map, must not be frozen, and must match the tick's
cadence phase. Invalid keys, states, shifts above 16 or phases outside the cadence
period refuse. An absent owner is never activated by travel. Inputs may be unordered.
The result orders creatures and deaths by PersistentId, births by mother ID,
and work by region key. Duplicate IDs and invalid integer domains refuse without
mutating the input. IDs use the shared allocator domain `0 < id < 2^63`.

## Units and counters

Positions are signed millimetres. Horizontal positions are in
`[-16777216000,16777216000)` and vertical positions in
`[-1048576000,1048576000)`. Ownership uses mathematical floor division by
512,000, including negative coordinates. Needs are integer pressures in
`[0,1000000]` ppm. Admission bridges use round-half-away-from-zero when converting
metres or normalized full-AI floats to these integers; bridges must check the
rounded result against these domains. Converting back divides by 1,000 or
1,000,000 respectively. Floats, runtime avatar state and tier transitions belong
to the session adapter, outside the integer kernel. Unrelated full-AI floats
retain their exact persisted bits while the coarse tier runs.

The session validates Full transforms against the same horizontal and vertical
domains as persistence before publishing kinematic, controller or restored
positions. A rounded horizontal bridge that reaches the next owner while its
Full float is still inside the previous owner parks at that owner's last
millimetre. Admission, demotion and movement use this same owner-preserving
quantization; rounding does not authorize a transfer. Stationary Full positions
retain their exact bits. Traversal skips zero-delta axes and uses only finite,
bounded crossing fractions when parking at an absent or Frozen owner. Proposals
outside the persisted domain are refused and controllers return to the valid
pre-step position before continuation is synchronized.

One living coarse creature whose previous owner is due and whose `region_stamp`
is less than the absolute tick accepts a cadence only if it has not already
stepped in the current owner's absolute cadence window. For shift `s`, the window
number is `tick >> s`, independent of the owner's phase. Compare the persisted
last tick as `region_stamp >> s`; zero denotes no completed step and admits the
first due step after tick zero. Tick zero itself never advances. This preserves
the existing absolute tick stamp and requires no new save fields. It stamps the tick,
increments `age_cadences` and `reproduce_cadences` with u64 saturation, and
decrements a nonzero reproduction cooldown by one. The elapsed absolute tick
difference and region cadence shift never multiply these counters. Full, dead,
off-due and already-stamped creatures retain all their state and cost zero.
Frozen time contributes zero, including on the first step after thaw.

For example, shift-1 owners `(0,0)` and `(1,0)` have phases 1 and 0. A creature
stepped in `(0,0)` at tick 1 may transfer immediately and step in `(1,0)` at tick 2,
which begins a new `[2,3]` window. In the reverse direction, a tick-2 transfer
suppresses the destination's tick-3 step in that same window, then accepts its
tick-5 step in `[4,5]`. A suppressed step leaves the stamp unchanged, so it cannot
suppress the next window. Repeating a pass at the same tick also costs zero.
If the owner cadence changes, compare the last absolute tick using the current
owner's shift; never extrapolate a future stamp or accumulate missed windows.

The adopted `coarse_need_rate_ppm` is added once to hunger, thirst and each
applicable named coarse pressure, saturating at 1,000,000. Addition subtracts
remaining capacity before adding, so even a maximum u32 policy rate is safe.
The full-AI need rates and unrelated fields are not inferred or advanced.
Zero need rate leaves pressures unchanged. Zero lifespan disables coarse death;
otherwise reaching the configured cadence age stages one death and suppresses
mating and travel. The adapter records that death in the persisted lifecycle
components and routes decay through one tier only.

Coarse counters and full-AI tick counters have independent units and survive
tier changes independently; they must never be reinterpreted by multiplying or
dividing by the current cadence shift. Admission/promotion bridges project
position and needs using the rounding rule above. The pure kernel neither ages
full-AI tick counters nor chooses promotion. Zero promotion distance disables
promotion in the session adapter, and zero travel rate leaves pending travel
intact. Conservative compiled policy defaults remain those defined in
`DistantPolicy.h`.

## Reproduction

Reproduction is sexual, same-species and genome-gated. Version 1 requires a live
non-predator with age at least 90 coarse cadences, stamina at least 500,000 ppm,
thirst at most 300,000 ppm, hunger at most its persisted genome threshold
projected to ppm, zero previous cooldown, and `reproduce_cadences` at least the
nonzero species policy period. Zero policy period disables reproduction.

Females in PersistentId order select the nearest unused eligible male of the
same species and previous owner within an inclusive 2,600 mm horizontal radius.
Distance ties select the lower PersistentId. Both partners must accept a cadence
and survive this step's lifespan check. Selection otherwise reads previous state,
before needs and movement. Distance components are bounded by the radius before
squaring, avoiding overflow. A male participates in at most one pair per cadence.

Three consecutive eligible courtship cadences complete a pair. The female's
stored `courting_cadences` counts proximity to an eligible mate, rather than a
specific partner; it resets on an accepted cadence without a pair. Each completed
pair stages one child, resets both reproduction/courtship counters, and sets each
parent's cooldown to its species period. Generations saturate at u32 maximum.
Births carry both persistent parent IDs, a seed, species, sex, generation and
the mother's previous position/owner. The adapter blends both persisted genomes
from the seed and allocates an ID during admission. Newborns never enter the
current pass. Failed ordinary birth admission retains the shared allocator's
established failed-spawn consumption rule.

## Travel and work

Travel uses the configured metres per accepted cadence, checking multiplication
by 1,000 before computing millimetres. It chooses one of east, west, north, south
from the travel draw's low two bits, or follows the persisted pending destination
(X first if both coordinates differ). Every intervening owner must exist in the
separate region-record map and must be non-frozen; it need not be due.
Movement is bounded by remaining world space before addition/subtraction.

The first absent or frozen destination seals its border. Positive travel
parks at `boundary - 1 mm`; negative travel parks at `boundary`. The creature
retains the last admitted owner and stores the blocked adjacent destination.
Later due steps retry that direction from the parked position using the current
per-cadence travel allowance; no unspent distance or frozen time accumulates.
The outer world boundary is sealed without creating an invalid pending key.
Transfers publish after the complete pass, and the persisted tick stamp prevents
a destination pass from updating the same creature again at that tick or at a
later phase within the same cadence window. Work remains charged only to the
previous owner that accepted the cadence, even when travel crosses multiple
available off-due owners.

The result charges one `entity_updates` unit per accepted cadence to the
previous owner. Births and actual removals are proposals here; the session adds
their work only after successful admission/removal and publishes completed work
after successful event drain. Uncommitted or rolled-back work costs zero.

## RegionRng version 1

Let `M` be `DeterministicRng::mix`, the existing splitmix64 integer recipe.
All operations wrap modulo 2^64. For world seed `s`, tag `t`, signed region
coordinates `x,z`, absolute tick `a`, PersistentId `p` and draw index `i`:

1. `h = M(s + t)`
2. `h = M(h XOR 1)` (recipe version)
3. `h = M(h XOR u32(x))`, then `h = M(h XOR u32(z))`
4. `h = M(h XOR a)`, then `h = M(h XOR p)`
5. `draw(i) = M(h XOR i)`

Signed coordinates convert modulo 2^32 before widening, rather than sign
extension. Tags are needs 41, travel 42, population 43, reproduction 44.
Needs currently follow the exact constant-rate equation without stochastic
variation. Travel uses draw 0. Reproduction uses the mother's previous owner,
mother ID and tick with the father ID as draw index; that draw is the genome
seed. The child's sex is female iff `M(seed) & 1` is zero. Population streams
remain separate even for an empty roster. Indexed draws have no mutable cursor.
Known-answer tests include negative coordinates and unsigned wraparound.

## Enabled ecology hash

`ComputeEcologySubHash(registry, true)` deliberately replaces the old partial
projection with `StableChecksum("ecology:creatures:v3:" +
SerializeCreatureSnapshot(BuildCreatureSnapshot(registry)))`. This includes the
complete versioned component inventory exactly once, sorted by PersistentId,
including optional presence, float bits, controller participation, plans,
references, coarse counters and pending transfers. References use persistent
identity, so allocation order, EnTT generations and storage iteration do not
affect the enabled hash. Invalid authority refuses through the same projection
used by persistence. An empty creature roster remains the empty string; content
identity is hashed by the existing ledger fold even with no creatures.

The default/false overload retains the legacy v2 byte recipe. Shared entity
identity consults plant or creature PersistentId only when enabled. These helpers
add no flag-off save, hash or artifact changes. Session and host integration must
pass the active-regions flag explicitly when choosing the enabled ecology
projection. Network/checkpoint layouts and hash composition slots do not change.


## Creature AI and lifecycle integration

The session seals `CreatureTickAdmission` after scheduling and before any D
consumer runs. It retains pre-update owners, tiers and PersistentIds until the
real tick finishes. Full AI admits only resolved durable Full creatures in due
Active owners with an earlier completion stamp. Source filtering happens before
snapshots and indexes, including prey, mates, flock members, pack members,
carcasses and water holes. Geometric comparisons retain precedence, with shared
identity resolving ties and ordering enabled accumulation. Empty gates preserve
the legacy ordering, arithmetic, RNG stream and direct birth recipe.

D owns brain and kinematic brain integration, both mating phases, steering and
survival blending, thirst, scavenging, alarm, decomposition, circadian,
territory, pack, migration, creature lifespan and the creature side of grazing.
E now gates instinct, perception, scent and emitters through the same sealed
admission. The session owns tier choice, the living coarse adapter, movement boundary
admission and controller physics. D-only session regressions omit E-owned
participants; E adds the complete persisted AI inventory regression without runtime
physics controllers. The pure C kernel remains unchanged.

Full genome ages advance only in mating resolve. Full mortal ages advance once
in the real-tick lifespan lane, after foliage and before alarm/decomposition.
An eaten creature synchronizes an existing Mortal death flag without adding age.
Coarse living counters remain exclusively kernel-owned. Coarse corpse admission
uses C's current-owner absolute cadence-window comparison; decay accepts one
operation, then completion stamps the creature only after the remaining systems
and event drain finish. Frozen time and plant resume windows add no creature
age, decay or pressure. A newly dead coarse creature enters the corpse cohort
only on a later accepted cadence.

Plant historical substeps retain their existing transaction and regrowth path.
Creature trampling and feeding run only on the final real-tick substep through a
separate source gate. Staging retains creature Region and Coarse metadata, so
eligibility and persistent ordering survive the temporary registry. The narrow
S7 species-zero Creature plus Transform remains an unadmitted positional grazer
when its existing non-frozen source owner is due. It receives no species or ID;
complete enabled saving and hashing still require legitimate durable admission.

The foliage recipient gate also enforces sealed Full admission for creatures
carrying optional Grazeable or Combustible components. Their regrowth, grazing,
fire ignition and burning-source participation run only on the final real-tick
substep, with creature completion bookkeeping. Coarse creatures cannot enter
these lanes through positional eligibility. Full Grazeable recipients retain
their ordinary grazing/regrowth behavior; the transient exception admits only
grazer sources. Pollination, disease and crops require plant-only inventory;
plant lifespan explicitly excludes creatures. The shared foliage guard protects
all these recipient lanes without changing plant resume substeps.

Enabled full mating collects proposals in mother PersistentId order and seeds
breeding from the reproduction RegionRng domain, preserving the Full breeding,
sex and sensory draw sequence. The session rejects absent, Frozen, Reduced or
off-due Full birth destinations before ID allocation. Subsequent admission
failure consumes the allocated ID. Parent courtship resets and cooldowns publish
only after success. Newborns carry Region, Coarse and explicit kinematic
Controller metadata, are stamped at admission, and cannot join the sealed birth
tick cohort. No additional persistence block or format version is introduced;
previous-release absence adoption and present-malformed refusal remain intact.

The independent test oracle freezes the pre-D implementations and legacy ecology
projection in test-only namespaces. Its nonempty trajectory includes birth,
predation, thirst, scavenging, alarm, decay, circadian, territory, pack, migration
and entity generation reuse. The session reference supplies explicit fixed legacy
metadata, the complete expected relative-path/byte map and regular-file count,
plus deterministic artifact and ecology bytes. It does not generate its expected
trajectory by calling the modified D entry points.


## Instinct, perception and field integration

Every E entry point appends optional source admission and completed-work callbacks.
Empty callbacks retain the legacy ordering, arithmetic, entity IDs and field
recipes. The enabled session passes D's sealed Full cohort to every actor and
recipient lane, including creatures carrying Opportunity, Sensable, FoodSource
or FieldEmitter components. A creature cannot acquire positional permission
through one of those optional components. Full creatures in Reduced owners,
even when due, and Coarse creatures in Active owners remain excluded.

Instinct gates before pressure growth, stimulus application, planning or optional
ActionPlan creation. Opportunities are filtered before either inline or substrate
gather and ordered by authored ID then shared PersistentId. The runtime
entity-valued destination is the opportunity carrier; Opportunity.target remains
authored text. Duplicate authored IDs resolve the winning score before applying
the identity tie. Locomotion filters actors and neighbors before snapshotting,
and checks the live action target before path or action progression. An admitted
actor holds with zero intent when its target is blocked, retaining the action,
elapsed action time and waypoint index for thaw. Blocked actors retain their
intent bits and optional presence. Perception filters both sides before snapshot;
admitted perceivers decay awareness normally when no signal remains.

Scent deposition requires the sealed source and its existing destination-cell
gate. Steering requires the Full cohort. Foraging filters food before nearest
selection, orders equal food choices by shared identity and rechecks food before
depletion. Source cell, chosen destination and common-phase border admission
precede enabled deposits, movement, pickups and delivery counters. A sealed move
holds the complete continuation rather than selecting a different step. Generic
grid-only foragers and foods seal their source owners without fabricating creature
metadata; completed work remains charged to the sealed source owner after a move.

Energy emitters filter sources before collection and each destination cell before
queuing bounded regional deposits, using shared PersistentId as deposit identity.
The existing radius ceiling remains 31. Absent, Frozen and off-due destinations
receive no queued deposits and do not activate. Regional energy ages, page
persistence, scent continuation and common-phase diffusion borders retain their
existing authority. Stored scent independently schedules continuation after the
last emitter freezes; freezing an emitter neither deletes its trail nor authorizes
new deposits. Frozen scent cells remain bit-exact.

No persistence block, algorithm version, RNG cursor, hash slot or wire layout is
added. Previous-release absence adoption and present-malformed refusal remain
unchanged. The independent legacy oracle now also freezes the pre-E entry points,
projects every secondary component explicitly and checks a nonempty trajectory
against exact scent/energy bytes. Session regressions enumerate the complete
flag-off save path/byte map and file count, world hash and artifact bytes; enabled
regressions cover complete blocked inventory, real recipient wiring, restored
persistent references and scent/energy continuation. Native execution of these
regressions remains a separate validation step.


The enabled session prepares durable consumers, schedules regions, reconciles
creature tiers and runtime controllers, then seals the real-tick cohorts. One
integer kernel pass reads the previous owners and stamps. Births are admitted in
mother identity order after that read pass; ordinary failed admission consumes an
already allocated identity. Failed birth preparation destroys staged children
before parental coarse results publish. Transfers and deaths publish from the
complete result. An existing Mortal is marked dead without adding optional
lifecycle components. Newly dead coarse creatures first decay on a later accepted
corpse cadence. Full AI retains its established order, with selected creature
physics in the movement lane. Final creature stamps precede event delivery;
completed work publishes only after successful delivery. Consumer faults stop the
remaining frame ticks and discard unpublished work, while coherent continuation
remains saveable.

Horizontal proximity uses validated simulation anchors and rounded millimetres,
with coordinate differences bounded before squaring. Promotion is inclusive at
the species distance and requires an Active owner. Full creatures retain their
tier through the additional 64 metre hysteresis band. Reduced owners, disabled
promotion and missing valid anchors demote Full creatures. Frozen owners only
remove runtime attachments: persisted tier, position, optional presence, wishes,
velocity and counters remain unchanged until thaw. Rendering and frame duration
never choose a tier. Demotion captures position and matching named pressures;
promotion imports them. Need names, order and Full growth rates remain intact.
Integer-to-float projection stays inside the admitted owner even when a boundary
millimetre cannot be represented by a float. Independent Full tick counters and
coarse cadence counters survive repeated transitions without conversion.

`AdmitCreature` resolves immutable species policy and admits required authority
through the shared allocator. Controller participation remains explicit: false
stays kinematic, including adopted legacy absence. True requires saved locomotion
continuation. Runtime physics handles are opaque, removable and fresh on reused
storage; surviving handles remain stable and stale handles refuse selected
stepping. Creature selection follows PersistentId, independently of storage slots,
and never steps unselected players or dormant creatures. Reconstruction restores
all velocity components and the grounded branch before the first step. Each
selected step synchronizes continuation immediately, so saving does not sample
controllers or change creature authority. Full movement walks the horizontal
region path, checks intervening owners and corner neighbors, and parks inside
the last admitted owner at an absent or Frozen border. Movement never activates a
region. The legacy index API and all-avatar stepping remain available for the
disabled host path.

Plants use species-specific cadence independently of page and water eligibility:
Active shift zero, Reduced shift equal to the smaller of the region shift and the
species maximum, and the ledger's phase at that effective shift. Persisted plant
stamps bound each elapsed window. Resume windows also obey the species cap,
calendar day bound and 36,000 tick bound. A zero cap stamps and acknowledges the
resume without evolving that plant. All affected plants and admitted births are
resolved before the shared resume token is acknowledged. Faster plant eligibility
does not authorize extra page or water updates. Historical replay continues to
exclude creature lifespan, decay, needs and feeding; final-substep grazing retains
the transient positional source compatibility rule.

`GetCreatureActivity` provides observational tier counts, transitions, accepted
updates, births, deaths, admitted removals, parked travel and controller lifecycle counts. Sampling
it neither allocates identities nor changes authority. These diagnostics do not
feed scheduling, RNG, hashes or completed-work accounting. Complete creature and
region projections, existing compatibility adoption rules and all persistence,
network and checkpoint layouts remain unchanged.
