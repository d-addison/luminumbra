# T-I6-042 Client Render Remote Avatars

## Goal

The client must visibly render remote multiplayer avatars from replicated server snapshots, not from local-only scripted transforms.

## Contract

- The skinned avatar showcase supports `--scenario=skinned_mesh_visual_smoke --avatars N --replicated` with `N >= 2`.
- Client id `1` is treated as the local avatar.
- Client ids `2..N` are treated as remote avatars.
- Remote avatar transforms are driven by the in-process replication path: server pose update, snapshot delivery, client interpolation, then render transform application.
- The render proof is written to `remote-avatar-render.json` in the runtime artifact directory.

## Artifact Requirements

The artifact schema is `luminumbra.network.remote_avatar_render.v1` and records:

- local client id
- expected avatar count
- remote avatar count
- replicated frame count
- rendered avatar count
- skinned draw and index counts
- per-client snapshot sequence, server tick, position in millimeters, and remote/rendered flags
- checks for snapshot receipt, interpolation, remote rendering, local/remote split, and deterministic client ordering

## Acceptance

The run passes when:

- at least one remote avatar exists
- every expected avatar has a replicated pose
- all remote avatars are marked interpolated
- the skinned render pass draws at least the expected avatar count
- the local client is not included in the remote set
- client pose ordering is deterministic by client id
