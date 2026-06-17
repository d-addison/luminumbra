# Remote Avatar Render Completion

Task: T-I6-042-client-render-remote-avatars

## Completed

- Added the `luminumbra.network.remote_avatar_render.v1` report contract in `src/luminumbra_common/network`.
- Added serialization, baseline validation, fixture construction, and artifact writing APIs.
- Moved replicated showcase avatar transform application into the pre-render client phase so captured frames reflect network-driven poses.
- Added runtime emission of `remote-avatar-render.json` for replicated multi-avatar showcase runs.

## Runtime Proof

The client writes `remote-avatar-render.json` when:

- `skinned_mesh_visual_smoke` is running
- `--avatars` is at least `2`
- `--replicated` is enabled
- replicated poses have been applied for at least two render frames
- the skinned pass has drawn at least the spawned avatar count

The artifact records actual transform positions from the client registry after replication/interpolation, plus the skinned draw counters from the rendered frame.

## Validation

Static validation was run with `git diff --check`.

The full runtime smoke was not run in this task turn because the requested file scope only allowed reading the assigned client and network paths.
