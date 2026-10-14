# Interim item and seed consumer

The client and headless host select the common item consumer before creating or opening a world.
It runs only with `sim.active_regions`; generic ground-store callers may leave it unselected.
With the flag off, catalogue files (including malformed files) and farming inventory blocks are
ignored, and no new save members, metadata or hash terms are emitted. Network and replay layouts
are unchanged. The drop action is a local host operation; there is no remote drop command.

The runtime catalogue destination is `data/common/items/items.json`, corresponding to the game's
`content/items/items.json`. Its schema is `luminumbra.game.items.v1`, with exactly `schema` and
`items` at the root. Items require `id`, `despawn_after`, and `payload`; optional `render` requires
exactly `model_id` and `material_id`. Item names are unique lowercase ASCII kebab-case, 1–64 bytes,
sorted in ascending order. Numeric fields are strict u32, excluding booleans, decimal and exponent
tokens. Duplicate, missing and unknown keys refuse. Files are UTF-8 with LF, no BOM and a final LF;
limits are 1 MiB and 4,096 definitions. Absence, unreadability, nonregular files and invalid content
refuse enabled game admission. A present render reference currently refuses admission because this
consumer has no model resolver; the shipping seed omits `render`.

Item type is FNV-1a-32 over the exact ID bytes, starting at 2166136261 and multiplying by 16777619
after each byte XOR, modulo 2^32. Zero refuses without remapping, and distinct-name collisions
refuse. Vectors: empty helper input 2166136261, `a` 3826002220, `farming-seed` 516719959,
`costarring` and `liquid` both 1582148253. Empty names are invalid. Parsing and reference validation
use candidates; refusal preserves the live catalogue and world state, including the ability to
save and drop. Enabled candidate admission errors are returned through the failed operation and
`GetLastWorldOpenAttemptError()`; they never enter the live operational error state. A successfully admitted
catalogue is const and is retained for that world, independently of later file edits.

The seed definition has lifetime 900 and payload zero. `type` is the catalogue-derived u32,
`id` is the host-allocated durable u64 for a single object, and `payload` is separate game data.
Seed payload zero means one generic plantable seed; selecting a plant species never changes it.
Restore validates every saved object type against the admitted catalogue before adopting the ground
store. Unknown types refuse without deleting or reinterpreting disk records. The low-level ground
store remains generic.

`FarmDropSeed` is appended to the rebindable action table, default B. During interactive, unpaused
local play, each press requests a drop one horizontal metre ahead of the player's feet, with
0.25 metres of clearance. Nonfinite/zero horizontal direction or invalid positions refuse. UI
capture and window focus suppress the action. The key is sampled every frame, including loading,
pause and overlays. A drop requires consecutive eligible samples: key-up followed by key-down.
The first eligible frame only records state after any ineligible interval; pause, settings and focus
transitions also invalidate the edge within an event batch. Held presses, including refused or
captured presses, require an eligible release before retrying. `DropFarmingSeed` validates the session, authoritative controller, catalogue,
payload, position and stock before calling `SpawnGroundObject`. Success consumes one seed and does
not affect harvest telemetry. Preallocation refusal consumes neither seed nor ID. Postallocation
host refusal preserves the seed but burns the ID. Observation uses the const store; pickup is absent.

Enabled planting, harvesting and dropping use the session's single farming controller. The HUD
reads a mirror of that controller. Harvest seed additions saturate at `INT_MAX` on the enabled
path. The flag-off controller retains its existing behavior.

Enabled saves put `"farmingInventory":{"version":1,"seeds":4}` in `world_info.json` within the
same snapshot transaction as objects and the allocator. Only seeds persist; harvest counters remain
telemetry. Exact keys, version 1 and integer seeds in `0..INT_MAX` are required for a present block.
A malformed or unknown-version block refuses before inventory adoption. **Save compatibility:** a
previous-release save without this block adopts the controller's existing default count (currently
five) and writes the block on its next save. Existing durable seeds are retained. For saves carrying
the block, restart restores the remaining count instead of replenishing previously dropped seeds.
The enabled seed count joins the existing ecology hash slot; no wire or replay fields are added.

Settling and lifetime use the existing ground-store contract unchanged: integer millimetres, half
away from zero quantization, and downward resident authoritative terrain probes. Missing terrain
leaves the object unsettled. Settling changes only height and the settled bit. Every due update
increments age once, saturating at u32 maximum; unsettled objects also age. Reduced regions age
only when due; Frozen regions do not age. A nonzero lifetime expires at its stored age threshold;
zero never expires. Thus 900 is 30 seconds only for a region due on every 30 Hz tick. Store limits
remain 65,536 live objects and 4 MiB.

Source coverage is registered in `ItemCatalogue_test`, `ItemDrop_test`, and the extended
`GameSessionHeadlessWorld_test`. Existing ground-store and regional host tests cover zero lifetime,
quantization, Reduced cadence, Frozen invariance and thaw. Native client input/HUD and packaged
save/restart acceptance still require execution. Item presentation and versioned catalogue content
identity in ledger compatibility are separate integration prerequisites; this consumer does not
introduce a competing identity sidecar or ledger extension.
