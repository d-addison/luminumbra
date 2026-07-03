# Stranded working-tree diffs (preserved, reverted from the tree)

Spec 021 Wave A, item FOLIAGE-08: uncommitted visual state must not ride the working tree.

- `grass-overhaul-2026-06.patch` — an unowned "grass overhaul" tuning set found riding the
  tree during the spec-021 audit: `data/common/foliage/scatter_set.json` (brighter/warmer
  blade colors, wider + shorter cards) and `data/common/materials.json` (grass albedo_tint
  blue response 0.90 → 0.74 so sky-ambient stops washing grass toward teal). Both are
  render-only. Reverted 2026-07-02 because landing them requires a deliberate
  WorldVisualSweep re-bless, which was blocked by the capture hang (RENDER-01) when they
  were authored.

To evaluate/land it after RENDER-01 is green:

```
git apply docs/audit/021/stranded/grass-overhaul-2026-06.patch
# capture, review via tools/flip_diff.py heatmap, then commit WITH the re-bless
# (WorldVisualSweep rerun, no NEW flags) or discard.
```
