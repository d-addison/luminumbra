# Regional water continuation

Regional water continuation is available through the test-only authority profile.
Shipping clients, servers and replays retain their existing water policy; enabling
`sim.active_regions` alone does not activate the profile. Native qualification and
production activation remain pending.

## Planning and execution

The planner consumes a bounded participation snapshot selected from simulation
anchors and physical sources. Residency does not define that simulation horizon.
Only funded columns may read committed payloads or generate terrain. Missing or
frozen owners are sealed, and parking pressure can defer the entire opportunity.
Planning leaves chunks, debt and cursors unchanged.

Each admitted owner has a canonical candidate order. Budgeted preparation can
initialize or resize a selected grid; kernels and seams consume the same bounded
cell budget. Source input is captured before execution, checked for representable
integer conversion and sorted canonically. Physical sources use
`FlowingWaterSourceComponent`; irrigation uses the distinct u16
`WaterSourceComponent`. A physical source component does not acquire a new plant
serialization or hash binding through this integration.

Shared faces retain canonical positive-X or positive-Z orientation. Both owners'
persisted cadence phases must admit a cross-owner face. Reservations fund both
endpoints together and seal unfunded faces. Same-owner pairs consume funded owner
windows. The planner rotates eligible pairs and cross-owner faces under its
existing conditional service rules. Changing eligibility or available funding
provides no unconditional starvation bound.

Grant accounting is separate from completed work. Unselected grants retain their
Q32 debt under the current allocator policy. Completed work counts only actual
preparation, kernel and face work, including both endpoints of a same-owner face.
A larger grant never repeats rain, source injection, damping or sleep advancement
on a selected interior in the same opportunity.

## Validation and publication

The checked kernels reject malformed depth, bed, momentum and mirror shapes
before live mutation. Source, interior and seam additions check representability.
A transaction stages only participating water and sparse ledger patches; it
validates the whole plan before publishing chunks, revisions, mesh invalidations,
cursors, debt and completed work. A failed plan preserves prior published state.
Repeated or backward transaction ticks refuse. The host profile executes this
work at a fixed simulation boundary and restores saved continuation without
elapsed-time catch-up.

Durable indexed reads use the current strict validation and retain the distinction
between absent authority and a read error. An error cannot fall back to generation
or the permissive legacy missing-flux repair. Existing snapshot transactions commit
the current chunk, plant, field, ledger and metadata members together. Current
metadata, manifest, journal, plant and ledger versions remain unchanged.

## Foliage and compatibility

Existing foliage pages store soil and irrigation in exact one-metre integer legacy
units, with 16-by-16 cells per page and bounded page counts. Existing field-page
sections 5 and 6, persistent plant identities and owner stamps are retained.
Due growth and bounded resume acknowledgement use the current staged consumer
implementation and its replay limits. Soil, irrigation and decomposition keep
the current region admission gates. Irrigation seam exchange samples prior-state
flux and applies paired transfers limited by current donor moisture and recipient
capacity, preserving their combined quantity.

This integration keeps authority revision 8 and adds no world, water, foliage,
field or authority hash content. It adds no save, replay, checkpoint, Hello or
configuration fields. Flag-off simulation and serialization keep their existing
bytes. The parameterised 3,600-tick budget parity coverage retains its existing
three passes per region mode; it is not duplicated in sanitizer shards.

The added diffusion regression compares measured and unmeasured solver evolution
against an independent integer-indexed reference with exact floating-point bit
comparisons. Telemetry sums repeated stage visits within one host tick while
retaining the independent work-item trace. These changes introduce no authority
state. Native replays, sanitizers, performance qualification and cross-platform
execution still require the normal native gates.
