# Frozen revision-6 presets

These seven JSON files are byte-for-byte copies of the packaged presets before
revision 7 (source commit recorded in `provenance.json`). Do not edit them when
changing shipping presets. `sha256.json` pins their original bytes.

Existing saves without `preset.json` resolve here by `worldType`, even if the
current packaged preset has been removed or changed. A missing historical preset
is an error; the catalog never falls back to current shipping terrain. Embedded
presets remain authoritative. New worlds use `worlds/atlas/presets` at revision 7.

When save metadata carries `presetIdentity`, the catalog checks it against the
FNV-1a checksum of canonical JSON (`nlohmann::json::dump()`), the existing
benchmark identity format. Historical metadata without an identity is supported.
The revision-6 hills regression fixture pins `026f24207a8ffc7e`; its terrain
parameter identity at seed 424242 is `16636010052247870746`.

This directory is a sibling of `presets` to preserve the canonical loader's
data-root resolution and is included by the existing recursive worlds install.
