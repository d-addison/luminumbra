# Texture import

`asset_processor` converts image files into LTEX textures with a complete mip
chain. Select the filtering policy explicitly when building material maps:

```sh
asset_processor albedo.png albedo.ltex --srgb
asset_processor normal.png normal.ltex --normal-map
asset_processor metallic-roughness.png metallic-roughness.ltex --linear
asset_processor leaf.png leaf.ltex --srgb --alpha-cutoff 0.5
```

Color maps filter in linear light and retain sRGB-encoded RGB bytes. Normal maps
filter without gamma conversion and renormalize each mip. Linear data maps retain
their channel interpretation. These options do not change the LTEX v1 layout:
RGBA8 pixels in source row order, followed by successively halved mip levels.

`--alpha-cutoff` preserves the base level's binary alpha coverage while generating
mips from a color texture's own alpha channel. It accepts finite values from zero
through one and uses the specified threshold on quantized alpha bytes. Use the
same threshold in the consuming material. Normal maps and linear data maps refuse
this color-cutout option. A zero threshold keeps every texel and leaves alpha
unscaled. Mip zero retains authored alpha at native resolution.

Coverage at each mip is an approximation constrained by its number of texels. A
single texel cannot represent half coverage with a binary test. Coverage filtering
does not prove silhouette or final renderer fidelity; inspect cutout edges and LOD
transitions in the consuming renderer.

The existing `--alpha-mask opacity.png` option replaces alpha from a separate,
matching opacity image and preserves coverage at cutoff 0.5. Supplying
`--alpha-cutoff` alongside that mask selects a different threshold. Existing calls
without the new option retain their previous filtering arithmetic and binary
format. Invalid cutoff arguments are refused before replacing an output file.

An optional integer size resizes the source to a square before generating mips;
the coverage reference is that resized base level. Omit it to preserve native
dimensions. `--preview-png` writes the resulting base image beside the LTEX.
Standalone conversion writes loose files. Use the installed authoring service's
validated generations when a group of outputs needs atomic publication.
