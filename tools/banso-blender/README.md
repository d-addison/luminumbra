# Banso Blender asset-family regeneration

This extension turns one geometry-nodes `.blend` graph into a deterministic family of validated GLBs. It expands a versioned JSON sidecar, starts the checked Blender version once for every expanded parameter set, calls `tools/blender/geonodes_bake.py` headlessly, runs `tools/blender/validate_glb.py` on every resulting GLB, and writes a family manifest.

Blender is a required host tool. A missing executable, an unexpected Blender version, a failed bake, a missing GLB, or any validator rejection makes the step red. The command never silently skips regeneration.

## Sidecar contract

Place the sidecar beside its source `.blend`. Version 1 has this shape:

```json
{
  "schemaVersion": 1,
  "sourceBlend": "forest.blend",
  "targetObject": "ProceduralTree",
  "expectedBlenderVersion": "4.3",
  "parameterMatrix": {
    "seeds": [11, 29],
    "parameters": {
      "Height": [4, 8],
      "Wind": [false, true]
    }
  },
  "output": {
    "directory": "generated",
    "glbNamingPattern": "tree-{index}-seed-{seed}.glb",
    "manifest": "tree-family.manifest.json"
  }
}
```

The schema is `schema/asset-family.schema.json`. `schemaVersion` is fixed at `1`. `sourceBlend` must be a filename beside the sidecar, not an absolute or parent path. `expectedBlenderVersion` accepts a pinned major/minor such as `4.3` or an exact patch such as `4.3.2`.

`parameterMatrix.seeds` is required even when the graph has no other varying inputs. The family is the Cartesian product of its unique integer seeds and every array in `parameters`, preserving declaration order. Scalar parameter values may be strings, finite numbers, or booleans; a non-empty scalar array represents a vector-like socket value.

`glbNamingPattern` is a filename ending in `.glb`. It may use `{index}` and `{seed}`. Expansion must produce a unique name for every family member. The output directory is relative to the sidecar and cannot escape it.

## Run the step

From the repository root:

```text
npm --prefix tools/banso-blender run regenerate-assets -- --sidecar path/to/forest.asset-family.json
```

Blender defaults to `blender` on `PATH`, and Python defaults to `python`. Override them explicitly when needed:

```text
npm --prefix tools/banso-blender run regenerate-assets -- --sidecar path/to/family.json --blender C:\Tools\Blender\blender.exe --python C:\Python312\python.exe
```

Each member uses this argument order, with paths and values substituted without shell interpolation:

```text
blender -b SOURCE.blend --python-exit-code 1 --python tools/blender/geonodes_bake.py -- --object OBJECT --output OUTPUT.glb --seed SEED --params-json COMPACT_JSON_OBJECT
python tools/blender/validate_glb.py --json OUTPUT.glb
```

The family manifest records the sidecar and source hashes, declared matrix, expected and actual Blender versions, exact bake and validator argv, every output hash, parsed validator report, per-member verdicts, and the aggregate verdict. It is written even for Blender discovery/version failures and after member failures, so red runs retain actionable evidence.

## Test

```text
npm --prefix tools/banso-blender test
```
