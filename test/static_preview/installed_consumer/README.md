# Installed SDK consumer

Copy this directory to a fresh location outside the checkout. It is an independent
CMake project using only `find_package(LuminumbraStaticPreview CONFIG REQUIRED)` and
the imported `Luminumbra::StaticPreview` target. It has no source-tree includes,
downloaded dependencies, test framework, or GPU context. Use a compiler compatible
with the installed SDK's C++ ABI.

The executable needs the installed-acceptance fixture produced by
`test/authoring/installed_prefab_runtime.py`: `fixture.root` at `(10,2,3)`,
`fixture.first` translated by `(1,0,0)`, and mirrored `fixture.second`, with two
mesh primitives each. Pass an explicit generation and its manifest digest, even
when `current.json` has advanced. It checks immutable snapshots and resource
lifetime, parent edits, revision and malformed-operation refusals, perspective
and off-center orthographic reversed-Z math, and the actual loaded library path.
It does not qualify rendering, Blender, or native GPU performance.

Linux (replace paths and pins with recorded identities):

```sh
cmake -S /evidence/consumer-source -B /evidence/consumer-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLuminumbraStaticPreview_DIR=/sdk/lib/cmake/LuminumbraStaticPreview
cmake --build /evidence/consumer-build --parallel 2
/evidence/consumer-build/installed_consumer '/fixture/project with spaces' \
  GENERATION MANIFEST_SHA256 /sdk/lib/libluminumbra_render_static.so
```

Windows PowerShell, prepared for the matching UCRT64 MinGW SDK (run under the
native lane's owned process and deadline):

```powershell
$Sdk = 'C:/acceptance/sdk'
$ConsumerSource = 'C:/acceptance/consumer-source'
$ConsumerBuild = 'C:/acceptance/consumer-build'
& 'C:/msys64/ucrt64/bin/cmake.exe' -S $ConsumerSource -B $ConsumerBuild -G Ninja `
  '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON' `
  '-DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe' `
  '-DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe' `
  "-DLuminumbraStaticPreview_DIR=$Sdk/lib/cmake/LuminumbraStaticPreview"
if ($LASTEXITCODE -ne 0) { throw 'Consumer configure failed' }
& 'C:/msys64/ucrt64/bin/cmake.exe' --build $ConsumerBuild --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Consumer build failed' }
$PreviousPath = $env:PATH
try {
  # Installed SDK/bin supplies all three MinGW runtime DLLs.
  $env:PATH = "$Sdk/bin;$env:SystemRoot/System32"
  & "$ConsumerBuild/installed_consumer.exe" 'C:/acceptance/project with spaces' `
    GENERATION MANIFEST_SHA256 "$Sdk/bin/libluminumbra_render_static.dll"
  if ($LASTEXITCODE -ne 0) { throw 'Consumer acceptance failed' }
} finally { $env:PATH = $PreviousPath }
```

Runtime DLL packaging still requires the native lane to execute this restricted
PATH command and record all actual loaded module paths and hashes. Older SDKs
without these DLLs must fail this qualification. For either platform, preserve
SDK file hashes before and after, copied consumer source
hashes, compiler/CMake identities, compile commands and header dependencies,
binary hash, command/output/exit code, and the fixture's immutable generation
pin. A failed process or changed SDK remains a failed receipt.

On Windows the consumer inspects already loaded dependencies using
`GetModuleHandleW` and `GetModuleFileNameW`, requires all three runtime DLLs to
come from the expected renderer module's directory, and prints their exact paths.
It does not load missing DLLs to satisfy those checks.

For the separate rendering probe, add `--require-sdk-runtime` to
`test/static_preview/installed_viewport.py` under the same restricted PATH. After
the first authenticated frame, it takes a bounded, read-only
[Toolhelp module snapshot](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot)
of its owned host PID. Six extra checks require Windows and bind the host,
renderer DLL, and three runtime DLLs to the complete SDK inventory captured before
launch. Exact paths and SHA256 digests are retained in `acceptance.json`; an
external, missing, ambiguous, or changed module fails the receipt. The existing
75-second host deadline and final SDK hash guard remain in force. Omitting the
flag preserves the existing probe check roster; a flagged non-Windows run fails.
`test_runtime_closure.py` supplies portable refusal controls plus an actual
Windows child-process snapshot test. Portable controls do not qualify DLL loading.

The MinGW install resolves `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, and
`libwinpthread-1.dll` through the configured compiler and requires each to be in
that compiler's real `bin` directory. When GCC returns the exact requested bare
filename, only that compiler's sibling file is considered; the current directory
and PATH are never searched. Other relative responses fail. Their toolchain
license notices accompany the DLLs. It refuses missing, redirected, or ambiguous
resolution; it does not copy
Windows system DLLs. `test_mingw_runtime_install.py` exercises CMake resolution,
refusal, and the unchanged non-MinGW install with compiler-response fixtures.
Those portable tests do not qualify native Windows loading.
