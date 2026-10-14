# Creature entity save format

Enabled sessions store complete creature authority in
`chunks/region/creature-entities.json`. This member joins the existing snapshot
index and undo journal; it introduces no chunk, network or replay layout change.
The root has exactly `schema` and `entities`. The schema is
`luminumbra.creature_entities.v1`. Each entity has exactly `id` and `components`.
Records ascend by shared PersistentId. Components form a name-sorted object;
each value has exactly `version` (integer 1) and `data` (an object).

Writers use compact UTF-8 JSON followed by one newline. Readers reject duplicate
keys, unknown keys, unknown components, future schema/component versions, wrong
scalar kinds, narrowing, missing required fields and unordered/duplicate IDs.
They accept insignificant JSON whitespace. Limits are 16,384 records, a legacy
base of 64 MiB including the trailing newline, 1,024 UTF-8 bytes per string,
256 elements per vector, 256 keys per object (the same collection budget), and
nesting depth 32.
For `R` records and `C` present compact Controller member bytes, reading,
canonical validation and writing use the same byte bound:
`67,108,864 + min(C, R × 63)`. Each present member earns only its compact bytes,
including its key and separating comma, capped individually at 63. The largest
stamp is `,"Controller":{"data":{"physics_controlled":false},"version":1}`;
`true` occupies 62 bytes, an empty legacy `data` object occupies 37, and an absent
block earns zero. Whitespace earns no allowance. An unstamped legacy document
therefore retains the exact 64 MiB guard. Before Controller presence is known,
file reads and SAX input use only the absolute allocation ceiling:
`67,108,864 + 16,384 × 63 = 68,141,056` bytes. The parser counts present compact
Controller bytes and applies the document-specific bound after parsing.

The original SAX node budget remains 4,194,304 values, containers and keys.
For `N` present Controller nodes the bound is
`4,194,304 + min(N, R × 8)`, with at most eight credited nodes per present member:
the Controller key, its object, the data key/object, the physics_controlled
key/boolean and the version key/integer. An empty legacy data object earns six
nodes; an absent block earns zero. The absolute stamped ceiling is therefore
`4,194,304 + 16,384 × 8 = 4,325,376` nodes. Controller nodes are counted during
SAX parsing; excess nodes in other payloads do not earn credit. The SAX decoder
checks node and collection budgets before insertion; oversized collections are
never fully materialized. Writers apply the same budgets before emitting bytes.
Loading also checks the adopted, stamped snapshot against these writer budgets
before publishing it, so every accepted legacy snapshot can be stamped and saved.
Numeric strings and fractional integer encodings are refused. Booleans are JSON booleans, except existing u8 flags, which retain
integer 0/1 (nocturnal retains its full u8 nonzero phenotype). Counters retain
their declared unsigned width; signed coordinates and indexes retain their
declared signed width.

Every f32 field uses the JSON key `<field>_bits` and a u32 IEEE-754 bit pattern.
Planner f64 values use u64 bit patterns. No object memory, padding, numeric float
conversion or runtime EnTT identity is serialized. Finite values, including
negative zero and subnormals, retain their exact bits. Non-finite values refuse.
Normalized pressures, activity, alarm, proximity, vigilance, awareness and biomass
are in [0,1]; FOV cosines are in [-1,1]. Speeds, ranges, durations, gains and rates
with nonnegative contracts refuse negative values. A scent sense sign is -1 or +1.
Action IDs are 0 through 6. Decay ticks do not exceed duration, nutrient release
is at most 1,000, and combustion fuel/moisture are at most 1,000 with burn state
0 through 2. Awareness states are 0 through 4; stimulus channels are 0 through 5.
Emitter radius is an integer in [0,31], bounding a gather to at most 3,969
cells. A u32 rate shifted by 32 or more is zero, so 31 is the last useful
Chebyshev ring under the existing falloff rule. Larger radii refuse even for
zero-rate emitters.
Plan/path indexes may equal vector length to denote completion. A planner ranking
index is -1 or identifies an existing candidate.

Required components are `Transform`, `Creature`, `Region` and `Coarse`.
Writers also stamp `Controller`; older v1 documents without that block or its
`physics_controlled` field adopt `false` (no controller participation), including
when velocity state is present. This legacy form is normalized before canonical
reprojection comparison and stamped explicitly on the next save. Present malformed
controller metadata still refuses. Every other component's presence is independent, including empty vectors and marker
components. Restore does not infer tags or install absent optional components.
The following inventory names every field. Dots in source field paths become
underscores in JSON keys; the float-bit suffix rule above applies afterward.

| Component | Fields |
|---|---|
| `Transform` | `position.x`, `position.y`, `position.z`, `rotation.w`, `rotation.x`, `rotation.y`, `rotation.z`, `scale.x`, `scale.y`, `scale.z` |
| `Region` | `persistent_id`, `owner_x`, `owner_z`, `region_stamp` |
| `Coarse` | `tier`, `transition`, `x_mm`, `y_mm`, `z_mm`, `hunger_ppm`, `thirst_ppm`, `needs`, `age_cadences`, `reproduce_cadences`, `reproduce_cooldown_cadences`, `courting_cadences`, `travel_pending`, `destination_x`, `destination_z` |
| `Controller` | boolean `physics_controlled` |
| `LocomotionState` | `velocity_x`, `velocity_y`, `velocity_z`, `grounded` |
| `Creature` | `is_predator`, `species_id`, `hunger`, `stamina`, `energy`, `move_speed`, `last_action`, `wish_x`, `wish_z`, `eaten` |
| `Genome` | `move_speed`, `vigilance`, `hunger_threshold`, `size_scale`, `vision_cos_half_fov`, `vision_range`, `hearing_range`, `age_ticks`, `reproduce_cooldown`, `generation`, `female`, `courting_ticks` |
| `Mortal` | `age_ticks`, `lifespan_ticks`, `dead` |
| `Decay` | `decay_ticks`, `decay_duration`, `nutrient_released_milli`, `fully_decomposed` |
| `Thirst` | `thirst`, `wish_x`, `wish_z`, `drinking`, `water_proximity` |
| `Circadian` | `nocturnal`, `activity` |
| `Alarm` | `level`, `alarmed` |
| `PackHunter` | `coord_x`, `coord_z`, `in_pack` |
| `Territory` | `home_x`, `home_z`, `radius`, `established` |
| `TerritoryBias` | `wish_x`, `wish_z` |
| `Migratory` | `drive`, `wish_x`, `wish_z` |
| `Scavenger` | `wish_x`, `wish_z`, `feeding` |
| `Needs` | `needs` |
| `Senses` | `nearest_food_source`, `nearest_water_source`, `nearest_threat`, `threat_distance` |
| `Opportunity` | `id`, `action`, `target`, `need`, `satisfaction`, `urgency`, `risk`, `stamina_cost`, `radius` |
| `ActionPlan` | `plan`, `current_action_index`, `time_in_current_action` |
| `InstinctAgent` | Empty marker object |
| `LocomotionProfile` | `move_speed`, `arrival_radius`, `slow_radius`, `separation_radius`, `separation_strength`, `flock_radius`, `cohesion_strength`, `alignment_strength` |
| `Sensable` | `scent_channel`, `scent_deposit`, `noise_loudness`, `noise_pitch`, `faction` |
| `Perception` | `vision_cos_half_fov`, `vision_range`, `facing_x`, `facing_z`, `ear.range`, `ear.peak_pitch`, `ear.bandwidth`, `ear.gain`, `ear.threshold`, `faction` |
| `Awareness` | `awareness.meter`, `awareness.state`, `awareness.has_last_known`, `awareness.last_known_x`, `awareness.last_known_z`, `awareness.time_since_signal`, `params.gain`, `params.decay`, `params.suspicious_at`, `params.alert_at`, `params.engaged_at` |
| `LocomotionIntent` | `wish_xz.x`, `wish_xz.y`, `arrived` |
| `ScentSense` | `channel`, `sign`, `strength`, `floor`, `weber_k` |
| `LocomotionPath` | `waypoints`, `index` |
| `StimulusSubscription` | `subscriptions` |
| `InstinctAgentState` | `actor_id`, `archetype`, `replan_interval_ticks`, `last_planned_tick`, `plans_executed`, `current_plan` |
| `Forager` | `cell_x`, `cell_z`, `home_x`, `home_z`, `carrying_food`, `deliveries` |
| `FoodSource` | `cell_x`, `cell_z`, `amount` |
| `FieldEmitter` | `channel`, `rate_raw_per_tick`, `radius_cells` |
| `PendingDeletion` | Empty marker object |
| `Grazeable` | `biomass`, `last_grazed_tick` |
| `WaterHole` | `radius` |
| `Combustible` | `fuel_milli`, `moisture_milli`, `burn_state`, `burn_ticks_remaining`, `ignition_radius` |
| `IgnitionSource` | `radius`, `intensity` |
| `WaterSource` | `output` |
| `WaterFlowSource` | `flow_rate` |

`Region.persistent_id` is a u64 equal to record `id`. IDs obey `0 < id < next`,
with allocator exhaustion at 2^63. They are unique across ground objects,
plants and creatures. Owner coordinates are i32 in [-32768,32768); the owner
must exist in the ledger and contain the Transform's XZ position. Position is
finite with XZ in [-2^24,2^24) metres and Y in [-2^20,2^20). The u64 region stamp
cannot exceed the saved ledger tick. Species IDs are nonzero u16; session
adoption checks their presence in the captured species inventory.

`Coarse.tier` is u8: 0 full, 1 coarse. `transition` is u8: 0 none, 1 pending
promotion, 2 pending demotion. Millimetre positions are i64 with the same spatial
limits. A coarse position must lie inside its owner under mathematical floor
division by 512,000. Hunger/thirst and named coarse pressures are u32 ppm in
[0,1,000,000]. Cadence counters are u64, separate from full-AI tick counters.
`needs` entries have exactly a nonempty unique `name` and u32 `pressure_ppm`.
`travel_pending` is boolean; destination coordinates have the owner-coordinate
bounds and may designate an absent or frozen region. Persistence advances none
of these counters and installs no creature ResumeHook. Admission rounding,
travel and tier conversion rules belong to the simulation consumers.

`LocomotionState` holds all three velocity bits and the grounded flag. The save
boundary samples attached controllers into this authoritative component. Runtime
`CreaturePhysics.avatar_index` is excluded and restore creates no avatars.
`Controller.physics_controlled` records participation independently of velocity
component presence and the runtime handle. Saving an attached controller sets
this bit; a restored entity retains it until controller reconstruction consumes
it. A true bit requires `LocomotionState`. This package restores the participation
bit and velocity/grounded values. Avatar creation and activation belong to the
session tier/controller integration; no movement step should run between restore
and that integration.

Nested vectors and planner objects have these exact inventories:

| Value | Fields |
|---|---|
| `Needs.needs[]` | string name, u32 pressure_bits, u32 growth_per_tick_bits |
| `ActionPlan.plan[]` | string name, persistent target reference, u32 duration_bits, boolean flee |
| `LocomotionPath.waypoints[]` | u32 x_bits, u32 y_bits |
| `StimulusSubscription.subscriptions[]` | u8 channel, string need, u32 gain_bits |
| `InstinctAgentState.current_plan` | string actor_id, string archetype, string decision_contract, candidates vector, i32 selected_index, string checksum, boolean passed |
| `current_plan.candidates[]` | i32 rank, strings id/action/target/need, u64 score_bits, u64 need_pressure_bits, string reason |

Planner target strings retain their authored meaning; entity-valued targets use
persistent references. `Senses`' three references and every `Action.target` have
exactly `kind` and `id`: `{kind:"null",id:0}`, `{kind:"creature",id:<u64>}`, or
`{kind:"plant",id:<u64>}`. A non-null target must exist in the corresponding
durable store. Wrong-kind, dangling and unsupported targets refuse; unadmitted
runtime targets cannot be saved. Ground objects have no ECS reference adapter in
this version. Restore creates every creature identity before resolving any
reference, so forward and cyclic creature references work. Plant targets are
resolved against plants already restored into the registry. Validation decodes
in scratch state, including all reference resolution, before destination entity
allocation or component signals. Input refusal leaves destination allocation and
generations unchanged. Public session loading restores the complete candidate world,
including species inventory checks and entity reference resolution, before resetting
or publishing any live authority. Rejected replacements preserve the previous world's
systems, registry, metadata, ledger, ground objects, fields, clocks and saveability;
their diagnostic is separate from the current world's integrity error. The direct
state loader also preflights restoration and existing target identities before
publishing any member; its failures latch the open error and disable saving.

An empty creature set removes the file durably and records an authoritative
absent member with zero bytes/digest in `snapshot.json`, including the first
empty save. A present empty creature file refuses. Missing files are clean empty
stores after membership verification. Previous-release indexes that omit the
creature member entirely remain valid when the file is absent; an unlisted file,
a missing indexed-present file, or a file behind indexed absence refuses.
A present file without an index also refuses. Older readers refuse the new
member path, including an absent member, through their fixed index allowlist.

Creature replacements/deletions use the same outer transaction as metadata,
ledger, chunks, plants, fields and ground objects. Failure after creature
replacement rolls back all members and suppresses deferred acknowledgements.
Creature snapshots are complete projections and have no independently cleared
dirty/revision state. Saving adds no full-world decode. Region digests append
`|creatures:` plus the complete region projection after ground and page inputs;
empty creature projections add nothing. Creature ownership retains ledger
records. The ecology ordering/hash consumer is separate from this format.

With `sim.active_regions` false, GameSession does not project, validate, write,
remove or restore creature records, sample their controllers, or include their
new metadata in region digests. Existing disabled-path save/hash/artifact bytes
are unchanged. Existing content-identity compatibility rules still apply.
