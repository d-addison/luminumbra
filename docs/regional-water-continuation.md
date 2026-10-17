# Regional water continuation (S7)

`sim.active_regions` enables the owner-thread fixed-tick water consumer in
`GameSession`, after current weather/energy and before foliage acknowledges
resume tokens. The streaming-frame water call binds its resident query map;
water evolves only on a successful fixed-tick opportunity. Loading restores
saved depth, bed, flux, resolution and sleep bookkeeping without boot stepping
or an uncapped migration. There is no elapsed-time water catch-up. Plant policy
shifts remain 1 through 8; this consumer does not change or clamp that policy.

This contract applies the October 13 water questioner corrections to the
historical D9 proposal. `WaterShare` and its independent decimal oracle remain
unchanged. The historical arithmetic and unconditional starvation claim are
superseded by the locked Q32 allocation and conditional scope below.

## Admission and unique operations

Snapshot streamed resident chunks once. Null, Loading, unknown-owner and
Frozen chunks never enter executable water admission. Parked and durable-only
payloads are neither source targets nor eligibility inputs. Validate packed
identities, world coordinate limits, legal saved resolutions (4, 8, 16, 32),
exact depth/bed/flux and required mirror shapes, nonnegative depth, finite
mirrors and bounded continuation counters. Empty uninitialized storage is a
legitimate preparation opportunity. Partial authority refuses; it is never
repaired or interpreted as a new grid.

Construct one immutable ascending unique candidate array per due owner before
claims. Candidates include preparation plus one kernel, prior awake state,
configured rain, resident physical sources and a one-hop prior-neighbor wake.
A configured weather-rain provider with positive scale conservatively admits
ready sleeping grids even when a particular cell's precipitation is zero.
New wake decisions cannot cascade within the same tick.

Physical sources use `WaterFlowSourceComponent` (float cubic metres/second).
Irrigation uses `WaterSourceComponent` (u16 moisture). These were previously
conflicting definitions of the same C++ type; they now have distinct identities.
Physical rates are checked and quantized once from frozen source inputs, sorted
by target/cell/amount. Resolve a resident initialized direct chunk first, then
its resident y=0 column; use floor cell mapping relative to the resolved target.
No authority lookup or rehydration is performed. Selected chunks receive source,
rain, damping and sleep progression once. Border-only participants receive none
of those kernel operations.

Enumerate each physical +X/+Z face once, retaining its positive endpoint while
sorting by owner RegionKey, neighbor RegionKey, positive ChunkID, other ChunkID,
and direction. Cross-owner reservations require known nonFrozen endpoints with
complete prior grids at the session resolution and common phase
`t % 2^max(shift_a,shift_b) == 0`. A preparation endpoint seals that border this
tick. Distinct faces may share chunks and corner cells. Each face remains a
separate charged sequential operation. Same-owner faces execute only when both
endpoints were selected. Nonzero seam transfer wakes both endpoints and resets
calm counters after interior sleep bookkeeping.

## Capacity, debt and completed work

At uniform target resolution R, the conservative capacity of one window is
`C = 2R² + 4R` cells. The default cell budget is 4096 and
`W_global = floor(cell_budget/C)`. Preparation charges R², each kernel charges
R², and each executed face charges 2R endpoints. Exact planned work is checked
against the cell budget as well.

Reserve each cross-owner face only if two windows fit. Preserve one interior
window when any positive due claim exists and the global budget is nonzero.
Seal every unfunded face. Let P be reserved cross-owner faces and
`W_int = W_global - 2P`. Run the unchanged checked Q32 allocator on a debt copy
using only current due claims. For grant q and candidate count a:

```
u = min(q,a)
start = saved_cursor % a                  (only when a > 0 and u > 0)
visit[j] = candidate[(start+j) % a]        (0 <= j < u)
next_cursor = (start+u) % a
```

Every positive grant with positive a executes at least one real kernel.
No chunk kernel repeats. Zero visits leave the cursor untouched. Allocated
capacity `q-u` is discarded without a refund, redistribution or completed work.
Old zero-awake due debt may still receive grants; absent claim debt is unchanged.
The allocator's cap/refusal semantics are preserved.

Let G sum allocated interior grants and U sum unique interior visits. Successful
telemetry exposes W_global, 2P, G, U, G-U, W_int-G, preparation cells, kernel and
seam cells, total budgeted cells, sources/sinks and exact volume. Both
`2P+G <= W_global` and `2P+U <= W_global` hold. For positive total awake claims:

```
D_after = D_before + W_int*Q - G*Q
```

For zero total awake claims there is no credit; current old debt can be serviced.
Empty claims leave all debt unchanged. Grant accounting subtracts G, completed
work counts U. Each actually processed owner receives one water region_ticks
contribution. Its water_cells counts kernels and every processed face endpoint,
including two endpoints charged to the same owner on a same-owner face. The
existing ARL work aggregator adds other consumers' contributions separately.

Mass covers the participant union, including border-only chunks, with explicit
preparation and ECS/kernel source/sink contributions. Physical volume is exact
in `millimetres * quarter-square-metres`, including old 32x32 grids. Depth-sum
and physical-volume invariants both must pass before publication.

## Refusal, publication and persistence

Copy the ledger and stage only participating chunks' water state. Checked
preparation, source additions, internal flux application and seam destination
additions execute privately. Allocate the next completed-work vector and far
revision map before publication. Any preflight, allocation, conversion, solver
or invariant refusal preserves live arrays, debts, cursors, revisions and prior
successful telemetry/work. The existing host fail-stop path handles the thrown
error. Earlier consumers in the tick retain their accepted fault semantics.
Repeated or backward successful tick execution refuses as well.

The commit swaps water arrays, moves the prepared ledger, swaps work and the far
revision map, and stores scalar/atomic state. No allocation or validation remains.
Changed chunks become sticky durable water authority and are dirty for saving;
water revisions increment once on live publication. Far authority revisions are
prepared under the revision mutex, including boundary halo regions.
Publication consumes the pending water-promotion flag after transferring the
obligation to the save-dirty bit. It does not acknowledge persistence; that bit
clears only when the snapshot commits with matching voxel and water revisions.

Existing durable formats persist all authority: chunk records hold water arrays
and sleep/resolution bookkeeping; FPG sections 5/6 hold soil and irrigation;
plant records hold persistent IDs, owner stamps and enabled component state;
ARL holds water debt/cursors and completed work. One indexed snapshot commits
these members together. Enabled plant physical rates use exact float bit
payloads. Indexed water reads refuse missing, narrowed or malformed authority;
they never invoke the legacy missing-flux repair. Error remains distinct from
Absent, including during world-open validation and exact durable reads.

Flag-off calls retain streaming-frame selection, global cursors and legacy
kernel ordering. Legacy serialization fields and hash projection are unchanged;
strict durable water checks apply to indexed enabled records. The physical
source C++ API rename is a compatibility change for external source spawners.
Native byte/hash qualification still requires CI execution.

## Qualification scope

The pure allocator's fixed-cohort bound requires fixed participating eligibility,
fixed successful positive shares and fixed W_int. Changing border reservations
changes W_int even when W_global is fixed. This caller has not established those
prerequisites and reports `fixed_cohort_bound_known=false`. No unconditional
region, chunk or border service bound is claimed.

The authored common and persistence regressions cover the executable contract;
they have not been built or run in this worker session. GCC/Clang/MSVC warnings,
clang-tidy, sanitizers, deterministic native replays, golden flag-off hashes,
performance and allocation-failure injection remain unqualified. The existing
foliage qualification table still limits exact coarse equality to its stated
integer-linear inputs; arbitrary unrecorded varying-input stage/event divergence
remains unproven. Accepted shifts 1..8 remain available throughout.
