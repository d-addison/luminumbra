# Polyglot Audit Report

Generated: 2026-06-07T12:40:01.975Z

## Summary

- Files scanned: 375
- Source lines scanned: 22447
- Languages detected: 8
- Risk findings: 36
- GoogleTest cases detected: 15
- CTest discovery wired: yes

## Languages

| Language | Files | LOC |
|----------|-------|-----|
| cpp | 102 | 11764 |
| json | 197 | 4740 |
| glsl | 36 | 1965 |
| markdown | 8 | 1627 |
| rcss | 4 | 1038 |
| lua | 11 | 500 |
| rml | 7 | 497 |
| cmake | 10 | 316 |

## Modules

| Module | Files | LOC |
|--------|-------|-----|
| luminumbra_client | 112 | 12285 |
| content_and_scripts | 208 | 5240 |
| luminumbra_common | 35 | 2723 |
| unmapped | 12 | 1763 |
| tests | 2 | 264 |
| tools | 2 | 145 |
| luminumbra_server | 4 | 27 |

## Risk Markers

| Pattern | Findings |
|---------|----------|
| todo | 20 |
| std_cout | 8 |
| raw_new_delete | 6 |
| runtime_error | 2 |

## Tests

- test/shield/test_world_generation.cpp

## Largest Files

| File | LOC |
|------|-----|
| src/luminumbra_client/rendering/RenderPipeline.cpp | 1184 |
| data/audio/sound_variation_definitions.bank.json | 575 |
| data/audio/complex_environmental_sfx.bank.json | 541 |
| data/audio/adaptive_music_system.bank.json | 534 |
| data/audio/complex_creature_sfx.bank.json | 472 |
| src/luminumbra_common/systems/SHIELD_WorldSystem.cpp | 433 |
| src/luminumbra_common/systems/PhysicsSystem.cpp | 412 |
| src/luminumbra_common/systems/WaterSystem.cpp | 388 |
| data/ui/themes/quantum/effects.rcss | 374 |
| src/luminumbra_client/audio/MiniaudioManager.cpp | 368 |
| data/ui/themes/quantum/components.rcss | 360 |
| src/luminumbra_client/ui/components/game/WorldList.cpp | 355 |

## Recommendations

- Treat CMake and CTest as first-class verification inputs for this repo.
- Keep vendor, external, and build outputs excluded from Forge pre-review scans.
- Promote the SHIELD world-generation test failures to the first remediation spec.
- Move Instinct Engine and Aetheric Field from README pillars into explicit C++ module specs before expanding implementation.
- Use this report as the project-local baseline until Forge core gets native C/C++ audit support.
