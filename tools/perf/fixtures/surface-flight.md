# Surface-flight identity provenance

The strict `.traversal` grammar does not accept comments. This adjacent note
records the deliberate defaults-on identity changes in `surface-flight.traversal`:

| Field | Before (revision 6) | After (revision 7) | Reason |
| --- | --- | --- | --- |
| `preset_revision` | `6` | `7` | New worlds materialize shipping feature defaults. |
| `preset_identity` | `026f24207a8ffc7e` | `0b5c29c7a27f611a` | The authored default preset enables its island mask and declares revision 7. |
| `content_identity` | `16636010052247870746` | `7583622726659997707` | `ComputeTerrainParamsHash` includes the enabled island-mask byte. |

The preset identity is FNV-1a over the sorted, compact JSON saved by
`GameSession::CreateWorld`. The content identity uses the canonical little-endian
object-byte mixing in `FarLodStore.cpp`, including loaded biome and structure
content hashes. An independent stdlib Python calculation reproduced **both old
identities exactly** before calculating the replacements. The loaded content
hashes were biome `5f02aa0bb6ecaa10` and structures `6b7d66e6db67b51f`.

These identities describe the unchanged shipping `default` preset, not the
separately retuned `temperate_forest` or `caverns` presets. The frozen revision-6
save fixture retains its original identity. No runtime simulation hash was
inferred from these calculations.
