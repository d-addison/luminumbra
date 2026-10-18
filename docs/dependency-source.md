# Reviewed dependency sources

RmlUi 6.1 uses the upstream source pin
`58c751531f4c1a1eb2dc56f409b8e8a323a0c9ad`
and the tracked patch in `cmake/patches/rmlui-6.1-shadow.patch`. The complete
upstream identity manifest is tracked metadata in
`vendor/rmlui/rmlui-6.1-source.json`. Both inputs use LF checkout bytes on every
platform, and changes to either select the Python and native CI lanes. The patch
renames constructor parameters and their bound uses and moves the unchanged
Top, Right, Bottom and Left aliases after OriginY. Public names, types, enum
values, layouts, defaults and executable operations are preserved.

Before the first configure, explicitly enroll a **fresh build root**:

```sh
python3 -I -B tools/ci/rmlui_source.py init --root build/release
cmake --preset release
```

For another preset or `cmake -B` directory, pass that exact directory to `init`.
Initialization refuses an existing file or directory. Use a new directory when
the delivery changes or an earlier delivery was interrupted. Existing dependency
caches and source overrides cannot be adopted or patched. Do not remove an
unknown ownership, admission or readiness record to make it eligible.

The build root is an exclusive private developer or CI workspace. Non-cooperating
namespace writers are outside this contract. There is no claim of conditional
unlink by file identity or durable namespace publication across power loss.

CMake populates RmlUi separately. Its empty population directory is distinct
from the identity-specific source destination because FetchContent creates its
source directory before running the download command. The helper downloads only
the fixed upstream URL and full revision, verifies ordinary local Git metadata,
HEAD, index, modes and the complete 1,118-file manifest, applies the exact patch,
and verifies every patched and unchanged file. It publishes the source before
writing readiness. The receipt binds the helper, manifest, patch and provisioning
policy. CMake verifies the completed source before evaluating upstream CMake.

A completed source with matching ownership and receipt can be verified repeatedly
without writes. Partial, mixed, unexpected or ambiguous inputs refuse and require
a new root. The global update-disconnected policy remains in force. Fully
disconnected configuration requires an already completed exact source; it never
fetches a replacement. Source overrides, shared Git administration, symlinks,
reparse paths, path escapes and Windows short-name aliases are ineligible.

`RmlUiSourceScriptContract` exercises the real patch against the full vendor
input and checks delivery refusals and read-only repetition. Offline authors may
provide an immutable pristine archive extraction directly under their private
`TMPDIR` through `LUMINUMBRA_RMLUI_TEST_SOURCE`; this is a test input only. The
tests temporarily move that input and restore it afterward. Otherwise the suite
fetches the exact revision into its own temporary workspace. No alternate
revision or cache fallback exists.

The Python controls and source checks do not qualify generated compile commands,
native builds, Windows, or minimum-supported CMake 3.20. Those require execution
on their supported lanes. Separate dependency and SDK failures retain their own
qualification requirements.
