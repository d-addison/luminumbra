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

The engine already dumps `glReadPixels(GL_BACK)` frames as `motion/frame_%03d.ppm` via
`WritePixelBufferPpm` (the showcase-video / storm-motion scenarios). A timelapse capture runs this
on the **settled, loaded** world while time advances (set a high `g_timeScale`, or bulk-advance the
sim between captures). Capturing during world load gives a dark/streaming clip — wait for the world
to be ready first.

> Status: the dedicated `--timelapse` capture mode (wait-for-loaded → advance sim-time per frame →
> dump) is the remaining slice; until it lands, use the existing showcase-video frame dumps + a high
> `g_timeScale`.

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
