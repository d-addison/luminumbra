# Wave 3 Integration

Task: T-EF-11-wave3-integration
Status: integrated

## Purpose

This artifact records the wave 3 integration handoff for the engine-frontier workstream. It defines the integration boundary, expected downstream contract, and verification items that should be satisfied before this wave is treated as ready for promotion.

## Integration Boundary

- The integration surface is limited to engine-frontier wave 3 artifacts.
- This file is the coordination record for the wave 3 integration package.
- Downstream consumers should treat implementation details as owned by their source artifacts and use this file for merge readiness, sequencing, and validation expectations.

## Handoff Contract

| Area | Integrated expectation | Required downstream handling |
| --- | --- | --- |
| Artifact ownership | Wave 3 integration is represented by this artifact. | Keep this file in `.forge/artifacts/engine-frontier/` unless every dependent reference is updated. |
| Merge sequencing | Wave 3 should be reviewed after its source wave artifacts are available. | Validate source artifacts before using this handoff as the final authority. |
| Compatibility | Wave 3 integration should not require unrelated artifact movement. | Preserve existing artifact paths and avoid broad metadata churn. |
| Verification | Integration readiness depends on local consistency checks. | Re-run applicable checks when any referenced wave 3 artifact changes. |

## Integration Checklist

- Confirm wave 3 artifact paths remain stable.
- Confirm no dependent artifact is moved or deleted without updating references.
- Confirm the wave 3 handoff can be read independently by later Forge tasks.
- Confirm implementation-specific decisions remain in their owning artifacts.
- Confirm this integration record only summarizes readiness and contract expectations.

## Promotion Notes

- Promote wave 3 only after source artifacts and dependent references agree on ownership and path stability.
- Treat unresolved source-artifact conflicts as blockers for promotion.
- Use this file as the wave 3 coordination anchor for follow-on engine-frontier tasks.

## Residual Risks

- Source artifacts may change after this integration record is written.
- Downstream tasks may require more specific implementation links if their inputs expand.
- Verification coverage depends on the checks selected by the owning implementation tasks.
