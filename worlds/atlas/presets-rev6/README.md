# Frozen revision-6 presets

These eight preset JSON files are byte-for-byte copies of the packaged presets
before revision 7. `provenance.json` maps each filename to its full source commit
in `source_commits`; `source_directory` and `schema_rev` apply to every entry.
The original seven come from `3b2c0677f45319600bf01ce564fde47489033dfd`;
`cave_dense.json` comes from `4591c7eb9361e12f69432cd6c133211e5ed66842`,
which shipped it later. Do not edit these copies when changing shipping presets.
`sha256.json` pins their original bytes.

Existing saves without `preset.json` resolve here by `worldType`, even if the
current packaged preset has been removed or changed. A missing historical preset
is an error; the catalog never falls back to current shipping terrain. Embedded
presets remain authoritative. New worlds use `worlds/atlas/presets` at revision 7.

When save metadata carries `presetIdentity`, the catalog checks it against the
FNV-1a checksum of canonical JSON (`nlohmann::json::dump()`), the existing
benchmark identity format. Historical metadata without an identity is supported.
The revision-6 hills regression fixture pins `026f24207a8ffc7e`; its terrain
parameter identity at seed 424242 is `16636010052247870746`.
The frozen cave-dense preset pins `e70158a4777ee36b`; the shipping revision-7
copy pins `5d081f382b2d5d0d`. Qualification fixtures must declare which version
they use rather than assuming identical terrain from the preset name.

This directory is a sibling of `presets` to preserve the canonical loader's
data-root resolution and is included by the existing recursive worlds install.
