# Visual regression: FLIP-style golden-image harness

`tools/flip_diff.py` is a perceptual image-diff gate. It compares a **candidate**
render against a blessed **golden** reference and produces:

1. a scalar perceptual **error score** in `[0,1]` (`0` == identical),
2. a per-pixel **heatmap PNG** that localizes *which region* regressed, and
3. a **pass/fail** verdict against a threshold (non-zero exit on fail) for CI.

It backs two things:

- **General visual-regression gate** for any render-only change. Render-only
  changes never touch `world_hash`; the pixels are the contract, so we guard
  them with a golden image instead.
- **GL -> Vulkan parity harness.** Render the same deterministic frame on the GL
  backend, bless it as the golden, render it on Vulkan, and `flip_diff` the
  Vulkan candidate against the GL golden. The heatmap localizes the regressed
  render pass (a wrong shadow term lights up the shadowed geometry; a tone-map
  drift lights up the whole frame uniformly), so you fix the pass, not guess.

## Workflow

### 1. Capture a candidate

The engine already dumps deterministic frames to PPM/PNG. Use whichever capture
matches what you're guarding (run from the build that has the binary):

```sh
# A scripted gameplay frame / timelapse frame:
luminumbra --timelapse ...        # writes PPM/PNG frames
# A what's-in-frame deterministic headless frame:
luminumbra --frame-scan ...       # dumps an image
# A UI screen:
luminumbra --ui-screenshot world_creation   # writes a PPM
```

PPM captures convert to PNG with `tools/ppm_to_png.py` if you want a PNG golden,
but `flip_diff.py` reads **both PNG and binary P6 PPM directly** (pure-stdlib
decoder), so you can diff a `.ppm` capture against a `.ppm` golden with no
conversion step.

### 2. Bless the first golden

The very first time, promote the candidate to be the reference:

```sh
python tools/golden_update.py captures/world_creation.ppm goldens/world_creation.ppm
# (or)   python tools/flip_diff.py --update <candidate> <golden>
```

`golden_update.py` refuses to overwrite an existing golden unless you pass
`--force`, so a golden is never silently re-blessed. A regression must be
reviewed before its new look becomes the reference. `--diff` prints the
perceptual delta old-golden-vs-candidate before you overwrite.

### 3. Diff a candidate against the golden (the gate)

```sh
python tools/flip_diff.py captures/world_creation.ppm goldens/world_creation.ppm \
       --out artifacts/world_creation_heatmap.png \
       --threshold 0.05 \
       --json artifacts/world_creation_flip.json
```

- Exit `0` => PASS (mean perceptual error `<=` threshold).
- Exit `1` => FAIL (over threshold). The heatmap localizes the regressed region.
- Exit `2` => usage / I/O error (e.g. size mismatch, missing file).

The `--out` heatmap uses a blue -> cyan -> green -> yellow -> red ramp:
deep blue = no error, red = maximum error. Open it next to the candidate to see
**which render pass** moved.

### 4. Re-bless after an intentional render change

When a render change is *intended* (e.g. a deliberate lighting overhaul), the
gate will FAIL — that's correct. Review the heatmap, confirm the new look is
the desired one, then re-bless:

```sh
python tools/golden_update.py captures/scene.ppm goldens/scene.ppm --force --diff
```

## Backends (auto-detect + graceful degrade)

`flip_diff.py` picks the strongest metric available and **prints + records which
backend judged the frame** (so a regression is never silently scored by a weaker
metric than you assume). Force one with `--backend {flip,ssim,luma,stdlib}`.

| Tier | Backend | How to get it | Notes |
|------|---------|---------------|-------|
| 1 | **NVIDIA FLIP** | `pip install flip-evaluator` | The real perceptual metric; the GL->Vulkan parity standard. |
| 2 | **SSIM** | `pip install scikit-image` | Structural similarity; error = `1 - SSIM`. Solid approximation. |
| 3 | **luma+gradient** | numpy only (already present) | Blends absolute-luma error, Sobel-gradient (edge/structure) error, and per-channel color error. No SciPy. |
| 4 | **stdlib** | nothing | Mean absolute RGB error. Always runnable, even with no numpy. |

Recommendation: `pip install flip-evaluator` on CI so the gate uses the true
FLIP metric for GL->Vulkan parity. The fallbacks keep the gate **runnable
anywhere** (dev boxes without the optional deps) rather than hard-failing on a
missing dependency.

Thresholds are per-backend in spirit (FLIP/SSIM/luma are not the same scale).
Pin a threshold against a known-good vs known-bad pair when you add a new golden;
`0.05` is a reasonable starting point for FLIP/luma.

## Self-test

`flip_diff.py --selftest` synthesizes an image, diffs it **against itself on
every available backend** (must score `0`), confirms a deliberately perturbed
copy scores `> 0`, and round-trips a heatmap. Run it in CI to prove the harness
itself is healthy before trusting its verdicts:

```sh
python tools/flip_diff.py --selftest
```

## Determinism note

This harness is **render-only tooling** — it touches no sim/worldgen code and
therefore does not affect `world_hash`. The legacy default preset stays
byte-identical (`--smoke == 6f008a9f637c40b7`). The golden images live under a
`goldens/` tree you choose; they are not part of the deterministic sim contract,
they are the *visual* contract that sits alongside it.
