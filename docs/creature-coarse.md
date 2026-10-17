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
