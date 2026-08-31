#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

class RenderPipeline;

// ===========================================================================
// framescan — deterministic "what's-in-frame" scan tool..
//
// Reads back the settled frame's G-buffer + back color buffer and computes a
// per-MATERIAL coverage + luminance summary, plus a water and a foliage tally,
// then writes a small JSON report. Run on a FIXED camera pose so the same world
// yields the same report every time (the tool is itself a pure function of the
// already-rendered frame — it issues NO draws and writes NO sim/world_hash
// state).
//
// MATERIAL ID SOURCE: the G-buffer normal/material attachment is RGBA8, and
// g_buffer.frag writes its ALPHA channel = float(MaterialID) / 255.0. Round-trip
// through the 8-bit alpha byte therefore recovers MaterialID exactly (the byte
// value IS the id). Coverage = fraction of frame pixels carrying that id;
// luminance = mean back-buffer luminance over those pixels (what the surface
// actually rendered to, post-lighting).
//
// WATER (backbuffer heuristic): live water (material id 7) is DISCARDED in
// g_buffer.frag, so it never appears in the id attachment — only the far-water
// sheet (id 200) does. Water coverage is therefore estimated from the back color
// buffer: a pixel counts as water where it is blue-dominant (b clearly > r) AND
// the G-buffer wrote no opaque solid id there (id == 0, i.e. the live-water
// forward pass / sky shows through), unioned with the far-water sheet (id 200).
//
// FOLIAGE: the instanced foliage scatter is a separate render pass whose
// instance count the pipeline already tracks; reported as a live-instance tally
// (it is alpha-keyed grass that does not get its own opaque material id).
// ===========================================================================
struct MaterialFrameStat {
    int id = 0;
    std::string name; // from data/common/materials.json (best-effort)
    std::uint64_t pixels = 0;
    double coverage = 0.0;       // pixels / total frame pixels
    double mean_luminance = 0.0; // mean back-buffer luminance over those pixels [0,1]
    double max_luminance = 0.0;  // brightest pixel of this material — catches spec/spark outliers
};

struct FrameScanReport {
    int width = 0;
    int height = 0;
    std::uint64_t total_pixels = 0;
    std::vector<MaterialFrameStat> materials; // sorted by coverage desc
    std::uint64_t water_pixels = 0;
    double water_coverage = 0.0;
    double water_mean_luminance = 0.0;
    std::uint64_t foliage_instances = 0; // live foliage scatter instances this frame
    double mean_frame_luminance = 0.0;   // whole back buffer
    bool ok = false;
};

// Reads back the CURRENT settled frame (caller must have rendered it and NOT yet
// drawn UI over the back buffer) and computes the scan. `materials_json` is the
// path to data/common/materials.json for id->name labels (optional; missing ->
// numeric ids only).: issues only glGetTexImage / glReadPixels, no
// draws, no GL state that survives the call.
FrameScanReport ScanFrame(const RenderPipeline& pipeline,
                          int framebuffer_width,
                          int framebuffer_height,
                          const std::filesystem::path& materials_json);

// Serializes the report to JSON and writes it to `out_path`. Returns false on a
// write failure. Creates parent directories.
bool WriteFrameScanReport(const FrameScanReport& report, const std::filesystem::path& out_path);

} // namespace Luminumbra::Rendering
