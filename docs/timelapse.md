# Timelapse & engine time-scale

A reusable pipeline for *showing change over time* — cloud/shade drift, smoke/fire particles,
creature behaviour, plant growth, erosion — by fast-forwarding the world and assembling captured
frames into a video.

Three composable pieces:

## 1. Engine time-scale (`g_timeScale`) — host_timescale-style

Source/GMod-like global time control in `main_client.cpp`:
- `1.0` = real time · `0` = paused · `<1` slow-mo · `>1` fast-forward.
- **Controls:** `[` slower · `]` faster (pause … 16x) · `\` reset to 1x; an F8-panel **Time scale**
  slider + 1x/Pause buttons; an always-on `TIME xN` / `PAUSED` HUD when not 1x.
- **Determinism-safe:** scales how many FIXED 30 Hz sim ticks run per real frame, NOT the tick dt.
  The tick sequence + per-tick `world_hash` are unchanged, so run==replay holds and the gates
  (which never change the scale) stay byte-identical. Render/client playback rate only — never hashed.
- Drives the whole sim each tick: creatures (instinct/perception/locomotion), weather, wind, aether,
  plant growth — i.e. "everything over time."

## 2. Frame capture (the loaded world → a `.ppm` sequence)

The **`--timelapse` capture mode** (LANDED) dumps a frame sequence of the live, settled world while
sim-time and the day clock fast-forward:

```
luminumbra_client_app.exe --auto-create-world --auto-enter-world --no-audio \
    --window-mode windowed --resolution 1280x720 \
    --timelapse-frames 24 --timelapse-ticks 90 --timelapse-tod 0 --timelapse-daystep 0.018 \
    --timelapse-dir build/tl
```

- `--timelapse-frames N` — how many frames to capture (then the client auto-exits).
- `--timelapse-ticks K` — EXTRA fixed sim ticks fast-forwarded between frames (weather/creatures/plants).
- `--timelapse-tod T` — starting time-of-day (**0 = noon/brightest**; applied during settle so frame 0 is lit).
- `--timelapse-daystep D` — advance time-of-day by D per frame for shade/sky drift (0 = hold).
- `--timelapse-dir P` — output directory (default `<artifact-dir>/timelapse`).

Notes: it waits ~45 frames for the world to stream/settle before frame 0; **physics keeps running**
each frame so the player settles on the ground (do NOT pause it). Run WITH ImGui (omit `--no-ui`) —
the non-scenario load path asserts without an ImGui context; in-game overlays are auto-suppressed
during capture so frames are clean.

## 3. Assembler — `tools/timelapse.py`

Turns a frame directory into a video:

```
python tools/timelapse.py <frames_dir> -o out.gif [--fps 12] [--scale 0.3]
                                        [--pattern "*.ppm"] [--mp4 out.mp4]
python tools/timelapse.py --selftest        # headless check (TimelapseSelftest ctest)
```

- Natural-sorted by frame index; .ppm (engine native) or png/jpg; optional downscale + fps.
- Always writes an animated GIF (Pillow); also an MP4 when `ffmpeg` is on PATH (much smaller).

### Example (assemble an existing capture)
```
python tools/timelapse.py build/debug/test-artifacts/runtime/<scenario>/motion \
    -o build/clip.gif --fps 20 --scale 0.28 --mp4 build/clip.mp4
```

## End-to-end (intended)
1. Launch the client to a loaded world.
2. Set a high time-scale (fast-forward) so day/night, weather, creatures, plant growth visibly evolve.
3. Capture the frame sequence (showcase-video dump, or the `--timelapse` mode when it lands).
4. `python tools/timelapse.py <dir> -o clip.mp4 --mp4 clip.mp4`.
