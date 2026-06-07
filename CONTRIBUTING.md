# Contributing

## Build

Use the repository root as the CMake source directory:

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Source Lists

The active build graph enters the client through `src/CMakeLists.txt`. The
`src/luminumbra_client/CMakeLists.txt` file is legacy-only unless it is first
updated to match the active client library plus executable layout.

When adding or removing `.cpp` files, update the owning manifest in the same
change:

- `src/luminumbra_common/sources.cmake` for common engine sources.
- `src/luminumbra_client/sources.cmake` for client library, client app, and
  vendored ImGui sources compiled into the client.

Keep manifest paths anchored with `CMAKE_CURRENT_LIST_DIR` so the files can be
included from either the module directory or the parent `src` directory.
