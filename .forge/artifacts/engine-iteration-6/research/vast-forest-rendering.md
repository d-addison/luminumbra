# Rendering Vast Forests in AAA Engines — Cited Research Brief

**Context:** C++20 / OpenGL 4.5 engine, RTX 5070 Ti @ 300 fps (≈3.3 ms total frame budget), 6x+ view distance,
**procedurally generated** trees (space-colonization branches + leaf cards built at runtime from a genome — NOT baked artist meshes).
Goal: render tens of thousands → millions of trees within a few ms/frame and bounded VRAM.

**Date:** 2026-06-19. Author: research dispatch (engine-iteration-6).

---

## TL;DR (the recommended pipeline)

For runtime-procedural trees the universally-used AAA pattern is **bake-once, instance-many**:

1. **Generate a small PALETTE of K distinct tree meshes once** (per species/biome), not unique geometry per tree.
   The forest then becomes K mesh archetypes instanced thousands of times with cheap per-instance data (transform + tint + phase).
2. Each palette entry ships with **3 discrete mesh LODs** + a **far-field IMPOSTOR** (multi-view billboard atlas).
3. **Crossfade / dithered transitions** between LODs and into the impostor to kill popping.
4. **GPU-driven culling (frustum + distance, multi-draw-indirect)** is the AAA standard once instance counts pass ~10k and especially with a 6x view distance — it is worth doing for this engine.

This is exactly the SpeedTree + Far Cry 4 + Horizon Zero Dawn model, generalized to procedural source geometry.

---

## 1. GPU instancing for vegetation — palette + per-instance data

**The pattern.** A forest is rendered as a **small palette of meshes instanced many times**, each instance carrying only a
compact per-instance record (transform + a little variation), rather than unique geometry per tree. This is the foundation of
every shipping AAA vegetation system; unique per-tree geometry is never streamed — only per-instance *placement* is.

- **Far Cry 4 (Stephen McAuley, GDC 2015, "Rendering the World of Far Cry 4")** builds trees from SpeedTree as
  **trunk meshes + leaf-cluster meshes**, instanced across the world. LOD0 meshes are heavy (e.g. larch trunk ≈5,564 verts,
  rosewood leaves ≈169,076 verts), so they are *only* rendered for the handful of nearest trees; aggressive culling caps
  realistic on-screen complexity at ≈80k verts for the most complex tree. The rest of the forest is cheaper LODs and impostors.
  Source: *GDC 2015, Stephen McAuley, Ubisoft — "Rendering the World of Far Cry 4."*
- **Horizon Zero Dawn (Jaap van Muijden, GDC 2017, "GPU-Based Run-Time Procedural Placement")** places vegetation
  **on the GPU at runtime** from artist-authored placement rules — data-driven, deterministic, locally stable. Instances are
  generated GPU-side around the player each frame; the CPU never touches per-tree data. Foliage motion is driven by a global
  wind force-field compute shader (~150 µs) — i.e. per-instance animation is a shader function of position+time, not stored per tree.
  Source: *GDC 2017, Guerrilla Games — "GPU-Based Run-Time Procedural Placement in Horizon Zero Dawn."*

**Typical palette size.** AAA forests use on the order of **a few to a few dozen distinct base meshes per biome**
(species × a couple of age/size variants), then derive visual variety from **per-instance transform + tint + random phase**,
plus optional sub-mesh swaps. The variety the eye perceives comes overwhelmingly from instance-level randomization (yaw, scale,
slight tilt, hue/value tint, wind phase), not from unique meshes. SpeedTree's authoring model (one tree definition → many
randomized instances) encodes the same assumption.
Source: *SpeedTree SDK docs — Level of Detail; Far Cry 4 GDC 2015.*

**Per-instance data layout (recommended for GL 4.5).** Keep the per-instance record tiny and 16-byte aligned so a large
forest's instance buffer stays small (a 1M-tree forest at 32 B/instance = 32 MB):

```
struct TreeInstance {            // 32 bytes
    float3 position;             // world XZ + terrain Y
    half2  scaleXZ_scaleY;       // anisotropic size (genome maturity cue)
    half   yawSin, yawCos;       // packed rotation (cheaper than a quat for a tree)
    uint   tintPacked;           // RGBA8 species/age tint
    half   windPhase;            // per-instance wind offset
    uint   paletteIndexLodBias;  // which palette mesh + per-instance LOD nudge
};
```

Bind this as an instanced vertex attribute stream (`glVertexAttribDivisor`) or, for GPU-driven paths, as an SSBO indexed by
`gl_InstanceID + gl_BaseInstance`. **Do not** store full 4×4 matrices per instance (64 B) at forest scale — reconstruct the
model matrix in the vertex shader from position/scale/yaw.

---

## 2. LOD strategy — discrete mesh LODs → impostors, with crossfade

**The canonical ladder** (close → far): a few discrete mesh LODs, then a far-field **impostor** (billboard / octahedral),
with smooth blends between every step.

- **SpeedTree LOD model.** 3D trees render up close; geometric resolution is reduced smoothly with distance; finally the tree
  **fades into a billboard** that matches its lighting. Crucially, the billboard is **multi-view**: several billboard images are
  captured around the tree, and the shader picks the one matching the camera azimuth + the instance's rotation, so the far-field
  tree still rotates correctly. The 3D→billboard handoff uses **alpha-to-coverage**: the 3D LOD's alpha fades out while the
  billboard fades in — a smooth, dither-free transition that avoids popping.
  Source: *SpeedTree SDK Documentation — "Level of Detail" and "Billboards."*
- **Far Cry 4** uses **3 mesh LODs per tree (trunk + leaf clusters) before the tree becomes an impostor.** The impostor is a
  **9-view G-buffer capture**: 8 perpendicular side views + 1 top-down, each storing albedo/normal/material. The impostor is a
  **tessellated 16×16 depth-displaced billboard** (depth from the capture displaces the grid), which dramatically improves
  silhouette and side-lighting vs a flat card and reduces the "cardboard" look at the LOD boundary.
  Source: *GDC 2015, Stephen McAuley — "Rendering the World of Far Cry 4."*
- **Octahedral / hemi-octahedral impostors** (the modern successor to fixed N-view billboards). Capture the tree from a grid of
  views arranged on an octahedron and store them in a texture atlas; at runtime map the view direction → atlas UV with cheap
  octahedral math (no trig) and **blend the nearest 3 frames** for a near-continuous parallax-correct far-field tree.
  - Practical config (Godot/UE-style bakers): **16×16 frame grid into a ~2048² atlas**, with albedo + normal + depth maps
    (+ optional ORM). **Hemi-octahedron is preferred for foliage** (the bottom hemisphere is wasted for ground-planted trees,
    so hemi roughly doubles the useful side-view resolution).
  - Caveat: octahedral impostors exhibit parallax error up close — **use them only in the far field**; keep a real mesh LOD for
    mid distances.
  Sources: *Ryan Brucks (Epic), "Octahedral Impostors," shaderbits.com 2018; Godot-Octahedral-Impostors (wojtekpil) — "16 is the
  recommended grid value, 2048 recommended atlas, hemisphere better for standard foliage"; yummers.dev "Hemi-Octahedral Impostors."*
- **UE5 Nanite Foliage (5.7+)** is the emerging alternative: virtualized geometry streams only pixel-visible triangles
  (a 500k-tri tree may draw ~200 tris at 500 m) with **no authored LODs**, using aggregate **voxels** for distant foliage instead
  of billboards. Powerful but **not portable to GL 4.5** — listed for completeness, not as our path.
  Source: *Epic — "Nanite Foliage," UE 5.7/5.8 documentation.*

**Anti-popping.** Two industry-standard techniques, both cheap on RTX-class hardware:
1. **Alpha-to-coverage crossfade** (SpeedTree) for LOD↔impostor — outgoing alpha ramps down, incoming ramps up over a short
   distance band, MSAA resolves it smoothly.
2. **Dithered (screen-door) LOD fade** — per-pixel `discard` against a Bayer/blue-noise threshold driven by the fade factor;
   works without MSAA and is the common choice for deferred/TAA pipelines (TAA dissolves the dither). Use a small overlap band
   (e.g. ±10–15% of the LOD distance) so both LODs are drawn only briefly.

---

## 3. GPU-driven culling — frustum + distance, multi-draw-indirect

**What it is.** Move per-instance frustum + distance culling and LOD selection into a **compute shader** that writes a
**compacted list of survivors** and per-mesh **indirect draw-argument buffers**, then issue a single
`glMultiDrawElementsIndirect` (or `glDrawElementsInstancedIndirect` per palette mesh). The CPU issues O(K) draws regardless of
tree count; the GPU decides what's visible and at which LOD.
Sources: *vkguide.dev "Compute-based Culling / GPU-Driven Engines"; Imagination "GPU-Controlled Rendering Using Compute &
Indirect Drawing"; ellioman "Indirect-Rendering-With-Compute-Shaders" (frustum + Hi-Z occlusion + screen-size/distance cull +
GPU LOD selection via DrawMeshInstancedIndirect).*

**Is it worth it for ~10–50k trees?** Yes, for this engine — here's the break-even reasoning:

- **Assassin's Creed Unity (SIGGRAPH 2015, "GPU-Driven Rendering Pipelines," Haar & Aaltonen, Ubisoft)** is the canonical proof:
  moving culling/draw-submission to the GPU gave **1–2 orders of magnitude fewer CPU draw calls while rendering ~10× more objects**.
  Source: *SIGGRAPH 2015 Advances in Real-Time Rendering — Ulrich Haar & Sebastian Aaltonen, "GPU-Driven Rendering Pipelines."*
- **Break-even, practically:**
  - **< ~2–5k instances**, single thread, CPU-side frustum cull + plain instanced draws is fine; GPU-driven adds complexity for
    little gain.
  - **~10k+ instances, OR a 6x view distance, OR many palette meshes (K large), OR per-frame placement** → GPU-driven wins,
    because (a) CPU cull cost grows linearly with instance count and (b) distance culling + per-instance LOD selection are
    trivially parallel on the GPU. With a 6x draw distance the *potential* instance set explodes, so doing the cull where the
    data already lives (VRAM) avoids a CPU readback/upload round-trip entirely.
  - At a 300 fps target the **frame budget is ~3.3 ms total**, so spending CPU time looping over 50k instances per frame is a
    direct threat to the budget; a compute cull pass is a few hundred µs and runs async with other GPU work.

**Recommendation:** implement CPU instancing first (simplest, get the forest on screen), then add the GPU-driven cull/compaction
path behind a flag once instance counts or view distance push past the break-even. The instance-buffer layout in §1 is
deliberately SSBO-friendly so the upgrade is additive, not a rewrite.

---

## 4. Memory / perf budget

**Per-frame time.** AAA targets keep *all* vegetation inside a small slice of frame time. Far Cry 4's whole scene budget was
23.4 ms (PS4, 30 fps); on an RTX 5070 Ti at 300 fps you have ~3.3 ms total, so a realistic vegetation budget is **~0.3–0.8 ms**:
- GPU cull/compaction compute pass: ~0.1–0.3 ms for tens of thousands of instances.
- Mesh-LOD draws (near trees only — a few hundred to low thousands): bounded by triangle throughput, kept small by aggressive
  distance LOD (Far Cry 4 capped ≈80k verts even for the most complex tree).
- Impostor draws (the vast majority of the forest): each impostor is ~2 tris (or a 16×16 depth grid à la Far Cry 4); thousands
  of impostors are a single instanced indirect draw and cost almost nothing on the vertex side — the cost is overdraw + atlas
  sampling, mitigated by alpha-test/coverage and a tight atlas.

**VRAM.**
- **Instance buffer:** ~32 B/instance (§1). 100k trees = 3.2 MB; 1M = 32 MB. Negligible.
- **Mesh palette:** K base meshes × 3 LODs. Procedural trees can be heavy at LOD0 (FC4: leaves up to ~169k verts), so keep K small
  and LOD0 budgeted. K=12 species × ~80k verts LOD0 + cheap LOD1/2 ≈ tens of MB of vertex/index data — fine.
- **Impostor atlases:** the main vegetation VRAM cost. A 16×16 octahedral atlas at 2048² with albedo+normal+depth (BC7/BC5/BC4)
  ≈ **8–12 MB per species**. **K=12 species ≈ 100–150 MB** — bounded and acceptable on a 16 GB card. Knobs to shrink it:
  fewer frames (8×8) for distant-only species, smaller atlas (1024²) for small plants, drop ORM (use the "Light" profile).
  Sources: *Godot-Octahedral-Impostors (2048² / 16×16 / hemisphere defaults); Ryan Brucks shaderbits 2018.*

**Sizing rule of thumb:** total vegetation VRAM ≈ instance buffers (single-digit MB) + palette meshes (tens of MB) +
impostor atlases (≈10 MB × K). Keep K bounded and the whole forest stays in low-hundreds-of-MB.

---

## 5. Concrete pipeline for runtime-PROCEDURAL trees

The procedural twist doesn't change the rendering pattern — it changes **when** geometry is built. Generate the palette *once*
(at world-gen / species-bake time, deterministic from the genome), then instance like any AAA forest. **Never** build unique
geometry per tree at render scale.

**Recommended pipeline:**

1. **Bake a palette of K archetype meshes per species/biome, once.** From the genome, run space-colonization + leaf-card
   placement to produce **K distinct trees per species** (different seeds → different silhouettes). Suggested
   **K = 8–16 per species**, ~4–8 species per biome → on the order of **32–128 total base meshes**, which matches AAA's
   "few dozen meshes per biome" practice. (Tune K up if banding/repetition is visible, down if VRAM/bake-time is tight.)
2. **Generate 3 mesh LODs per archetype** at bake time: LOD0 full branches + leaf cards; LOD1 decimated branches + merged leaf
   cards; LOD2 trunk + a few billboard-leaf clusters. (Far Cry 4: 3 LODs before impostor.)
3. **Bake one impostor atlas per archetype:** hemi-octahedral, **16×16 frames into 2048²**, storing albedo + packed normal + depth,
   3-frame blended at runtime. (Far Cry 4 proves even 9 views + depth-displacement reads convincingly; 16×16 octahedral is the
   modern, smoother default.) Use 8×8/1024² for small/short plants.
4. **Instance at runtime** with the 32-byte per-instance record (§1); derive yaw/scale/tint/wind-phase from a deterministic
   per-instance hash so the sim stays integer/deterministic and the *visual* variety is shader-side.
5. **Cull + LOD-select on GPU** (compute → compacted survivor list + indirect args → `glMultiDrawElementsIndirect`) once counts
   or the 6x view distance justify it; CPU instancing path first.

**Suggested LOD distance bands** (starting points — tune against the 6x view-distance and the 3840×1600 display; scale by
screen-space size, not raw distance, so it's resolution-independent):

| Band      | Representation                          | Approx. distance (tune!) |
|-----------|-----------------------------------------|--------------------------|
| LOD0      | Full procedural mesh (branches + cards) | 0 – 30 m                 |
| LOD1      | Decimated mesh                          | 30 – 80 m                |
| LOD2      | Trunk + billboard leaf clusters         | 80 – 150 m               |
| Impostor  | Hemi-octahedral multi-view billboard    | 150 m – far cull         |
| Cull      | Not drawn                               | beyond view distance × 6 |

Transitions: **dithered/blue-noise crossfade with TAA**, or **alpha-to-coverage** (SpeedTree-style) if MSAA, over a ~10–15%
overlap band at each boundary. Prefer **screen-space-size thresholds** (e.g. switch to impostor when the tree projects to
< ~32–48 px tall) so the bands above auto-adapt to FOV and resolution.

**GPU-driven culling — worth it now?** Yes, behind a flag. With a 6x view distance and forests in the tens of thousands of
instances, the *potential* set is large and the 300 fps budget (~3.3 ms) leaves no room for per-frame CPU instance loops.
Ship CPU instancing first to get pixels, then turn on the compute cull/compaction + MDI path; the §1 SSBO-friendly layout makes
that an additive change.

---

## Sources (names / years)

- **Ubisoft — Stephen McAuley, "Rendering the World of Far Cry 4," GDC 2015.** (3 tree LODs → impostor; 9-view G-buffer impostor
  capture, 8 side + 1 top; tessellated 16×16 depth-displaced billboards; ~80k-vert cull cap; SpeedTree-based trunk+leaf instancing.)
- **Guerrilla Games — Jaap van Muijden, "GPU-Based Run-Time Procedural Placement in Horizon Zero Dawn," GDC 2017.**
  (GPU-side deterministic instance placement around player; global wind force-field compute ~150 µs; data-driven rules.)
- **Ubisoft — Ulrich Haar & Sebastian Aaltonen, "GPU-Driven Rendering Pipelines," SIGGRAPH 2015 (Advances in Real-Time Rendering);
  Assassin's Creed Unity.** (Compute cull + MDI: 1–2 orders of magnitude fewer CPU draw calls, ~10× more objects.)
- **SpeedTree SDK Documentation — "Level of Detail" and "Billboards."** (Smooth geometric LOD reduction; multi-view billboard
  array selected by camera azimuth + instance rotation; alpha-to-coverage 3D→billboard crossfade.)
- **Ryan Brucks (Epic Games) — "Octahedral Impostors," shaderbits.com, 2018.** (Octahedral/hemi-octahedral capture grid into atlas;
  frame blending; near-distance parallax caveat.)
- **wojtekpil — "Godot-Octahedral-Impostors" (GitHub).** (Concrete defaults: 16×16 frame grid, 2048² atlas, hemisphere preferred
  for foliage; albedo/normal/depth/ORM maps; Light vs Standard profiles.)
- **yummers.dev — "Hemi-Octahedral Impostors."** (Hemi vs full octahedron; ~8×8 too few frames; increase resolution for side views.)
- **NVIDIA — "GPU Gems 3, Ch. 16: Vegetation Procedural Animation and Shading in Crysis," 2007.** (CryEngine 2 vegetation:
  procedural animation, shading, distant sprite/billboard generation as the far-field representation.)
- **Epic Games — "Nanite Foliage," Unreal Engine 5.7 / 5.8 Documentation, 2025–2026.** (Virtualized geometry + aggregate voxels for
  distant foliage; no authored LODs — noted as the modern alternative, NOT portable to GL 4.5.)
- **vkguide.dev — "GPU-Driven Engines / Compute-based Culling"; Imagination Technologies — "GPU-Controlled Rendering Using Compute
  & Indirect Drawing"; ellioman — "Indirect-Rendering-With-Compute-Shaders" (GitHub).** (Compute frustum + Hi-Z occlusion +
  distance/screen-size cull, instance compaction, GPU LOD selection, indirect draw.)
- **Kojima Productions — Death Stranding / Decima vegetation, GDC talk.** (Leaf translucency, wind motion, distance-based LOD
  switching across multiple data structures — corroborates the discrete-LOD + distance-switch model on Decima.)
