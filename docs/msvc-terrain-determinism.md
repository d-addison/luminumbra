# MSVC terrain-height contraction investigation

At `a360460`, local MSVC 19.50.35727.0 failed the existing five synthetic
height-grid pins and the shipped archipelago pin. CI MSVC 19.51.36256.0 passed
them. Repeated sampling and scalar/grid agreement within each build passed;
changing expectations would have hidden the compiler-dependent behavior.

The preserved diagnostics contain 4,096 positions per fixture and six float-bit
columns: terrain height, base noise, base height, island noise, island mask and
final height. The largest sampled height difference across six fixtures was
0.0012435913 m. This investigation is separate from the earlier AVX reciprocal
and square-root repair, which corrected cave occupancy.

## Controls and cause

| Control | Result |
|---|---|
| Exact CI executable plus its three MSVC CRT DLLs, run locally | All five portable diagnostic JSON files byte-identical to CI |
| Local executable with the CI CRT DLLs | Original local sample bits retained |
| CI executable with the local CRT DLLs | Original CI sample bits retained |
| Rebuilt AVX2 object with original flags, all other objects fixed | Original local sample bits retained |
| Same object with only `/fp:precise` appended | Some FBm samples changed; the island mismatch remained; rejected as a fix |
| Explicit CI-order Simplex 2D operations, original flags | Every sample bit in all five portable grids matched CI |

Independent Astra/xhigh review recovered the Simplex 2D methods through PE RTTI,
vtables and linker maps. MSVC 19.51 contracts skew/unskew multiply-add operations;
19.50 keeps those operations separate. An exact binary32 evaluation reproduces
all 4,096 local island samples and all 4,096 CI samples. Unskew contraction alone
accounts for all 303 differences on that grid. Skew contraction changes none on
that particular grid, but its operation order is also explicit in the correction.

The established MSVC AVX2 order is:

```text
sum = x + y
cellX = floor(fma(F2, sum, x))
cellY = floor(fma(F2, sum, y))
cellSum = cellX + cellY
localX = x - fma(-G2, cellSum, cellX)
localY = y - fma(-G2, cellSum, cellY)
```

The integer cell/hash calculations remain between skew and unskew. The source
uses the existing FastSIMD fused helpers. Only the MSVC AVX2 branch of the first
Simplex 2D overload changes. Other SIMD levels, non-MSVC expressions, 3D/4D
Simplex, OpenSimplex, Fractal and the precise AVX boundary remain unchanged.
Static comparison also establishes that local and CI FBm already fuse their
accumulation in the same order; no Fractal change is justified.

## Verification and scope

The isolated experiment constructs a fresh 13-member FastNoise archive, replaces
exactly one AVX2 object, checks archive payloads and selected linker-map members,
and retains the other linked inputs. All 1,031 protected original input hashes
were unchanged afterward. A copied header tree differs only in Simplex.inl.
The normal production build subsequently passed all 33 native terrain/player
tests with zero skips. All six diagnostic grids, including the shipped
archipelago, match the existing CI sample bits. Height, topology and serialization
expectations were not edited.

The causal receipts retain executable, library, compiler, command and input
identities; original and rejected experiments remain preserved in the private
release campaign. [Issue #115](https://github.com/d-addison/luminumbra/issues/115)
tracks the final committed CI/native/package acceptance. This bounded source
verification is not release approval or a universal cross-compiler bitwise claim.
