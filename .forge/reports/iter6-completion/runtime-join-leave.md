# Runtime Join/Leave Server Mode

## Completed

- Added `NetworkRuntimeJoinLeave*` fixture/report APIs under `src/luminumbra_common/network`.
- Added TCP host `--server-mode` for runtime client lifecycle handling.
- Host server mode now ticks before clients connect, accepts late clients, logs leave events, zeroes disconnected input, and exits successfully after completing requested ticks.
- Added server-mode artifact output for lifecycle diagnostics.

## Verification

- `git diff --check -- .forge/specs/iter6/multiplayer-runtime-join-leave-server-mode.md src/luminumbra_common/network src/luminumbra_server/main_server.cpp .forge/reports/iter6-completion/runtime-join-leave.md` passed.
- Forge hygiene verification passed for `NetworkLoopbackAuthority.h`, `NetworkLoopbackAuthority.cpp`, and `main_server.cpp`.
- `cmake --build build --target luminumbra_server_app --config Debug` could not run because this worktree has no configured CMake cache at `build`.
