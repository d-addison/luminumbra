# Game assets

The engine builds, runs its unit tests and starts its menu or headless server
without the game content pack. Entering a game world requires the authored tree
meshes and materials. The client refuses missing or corrupt content before
creating or replacing a world.

From the repository or packaged game root, run:

```sh
python3 tools/assets/acquire.py
```

On Windows, use `py -3` instead of `python3`. Restart the client after setup.
The pack is installed beneath `game-assets/tree-small-02/1.0.0/`; source releases
contain the manifest and acquisition tool, while art is versioned separately in
[luminumbra-game Releases](https://github.com/d-addison/luminumbra-game/releases).
Game code extraction into that repository is a subsequent change.

`config/game-asset-packs.json` pins each archive's URL, compressed and expanded
sizes, SHA-256 and every required file. The cache contains only verified archives.
Downloads have byte limits, a transfer deadline and socket timeouts. Extraction
rejects links, traversal, duplicate or unexpected files and excess expanded data.
Installation verifies all staged files before renaming the complete version
directory into place.

For offline setup, copy the exact archive from another machine and run:

```sh
python3 tools/assets/acquire.py --offline --archive tree-small-02-runtime-v1.0.0.tar.gz
```

An already verified installation or cached archive can be reused with `--offline`.
Use `--repair` to replace a corrupt installation; the previous directory is
preserved with a `.corrupt-` suffix. A failed transfer or extraction does not
replace installed content. These directories and the cache are excluded from Git.

The tree is Poly Haven's **Tree Small 02** by Rico Cilliers, distributed under CC0.
The game repository's source manifest records upstream identities and license
provenance. Its conversion tool verifies those source files, exports separate
trunk, branch and leaf meshes, applies the glTF texture-coordinate transform and
produces three mesh LODs. It records actual triangle counts because topology can
prevent a simplification target from being reached.

Runtime materials use sRGB albedo, linear normal and ARM maps, and a separate
source opacity mask for leaf cutouts. Texture mips preserve linear-light color,
normal direction and leaf coverage. The renderer bakes a full-sphere atlas for
distant trees; texture arrays and atlas handles are released on shutdown.
The QA launcher uses the same required game content and does not install
placeholder trees. Generic renderer unit tests may use explicit synthetic inputs.

Private audio recordings and Steam payloads are not included in public art packs.
