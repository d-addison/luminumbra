#!/usr/bin/env python3
"""Device-free regressions for shader scene evidence, metrics and CI routing."""
import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


scene = module('shader_scenes', 'tools/gates/shader-scenes.py')
paths = module('scene_paths', 'tools/ci/ci_paths.py')
software = module('scene_software', 'tools/static_preview/run_software_gl.py')
wiring = module('upload_wiring', 'test/rendering/test_upload_budget_wiring.py')
read, compact, body = wiring.read, wiring.compact, wiring.body
MANIFEST = ROOT / 'test/rendering/shader_scenes/scenes.json'


class SkyWarmGradeTests(unittest.TestCase):
    """CPU grade evaluation; no graphics context or replacement golden images.

    The source contract pins the mirrored expressions. The shipping helper's
    rolloff assignment selects compression, so reverting it mutates numeric tests.
    These analytical dome inputs isolate the grade, not LUT/cloud rendering.
    """

    @classmethod
    def setUpClass(cls):
        cls.shader = (ROOT / 'res/shaders/enhanced_skybox.frag').read_text(encoding='utf-8')
        helper = cls.shader.split('vec3 preserveSkyLuminance(', 1)[1].split('void main()', 1)[0]
        helper = re.sub(r'//[^\n]*', '', helper)
        cls.rolloff = 'saturation /= 2.0 - saturation;' in helper

    @staticmethod
    def smoothstep(low, high, value):
        value = min(1, max(0, (value - low) / (high - low)))
        return value * value * (3 - 2 * value)

    @staticmethod
    def luma(color):
        return sum(c * weight for c, weight in zip(color, (.2126, .7152, .0722)))

    @staticmethod
    def transmittance(sun_y):
        # Mirror the ground row's two bilinear texels in the production 256x64
        # transmittance compute LUT: 2 texels * 40 midpoint samples, bounded.
        column = min(255, max(0, (sun_y + 1) * .5 * 256 - .5))
        first = int(column)

        def texel(index):
            radius = 6360000 + .5 * 100000 / 64
            mu = (index + .5) / 256 * 2 - 1
            distance = -radius * mu + math.sqrt(
                (radius * mu) ** 2 - radius ** 2 + 6460000 ** 2)
            step = distance / 40
            rayleigh = mie = 0
            for sample in range(40):
                along = step * (sample + .5)
                altitude = max(0, math.sqrt(radius ** 2 + along ** 2
                                           + 2 * radius * mu * along) - 6360000)
                rayleigh += math.exp(-altitude / 8000) * step
                mie += math.exp(-altitude / 1200) * step
            return tuple(math.exp(-beta * rayleigh - 4.4e-6 * mie)
                         for beta in (5.802e-6, 13.558e-6, 33.1e-6))

        left, right = texel(first), texel(min(first + 1, 255))
        return tuple(a + (b - a) * (column - first) for a, b in zip(left, right))

    def tint(self, transmittance, sun_y, day_factor, elevation, azimuth):
        peak = max(max(transmittance), 1e-4)
        hue = tuple(min(1, max(0, c / peak)) ** exponent
                    for c, exponent in zip(transmittance, (1, 1.45, 2.6)))
        horizon = 1 - .55 * self.smoothstep(.25, 1, math.sin(elevation))
        sun_dot = (math.sin(elevation) * sun_y + math.cos(elevation)
                   * math.sqrt(1 - sun_y ** 2) * math.cos(azimuth))
        toward = .80 + .20 * min(1, max(-1, sun_dot))
        low_sun = 1.05 + .40 * (1 - self.smoothstep(.10, .85, sun_y))
        warmth = min(1, max(0, horizon * toward * low_sun))
        warmth *= self.smoothstep(0, .22, day_factor)
        return tuple(1 + (c - 1) * warmth for c in hue)

    def candidate(self, color, tint, day_factor=0, sun_y=1, night_fade=1, use_lut=True):
        graded = tuple(c * t for c, t in zip(color, tint)) if use_lut else color
        if use_lut:
            bright_dome = self.smoothstep(.08, .40, self.luma(color))
            low_sun_gate = 1 - self.smoothstep(.55, .78, sun_y)
            warm_green = .75 * graded[0] + .25 * min(graded[0], graded[2])
            green_lead = max(graded[1] - max(warm_green, graded[2]), 0)
            graded = (graded[0], graded[1] - green_lead * bright_dome * low_sun_gate
                      * min(1, max(0, night_fade)), graded[2])
        return tuple(c ** (1 + (exponent - 1) * day_factor)
                     for c, exponent in zip(graded, (.9, .95, 1.05)))

    def preserve_luminance(self, color, graded):
        color = tuple(min(1, max(0, c)) for c in color)
        graded = tuple(max(0, c) for c in graded)
        luminance, graded_luminance = self.luma(color), self.luma(graded)
        if graded_luminance <= 0:
            return color
        chroma = tuple(c * (luminance / graded_luminance) - luminance for c in graded)
        saturation = 1
        for channel in chroma:
            if channel > 0:
                saturation = min(saturation, (1 - luminance) / channel)
            elif channel < 0:
                saturation = min(saturation, -luminance / channel)
        # The source contract pins the complete consumed helper, including the
        # branch and bounds. Its assignment selects rolloff versus the old edge.
        if self.rolloff and saturation < 1:
            saturation /= 2 - saturation
        return tuple(min(1, max(0, luminance + saturation * c)) for c in chroma)

    def grade(self, color, tint, **kwargs):
        # Section 7 clamps ACES before constructing the candidate, including HDR.
        color = tuple(min(1, max(0, c)) for c in color)
        return self.preserve_luminance(color, self.candidate(color, tint, **kwargs))

    def test_grade_source_contract(self):
        source = re.sub(r'//[^\n]*', '', self.shader)
        source = ' '.join(source.split())
        for expression in (
                'aerialTrans / max(aerialNorm, 1e-4)',
                'pow(clamp(aerialHue, vec3(0.0), vec3(1.0)), vec3(1.0, 1.45, 2.6))',
                '1.0 - 0.55 * smoothstep(0.25, 1.0, max(viewDir.y, 0.0))',
                '0.80 + 0.20 * clamp(dot(viewDir, u_sunDirection), -1.0, 1.0)',
                'mix(1.05, 1.45, 1.0 - smoothstep(0.10, 0.85, u_sunCosZenith))',
                'smoothstep(0.0, 0.22, dayFactor)',
                'clamp(aerialHorizonF * aerialTowardSun * aerialLowSun, 0.0, 1.0) * aerialNightFade',
                'vec3 tint = mix(vec3(1.0), aerialHue, clamp(aerialWarm, 0.0, 1.0));',
                'skyColor = clamp(skyColor * (2.51 * skyColor + 0.03) '
                '/ (skyColor * (2.43 * skyColor + 0.59) + 0.14), 0.0, 1.0);',
                'vec3 graded = skyColor;',
                'graded *= tint;',
                'float brightDome = smoothstep(0.08, 0.40, '
                'dot(skyColor, vec3(0.2126, 0.7152, 0.0722)));',
                'float lowSunGate = 1.0 - smoothstep(0.55, 0.78, u_sunCosZenith);',
                'float warmGreen = mix(graded.r, min(graded.r, graded.b), 0.25);',
                'float greenLead = max(graded.g - max(warmGreen, graded.b), 0.0);',
                'graded.g -= greenLead * brightDome * lowSunGate '
                '* clamp(aerialNightFade, 0.0, 1.0);'):
            self.assertIn(expression, source)
        helper = source.split('vec3 preserveSkyLuminance(', 1)[1].split('void main()', 1)[0]
        self.assertEqual(helper, 'vec3 color, vec3 graded) { '
                         'const vec3 lumaWeights = vec3(0.2126, 0.7152, 0.0722); '
                         'color = clamp(color, vec3(0.0), vec3(1.0)); '
                         'graded = max(graded, vec3(0.0)); '
                         'float luminance = dot(color, lumaWeights); '
                         'float gradedLuminance = dot(graded, lumaWeights); '
                         'if (gradedLuminance <= 0.0) { return color; } '
                         'vec3 chroma = graded * (luminance / gradedLuminance) - vec3(luminance); '
                         'float saturation = 1.0; '
                         'for (int channel = 0; channel < 3; ++channel) { '
                         'if (chroma[channel] > 0.0) { '
                         'saturation = min(saturation, (1.0 - luminance) / chroma[channel]); '
                         '} else if (chroma[channel] < 0.0) { '
                         'saturation = min(saturation, -luminance / chroma[channel]); } } '
                         'if (saturation < 1.0) { saturation /= 2.0 - saturation; } '
                         'return clamp(vec3(luminance) + saturation * chroma, '
                         'vec3(0.0), vec3(1.0)); } ')
        self.assertIn('graded = pow(graded, mix(vec3(1.0), vec3(0.9, 0.95, 1.05), dayFactor));',
                      source)
        self.assertIn('skyColor = min(pow(skyColor, vec3(1.0/2.2)), vec3(0.99));', source)
        self.assertEqual(source.count('skyColor = preserveSkyLuminance(skyColor, graded);'), 1)
        self.assertNotIn('tintLuma', source)
        main = source.split('void main()', 1)[1]
        self.assertIn('if (u_useSkyLut != 0) { vec3 tint', main)
        correction = main.index('skyColor = preserveSkyLuminance(')
        self.assertLess(main.index('skyColor = renderClouds('), main.index('2.51 * skyColor'))
        self.assertLess(main.index('2.51 * skyColor'), main.index('vec3 graded = skyColor;'))
        self.assertLess(main.index('graded = pow('), correction)
        self.assertLess(correction, main.index('vec3(1.0/2.2)'))
        self.assertLess(main.index('vec3(1.0/2.2)'),
                        main.index('skyColor = mix(skyColor, vec3(1.0), clamp(discCore, 0.0, 1.0));'))
        self.assertIn('float discCore = smoothstep(0.99975, 0.99997, sunDot) '
                      '* highSun * dayFactor * smoothstep(0.05, 0.20, viewDir.y);', main)
        lut = (ROOT / 'src/luminumbra_client/rendering/SkyAtmosphereLut.ipp').read_text(
            encoding='utf-8').split('const char* kTransmittanceComputeSrc', 1)[1].split(')GLSL"', 1)[0]
        for expression in ('6360000.0', '6460000.0', '8000.0', '1200.0',
                           '4.4e-6', 'vec3(5.802e-6, 13.558e-6, 33.1e-6)',
                           'TW = 256', 'TH = 64', 'SAMPLES = 40'):
            self.assertIn(expression, lut)

    def test_dusk_and_dawn_dome_keeps_gradient_without_clipped_plateau(self):
        # Existing dusk scene sun and dawn gate sun, plus their horizon approach
        # on either side. Each evaluates a bounded 32 azimuths * 64 elevations.
        for sun_y, day_factor in ((-.08, .43), (0, .43), (.10, .65), (.47, 1)):
            transmittance = self.transmittance(sun_y)
            with self.subTest(sun_y=sun_y):
                dome = []
                for row in range(64):
                    height = row / 63
                    elevation = height * math.pi / 2
                    # Neutral analytical radiance isolates the tint from the LUT;
                    # the existing horizon brightness must survive the grade.
                    color = (.95 - .55 * height,) * 3
                    band = []
                    for column in range(32):
                        azimuth = column * 2 * math.pi / 32
                        tint = self.tint(transmittance, sun_y, day_factor, elevation, azimuth)
                        band.append(self.grade(color, tint, day_factor=day_factor, sun_y=sun_y))
                    dome.append(band)
                clipped = sum(max(pixel) >= 1 for band in dome for pixel in band)
                self.assertEqual(clipped, 0,
                                 f'the grade introduces {clipped} channel-clipped plateau samples')
                # Powers are already in the candidate; apply gamma, the display
                # reserve and 8-bit quantization exactly once, outside the core.
                display = [tuple(round(min(c ** (1 / 2.2), .99) * 255) for c in pixel)
                           for band in dome for pixel in band]
                # Check before the reserve too: its cap would otherwise hide
                # any 255 plateau caused by the grade and gamma quantization.
                unreserved = [tuple(round(c ** (1 / 2.2) * 255) for c in pixel)
                              for band in dome for pixel in band]
                self.assertGreater(len(set(display[::32])), 48)
                for column in range(32):
                    clipped_rows = [max(display[row * 32 + column]) == 255 for row in range(64)]
                    self.assertFalse(any(a and b for a, b in zip(clipped_rows, clipped_rows[1:])),
                                     'display quantization creates a clipped vertical plateau')
                    unreserved_rows = [max(unreserved[row * 32 + column]) == 255
                                       for row in range(64)]
                    self.assertFalse(any(a and b for a, b in zip(unreserved_rows, unreserved_rows[1:])),
                                     'the display reserve hides a clipped vertical plateau')
                for row in range(1, 64):
                    self.assertGreater(self.luma(dome[row - 1][0]) - self.luma(dome[row][0]), .008)
                self.assertGreater(self.luma(dome[0][0]) - self.luma(dome[-1][0]), .5)
                for band in dome[:32]:
                    red, green, blue = band[0]  # Sun-facing half of the dome.
                    self.assertGreaterEqual(red, green)
                    self.assertGreaterEqual(green, blue)
                    self.assertGreater(red - blue, .01)

    def test_gamut_bound_preserves_in_range_grades_luminance_and_extinction(self):
        # 8 tint corners, neutral/faded/near-zero tints, and 125 input colors.
        tints = [(r, g, b) for r in (0, 1) for g in (0, 1) for b in (0, 1)]
        tints += [(1, 1, 1), (.7, .7, .7), (1e-8, 2e-8, 3e-8), (1, .1, 0)]
        for tint in tints:
            for red in (0, .01, .2, .8, 1):
                for green in (0, .01, .2, .8, 1):
                    for blue in (0, .01, .2, .8, 1):
                        color = (red, green, blue)
                        graded = self.grade(color, tint)
                        self.assertTrue(all(math.isfinite(c) and 0 <= c <= 1 for c in graded))
                        candidate = self.candidate(color, tint)
                        candidate_luma = self.luma(candidate)
                        unbounded = (tuple(c * (self.luma(color) / candidate_luma)
                                           for c in candidate) if candidate_luma > 0 else color)
                        self.assertAlmostEqual(self.luma(graded), self.luma(color))
                        if max(unbounded) <= 1:
                            for actual, expected in zip(graded, unbounded):
                                self.assertAlmostEqual(actual, expected)
                        elif 0 < self.luma(color) < 1:
                            self.assertLess(max(graded), 1, 'out-of-gamut grades must roll below one')
                        order = sorted(range(3), key=lambda channel: unbounded[channel])
                        self.assertLessEqual(graded[order[0]], graded[order[1]])
                        self.assertLessEqual(graded[order[1]], graded[order[2]])
                        if tint == (1, 1, 1):
                            for actual, expected in zip(graded, color):
                                self.assertAlmostEqual(actual, expected)
                            self.assertAlmostEqual(self.luma(graded), self.luma(color))
        for day_factor in (0, .1, .22, 1):
            for transmittance in ((0, 0, 0), (1e-8, 0, 0), (1, 1, 1)):
                tint = self.tint(transmittance, 0, day_factor, 0, 0)
                graded = self.grade((.8, .6, .4), tint, day_factor=day_factor, sun_y=0,
                                    night_fade=self.smoothstep(0, .22, day_factor))
                self.assertTrue(all(math.isfinite(c) and 0 <= c <= 1 for c in graded))
                if day_factor == 0:
                    for actual, expected in zip(graded, (.8, .6, .4)):
                        self.assertAlmostEqual(actual, expected)

        # Exercise the new grade structure, both LUT and legacy, across all
        # day-power / green-removal gates (4 * 3 * 2 * 3 bounded cases).
        for day_factor in (0, .1, .22, 1):
            for sun_y in (0, .65, 1):
                for use_lut in (False, True):
                    for color in ((.2, .8, .1), (.95, .95, .95), (1e-8, 2e-8, 3e-8)):
                        kwargs = dict(day_factor=day_factor, sun_y=sun_y,
                                      night_fade=self.smoothstep(0, .22, day_factor),
                                      use_lut=use_lut)
                        graded = self.grade(color, (1, .7, .1), **kwargs)
                        self.assertTrue(all(math.isfinite(c) and 0 <= c <= 1 for c in graded))
                        self.assertAlmostEqual(self.luma(graded), self.luma(color))
        low = self.grade((.4, .8, .3), (1, 1, 1), sun_y=0)
        high = self.grade((.4, .8, .3), (1, 1, 1), sun_y=1)
        self.assertLess(low[1] - max(low[0], low[2]), high[1] - max(high[0], high[2]))
        self.assertGreater(self.luma(self.grade((.8,) * 3, (1, .7, .1), day_factor=1)),
                           self.luma(self.grade((.4,) * 3, (1, .7, .1), day_factor=.43)))

        # The same 50 inputs and 3e-6 tolerance as the shipping-helper rendering
        # regression, evaluated here without a graphics context.
        colors = ((0, 0, 0), (1, 1, 1), (.5, .5, .5), (1, 0, 0), (0, 1, 0), (0, 0, 1),
                  (.9, .7, .3), (.01, .6, .98), (.999, .998, .995), (1e-6, 2e-6, 3e-6))
        for color in colors:
            for tint in ((1, 1, 1), (1, .7, .1), (.01, 1, .2), (0, 0, 0), (4, .1, 2)):
                candidate = tuple(c * t for c, t in zip(color, tint))
                graded = self.preserve_luminance(color, candidate)
                self.assertAlmostEqual(self.luma(graded), self.luma(color), delta=3e-6)
                for channel, original in zip(graded, color):
                    self.assertTrue(math.isfinite(channel))
                    self.assertGreaterEqual(channel, 0)
                    self.assertLessEqual(channel, 1)
                    if tint in ((0, 0, 0), (1, 1, 1)):
                        self.assertAlmostEqual(channel, original, delta=3e-6)

    def test_compression_boundary_and_hdr_extremes(self):
        # Cross the normalized chroma boundary without changing input luminance.
        # Candidate channels preserve .5 luminance and pass through peak == 1.
        for delta in (-1e-5, 0, 1e-8, 1e-5, .1):
            peak = 1 + delta
            other = (.5 - .2126 * peak) / (.7152 + .0722)
            candidate = (peak, other, other)
            graded = self.preserve_luminance((.5,) * 3, candidate)
            self.assertAlmostEqual(self.luma(graded), .5)
            self.assertTrue(all(math.isfinite(c) and 0 <= c <= 1 for c in graded))
            if delta <= 0:
                for actual, expected in zip(graded, candidate):
                    self.assertAlmostEqual(actual, expected)
            else:
                self.assertLess(max(graded), 1)
            if abs(delta) <= 1e-5:
                self.assertLess(max(abs(a - b) for a, b in zip(graded, candidate)), 1e-4)
        # Section 7's ACES clamp precedes grading, including near-white and HDR.
        for delta in (-1e-5, 0, 1e-8, 1e-5, .1, 2.51 / 2.43 - 1):
            for other in (0, .3, 1):
                color = (1 + delta, other, other)
                graded = self.grade(color, (1, 1, 1))
                self.assertTrue(all(math.isfinite(c) and 0 <= c <= 1 for c in graded))
                bounded = tuple(min(1, max(0, c)) for c in color)
                self.assertAlmostEqual(self.luma(graded), self.luma(bounded))
                self.assertGreaterEqual((graded[0] - graded[1]) * (color[0] - color[1]), 0)
                self.assertEqual(graded[1], graded[2])
                if max(color) <= 1:
                    for actual, expected in zip(graded, color):
                        self.assertAlmostEqual(actual, expected)
                if abs(delta) <= 1e-5:
                    self.assertLess(max(abs(a - b) for a, b in zip(graded, color)), 1e-4)
        self.assertEqual(self.grade((2.51 / 2.43,) * 3, (1, 1, 1)), (1, 1, 1))
        # Black/white, zero or negative candidates, near-white, and both small
        # and HDR-scale candidates also reach the helper's clamps and fallback.
        for color in ((0, 0, 0), (1, 1, 1), (.999, .998, .995), (1e-8, 2e-8, 3e-8)):
            for candidate in ((0, 0, 0), (-1, -2, -3), (-1, .5, 2), (4, .1, 2),
                              (1e-8, 0, 0), color):
                graded = self.preserve_luminance(color, candidate)
                self.assertTrue(all(math.isfinite(c) and 0 <= c <= 1 for c in graded))
                self.assertAlmostEqual(self.luma(graded), self.luma(color))
                if max(candidate) <= 0 or candidate == color:
                    for actual, expected in zip(graded, color):
                        self.assertAlmostEqual(actual, expected)


class ShaderScenesTests(unittest.TestCase):
    def test_near_shrubs_are_leaf_cards_and_every_bush_lod_avoids_grass_albedo(self):
        rendering = ROOT / "src/luminumbra_client/rendering"
        source = read(ROOT / "src/luminumbra_client/app/ProcgenPalettes.cpp")
        palette = compact(body(source, "void BuildProcgenBushPalette("))
        self.assertIn("BuildBushLeafCard(center, normal, 0.24f, 0.18f)", palette)
        for corner, uv in ((0, "0.0f, 0.0f"), (1, "1.0f, 0.0f"),
                           (2, "1.0f, 1.0f"), (3, "0.0f, 1.0f")):
            self.assertIn("verts.push_back({card[%d], normal, {%s}});" % (corner, uv), palette)
        self.assertIn("{0u, 1u, 2u, 0u, 2u, 3u}", palette)
        draw = compact(body(read(rendering / "passes/GBufferPass.cpp"),
                            "void GBufferPass::geometry_pass_static_meshes("))
        self.assertIn('const bool isBush = batch.basePath.starts_with("procgen://bush_");', draw)
        self.assertIn("const bool leafCards = isLeaf || (isBush && !isFarLod);", draw)
        self.assertIn('shader->setInt("u_alphaTest", leafCards ? 2 : 0);', draw)
        self.assertIn("two_sided = leafCards;", draw)
        self.assertIn('shader->setFloat("u_windStrength", leafCards ? 0.85f : 0.0f);', draw)
        self.assertNotIn('isLeaf ? 0.85f', draw)
        self.assertIn('shader->setInt("u_forceFlat", (isBush || (isLeaf && isFarLod)) ? 1 : 0);', draw)
        # Closed far hulls keep the leaf reflectance; every other draw resets it.
        self.assertIn('shader->setInt("u_leafSurface", isBush ? 1 : 0);', draw)
        self.assertIn('shader->setInt("u_forceFlat", 0); shader->setInt("u_leafSurface", 0);', draw)
        frag = compact(read(ROOT / "res/shaders/g_buffer.frag"))
        self.assertIn("uniform int u_leafSurface = 0;", frag)
        self.assertIn("if (proceduralLeaf || u_leafSurface == 1) {", frag)
        # The card silhouette test stays keyed to alpha-tested cards only.
        self.assertIn("bool proceduralLeaf = u_alphaTest == 2;", frag)
        # Both legacy and ladder programs use this same draw consumer.
        self.assertIn("instanced_static_mesh_shader(input.far_selection.effective)", draw)
        native = read(ROOT / "test/rendering/diagnostic_attribution_test.cpp")
        for regression in ("LeafCardsAreFiniteOutwardQuadsForSideAndPolarNormals",
                           "NearBushesUseFullLeafQuadsAndNeverTerrainGrassTexturing"):
            self.assertIn(regression, native)


    def capture(self, directory, value=.25):
        for name in sorted(scene.INPUTS - {'test/rendering/shader_scene_render_test.cpp'}):
            (directory / Path(name).name).write_bytes((ROOT / name).read_bytes())
        fixture = (ROOT / 'test/rendering/shader_scene_render_test.cpp').read_bytes().replace(b'\r\n', b'\n')
        (directory / 'fixture-sha256.txt').write_text(scene.digest(fixture), encoding='ascii')
        (directory / 'material.pfm').write_bytes(b'PF\n128 128\n-1.0\n' +
                                               struct.pack('<f', value) * scene.CHANNELS)
        (directory / 'tree-cutout.pfm').write_bytes(
            b'PF\n128 128\n-1.0\n' + struct.pack('<' + 'f' * scene.CHANNELS, *self.cutout()))
        for name, values in (('night-sky', self.night()), ('terrain-blend', self.terrain()),
                             ('dusk-sky', [.1] * scene.CHANNELS),
                             ('shrub-leaf', scene.shrub_reference()),
                             ('shrub-normal', scene.shrub_reference(True))):
            (directory / (name + '.pfm')).write_bytes(
                b'PF\n128 128\n-1.0\n' + struct.pack('<' + 'f' * scene.CHANNELS, *values))
        (directory / 'renderer.txt').write_text('llvmpipe (test fixture)\n', encoding='utf-8')
        (directory / 'version.txt').write_text('4.5 Mesa fixture\n', encoding='utf-8')

    @staticmethod
    def cutout():
        return [value for y in range(128) for x in range(128)
                for value in ((.0625, .125, .1875) if (x // 32 + y // 32) % 4 < 2
                              else (.25, .5, .75))]

    @staticmethod
    def night():
        return [max(0, .7 * (1 - ((x % 32 - 16) ** 2 + (y - 96) ** 2) / 100))
                if y >= 64 else 0
                for y in range(128) for x in range(128) for channel in range(3)]

    @staticmethod
    def terrain():
        return [.5 + delta * (x + .5) / 128 for y in range(128) for x in range(128)
                for delta in (.4, .3, 0)]

    def test_inventory_and_analytical_reference(self):
        manifest, data = scene.load_manifest(MANIFEST)
        values = scene.rgb(data['material'], False)
        self.assertEqual(len(values), scene.CHANNELS)
        self.assertEqual(scene.metrics(values, values, manifest['fixtures'][0]['mask']), (0, 0))
        for metadata in manifest['reference_capture'].values():
            self.assertIn(metadata['origin'], ('analytical', 'llvmpipe'))
        tree = scene.rgb(data['tree-cutout'], False)
        self.assertLessEqual(max(abs(a - b) for a, b in zip(tree, self.cutout())), .5 / 255)
        self.assertEqual(set(data), {'material', 'tree-cutout', 'night-sky', 'terrain-blend'})
        self.assertLessEqual(max(abs(a - b) for a, b in zip(
            scene.rgb(data['terrain-blend'], False), self.terrain())), .5 / 255)
        self.assertNotIn('dusk-sky', data)
        self.assertEqual(manifest['diagnostics'], scene.DIAGNOSTICS)
        self.assertEqual(manifest['analytical'], scene.ANALYTICAL)

    def test_shrub_analytical_reference_and_each_regression(self):
        albedo, normals = scene.shrub_reference(), scene.shrub_reference(True)
        scene.check_shrub_scene(albedo, normals)
        for x, y, expected_albedo, expected_normal in (
                (0, 0, (.2, .6, .15), (.7, .6, 0)),
                (32, 0, (.10, .24, .045), (.7, .6, 0)),
                (64, 0, (.0625, .125, .1875), (.0625, .125, .1875)),
                (80, 32, (.10, .24, .045), (.7, .6, 0)),
                (112, 32, (.10, .24, .045), (.1, .2, 0)),
                (80, 96, (.10, .24, .045), (2 / 3, 2 / 3, 0))):
            offset = (y * 128 + x) * 3
            for actual, expected in zip(albedo[offset:offset + 3], expected_albedo):
                self.assertAlmostEqual(actual, expected)
            for actual, expected in zip(normals[offset:offset + 3], expected_normal):
                self.assertAlmostEqual(actual, expected)
        # Change exactly one pixel per class: neither image-wide MAE nor p99
        # can conceal a lost hull reflectance, cutout or inverse transpose.
        for x, y, normal, value in ((32, 0, False, (.2, .6, .15)),
                                    (64, 0, False, (.10, .24, .045)),
                                    (80, 32, True, (.625, .75, 0)),
                                    (112, 32, True, (.25, .125, 0)),
                                    (80, 96, True, (.7, .6, 0))):
            changed = (normals if normal else albedo).copy()
            offset = (y * 128 + x) * 3
            changed[offset:offset + 3] = value
            with self.subTest(x=x, y=y, normal=normal), self.assertRaisesRegex(ValueError, 'invariant'):
                scene.check_shrub_scene(albedo if normal else changed, changed if normal else normals)
        for invalid in ([], albedo[:-1], [math.nan] + albedo[1:],
                        [math.inf] + albedo[1:], [-.1] + albedo[1:], [1.1] + albedo[1:]):
            for normal in (False, True):
                with self.subTest(normal=normal), self.assertRaises(ValueError):
                    scene.check_shrub_scene(albedo if normal else invalid, invalid if normal else normals)

    def test_exact_thresholds_and_mask(self):
        zeros = (0.,) * scene.CHANNELS
        for value, passes in ((.004, True), (.005, True), (.0051, False), (.021, False)):
            mean, percentile = scene.metrics((value,) * scene.CHANNELS, zeros, [0, 0, 128, 128])
            self.assertEqual(mean <= .005 and percentile <= .02, passes)
        sparse = list(zeros)
        sparse[:1000] = [.021] * 1000
        mean, percentile = scene.metrics(sparse, zeros, [0, 0, 128, 128])
        self.assertLess(mean, .005)
        self.assertGreater(percentile, .02)
        self.assertEqual(scene.metrics(sparse, zeros, [64, 64, 128, 128]), (0, 0))
        for mask in ([0, 0, 0, 0], [-1, 0, 128, 128], [0, 0, 129, 128], [0., 0, 1, 1], []):
            with self.subTest(mask=mask), self.assertRaises(ValueError):
                scene.metrics(zeros, zeros, mask)
        for value in (math.nan, math.inf, -math.inf):
            invalid = list(zeros)
            invalid[0] = value
            with self.assertRaises(ValueError):
                scene.metrics(invalid, zeros, [64, 64, 128, 128])
        with self.assertRaises(ValueError):
            scene.metrics([], zeros, [0, 0, 1, 1])

    def test_image_refusals(self):
        good = b'PF\n128 128\n-1.0\n' + struct.pack('<f', .25) * scene.CHANNELS
        self.assertEqual(scene.rgb(good, True)[0], .25)
        for bad in (b'', good[:-1], good + b'\0', good.replace(b'128 128', b'129 128'),
                    good.replace(b'-1.0', b'1.0'), good.replace(b'PF', b'Pf')):
            with self.subTest(header=bad[:20]), self.assertRaises(ValueError):
                scene.rgb(bad, True)
        for value in (math.nan, math.inf, -math.inf, -.01, 1.01):
            with self.subTest(value=value), self.assertRaises(ValueError):
                scene.rgb(good[:len(b'PF\n128 128\n-1.0\n')] + struct.pack('<f', value) * scene.CHANNELS, True)

    def test_bounded_regular_reads_and_path_refusals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'small'
            target.write_bytes(b'1234')
            self.assertEqual(scene.read_file(target, 4), b'1234')
            with self.assertRaises(ValueError):
                scene.read_file(target, 3)
            with patch.object(scene.os, 'fstat', return_value=SimpleNamespace(
                    st_mode=target.stat().st_mode, st_size=0)):
                with self.assertRaisesRegex(ValueError, 'exceeds bound'):
                    scene.read_file(target, 3)
            with patch.object(scene.os, 'fstat', return_value=SimpleNamespace(st_mode=0, st_size=0)):
                with self.assertRaisesRegex(ValueError, 'regular file'):
                    scene.read_file(target)
            for path in (root, root / 'absent'):
                with self.assertRaises((OSError, ValueError)):
                    scene.read_file(path)
            for name in ('../outside', '/absolute', 'C:/drive', 'a\\b', '', './small', 'x//small'):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    scene.local_file(root, name)
            # Windows TEMP can be an 8.3 short path; local_file returns the resolved one.
            self.assertEqual(scene.local_file(root, 'small'), target.resolve())
            with patch.object(scene.os, 'open', side_effect=PermissionError('denied')):
                with self.assertRaises(PermissionError):
                    scene.read_file(target)
            with patch.object(Path, 'resolve', side_effect=[root.parent / 'escape', root]):
                with self.assertRaisesRegex(ValueError, 'escapes'):
                    scene.local_file(root, 'small')

    def test_manifest_refuses_partial_unknown_and_changed_inputs(self):
        original = json.loads(MANIFEST.read_text(encoding='utf-8'))
        mutations = [dict(schema=1), dict(schema=2), dict(schema=True), dict(size=[128., 128]), dict(size=[129, 128]), dict(defines=['UNKNOWN 1']),
                     dict(encoding='srgb'), dict(fixtures=[]), dict(inputs={}),
                     dict(reference_sha256='0' * 64), dict(reference_sha256={}),
                     dict(reference_sha256={'material': '0' * 64, 'tree-cutout': '0' * 64}), dict(reference_capture={}), dict(inputs=None),
                     dict(reference_capture=None),
                     dict(reference_capture={'origin': 'llvmpipe', 'renderer': 'hardware', 'version': '4.5'})]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'references').mkdir()
            for fixture in scene.FIXTURES:
                (root / fixture['reference']).write_bytes(
                    (MANIFEST.parent / fixture['reference']).read_bytes())
            for changed in mutations:
                data = dict(original, **changed)
                path = root / 'scenes.json'
                path.write_text(json.dumps(data), encoding='utf-8')
                with self.subTest(changed=changed), self.assertRaises(ValueError):
                    scene.load_manifest(path)
            data = copy.deepcopy(original)
            data['inputs']['res/shaders/static_asset.frag'] = '0' * 64
            path.write_text(json.dumps(data), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'input hash changed'):
                scene.load_manifest(path)
            with self.assertRaises((ValueError, OSError)):
                scene.local_file(root, 'missing/reference.ppm')

    def test_compare_and_regenerate_require_complete_evidence_before_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / 'capture'
            capture.mkdir()
            self.capture(capture)
            output = root / 'candidate'
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST, capture=capture, output=output)
            (capture / 'renderer.txt').write_text('hardware GPU', encoding='utf-8')
            with self.assertRaises(ValueError):
                scene.run(args)
            self.assertFalse(output.exists())
            self.capture(capture)
            for filename, content in (('fixture-sha256.txt', b'0' * 64),
                                      ('fixture-sha256.txt', b'unknown'),
                                      ('static_asset.frag', b'old shader')):
                (capture / filename).write_bytes(content)
                with self.subTest(filename=filename), self.assertRaises(ValueError):
                    scene.run(args)
                self.assertFalse(output.exists())
                self.capture(capture)
            self.assertEqual(scene.run(args), 0)
            regenerated, unused = scene.load_manifest(output / 'scenes.json')
            self.assertTrue(all(value['origin'] == 'llvmpipe'
                                for value in regenerated['reference_capture'].values()))
            with self.assertRaises(FileExistsError):
                scene.run(args)
            args.command = 'compare'
            args.manifest = output / 'scenes.json'
            previous = copy.deepcopy(regenerated)
            previous['inputs']['res/shaders/static_asset.frag'] = '0' * 64
            with patch.object(scene, 'load_manifest', return_value=(previous, unused)):
                with self.assertRaisesRegex(ValueError, 'capture inputs differ'):
                    scene.run(args)
            self.assertEqual(scene.run(args), 0)
            self.capture(capture, .9)
            self.assertEqual(scene.run(args), 1)
            (capture / 'version.txt').unlink()
            with self.assertRaises(OSError):
                scene.run(args)
            self.assertTrue((output / 'scenes.json').is_file())

    def test_regeneration_io_failure_leaves_baseline_and_completion_metadata_unchanged(self):
        baseline = MANIFEST.read_bytes()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / 'capture'
            capture.mkdir()
            self.capture(capture)
            output = root / 'candidate'
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST, capture=capture, output=output)
            with patch.object(Path, 'write_bytes', side_effect=OSError('short write')):
                with self.assertRaises(OSError):
                    scene.run(args)
            self.assertFalse((output / 'scenes.json').exists())
            self.assertEqual(MANIFEST.read_bytes(), baseline)
            args.output = None
            with self.assertRaisesRegex(ValueError, 'fresh --output'):
                scene.run(args)

    def test_second_reference_inventory_refusals_and_late_write_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'references').mkdir()
            original = json.loads(MANIFEST.read_text(encoding='utf-8'))
            for fixture in scene.FIXTURES:
                (root / fixture['reference']).write_bytes((MANIFEST.parent / fixture['reference']).read_bytes())
            for mutation in ('duplicate', 'unknown', 'float-mask', 'missing-hash', 'corrupt-reference'):
                manifest = copy.deepcopy(original)
                if mutation == 'duplicate':
                    manifest['fixtures'].append(manifest['fixtures'][0])
                elif mutation == 'unknown':
                    manifest['fixtures'][1]['name'] = 'unknown'
                elif mutation == 'float-mask':
                    manifest['fixtures'][1]['mask'][0] = 0.0
                elif mutation == 'missing-hash':
                    del manifest['reference_sha256']['tree-cutout']
                else:
                    manifest['reference_sha256']['tree-cutout'] = '0' * 64
                (root / 'scenes.json').write_text(json.dumps(manifest), encoding='utf-8')
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    scene.load_manifest(root / 'scenes.json')
            tree_reference = root / 'references/tree-cutout.ppm'
            good_reference = tree_reference.read_bytes()
            for payload in (None, b'', good_reference[:-1], good_reference + b'0'):
                manifest = copy.deepcopy(original)
                if payload is None:
                    tree_reference.unlink()
                else:
                    tree_reference.write_bytes(payload)
                    manifest['reference_sha256']['tree-cutout'] = scene.digest(payload)
                (root / 'scenes.json').write_text(json.dumps(manifest), encoding='utf-8')
                with self.subTest(payload_size=None if payload is None else len(payload)), \
                        self.assertRaises((OSError, ValueError)):
                    scene.load_manifest(root / 'scenes.json')
                tree_reference.write_bytes(good_reference)
            capture = root / 'capture'
            capture.mkdir()
            self.capture(capture)
            output = root / 'candidate'
            write = Path.write_bytes

            def fail_second(path, payload):
                if path.name == 'tree-cutout.ppm':
                    raise OSError('second reference failed')
                return write(path, payload)

            args = argparse.Namespace(command='regenerate', manifest=MANIFEST, capture=capture, output=output)
            with patch.object(Path, 'write_bytes', fail_second), self.assertRaises(OSError):
                scene.run(args)
            self.assertTrue((output / 'references/material.ppm').is_file())
            self.assertFalse((output / 'scenes.json').exists())

    def test_cutout_categories_and_failure_samples(self):
        actual = self.cutout()
        self.assertEqual(scene.tree_cutout_error(actual), 0)
        for index in (0, 32 * 3, 64 * 3, 96 * 3, scene.CHANNELS - 3):
            # Transparent, below cutoff, exact cutoff and opaque texels, with
            # an error at even one pixel: a whole-image MAE can miss this.
            for delta, passes in ((.0000009, True), (.0000011, False), (.1, False)):
                changed = actual.copy()
                changed[index] += delta
                self.assertEqual(scene.tree_cutout_error(changed) <= .000001, passes)
        for invalid in ([], actual[:-1], actual + [0],
                        [math.nan] + actual[1:], [math.inf] + actual[1:],
                        [-math.inf] + actual[1:]):
            with self.assertRaises(ValueError):
                scene.tree_cutout_error(invalid)

    def test_second_scene_evidence_is_mandatory_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / 'capture'
            capture.mkdir()
            output = root / 'candidate'
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST,
                                      capture=capture, output=output)
            for filename in ('tree_impostor.vert', 'tree_impostor.frag', 'tree-cutout.pfm'):
                for bad in (None, b'', b'partial'):
                    self.capture(capture)
                    if bad is None:
                        (capture / filename).unlink()
                    else:
                        (capture / filename).write_bytes(bad)
                    with self.subTest(filename=filename, bad=bad), self.assertRaises((OSError, ValueError)):
                        scene.run(args)
                    self.assertFalse(output.exists())
            self.capture(capture)
            # An opaque rectangle must fail both comparison and regeneration,
            # independently of the reference score or baseline regeneration.
            (capture / 'tree-cutout.pfm').write_bytes(
                b'PF\n128 128\n-1.0\n' + struct.pack('<f', .25) * scene.CHANNELS)
            for command in ('compare', 'regenerate'):
                args.command = command
                with self.assertRaisesRegex(ValueError, 'cutout/background invariant'):
                    scene.run(args)
                self.assertFalse(output.exists())

    def test_sky_and_terrain_metrics_detect_single_pixel_and_triangle_regressions(self):
        night, terrain = self.night(), self.terrain()
        scene.check_scene_invariants({'night-sky': night, 'terrain-blend': terrain})
        self.assertEqual(scene.terrain_blend_metrics(terrain)[:2], (102, 102))
        for value, passes in ((.0000009, True), (.0000011, False)):
            changed = night.copy()
            changed[63 * 128 * 3] = value  # The exact horizon row is included.
            self.assertEqual(scene.night_sky_metrics(changed)[0] <= .000001, passes)
        for channel in range(3):
            changed = terrain.copy()
            changed[(100 * 128 + 20) * 3 + channel] += .001
            with self.assertRaisesRegex(ValueError, 'terrain blend'):
                scene.check_scene_invariants({'night-sky': night, 'terrain-blend': changed})
        for tile in range(4):
            changed = night.copy()
            for y in range(95, 98):
                for x in range(tile * 32 + 15, tile * 32 + 18):
                    changed[(y * 128 + x) * 3:(y * 128 + x + 1) * 3] = [.7] * 3
            with self.subTest(tile=tile), self.assertRaisesRegex(ValueError, 'night sky'):
                scene.check_scene_invariants({'night-sky': changed, 'terrain-blend': terrain})
        boundary = night.copy()
        boundary[(64 * 128) * 3:(64 * 128) * 3 + 3] = [1] * 3
        with self.assertRaisesRegex(ValueError, 'boundary'):
            scene.night_sky_metrics(boundary)
        flat = [.5] * scene.CHANNELS
        with self.assertRaises(ValueError):
            scene.check_scene_invariants({'night-sky': flat, 'terrain-blend': terrain})
        # Old provoking-vertex assignment makes a diagonal two-triangle step.
        triangles = [.5 + delta * int(x + y >= 128) for y in range(128) for x in range(128)
                     for delta in (.4, .3, 0)]
        with self.assertRaisesRegex(ValueError, 'terrain blend'):
            scene.check_scene_invariants({'night-sky': night, 'terrain-blend': triangles})
        for invalid in ([], terrain[:-1], terrain + [0],
                        [math.nan] + terrain[1:], [math.inf] + terrain[1:],
                        [-.1] + terrain[1:], [1.1] + terrain[1:]):
            for metric in (scene.night_sky_metrics, scene.terrain_blend_metrics, scene.horizontal_step):
                with self.subTest(metric=metric.__name__), self.assertRaises(ValueError):
                    metric(invalid)

    def test_dusk_continuity_includes_full_azimuth_and_wrap(self):
        flat = [.1] * scene.CHANNELS
        self.assertEqual(scene.horizontal_step(flat), 0)
        for x in (0, 64, 127):
            changed = flat.copy()
            for y in range(128):
                changed[(y * 128 + x) * 3:(y * 128 + x + 1) * 3] = [.2] * 3
            self.assertAlmostEqual(scene.horizontal_step(changed), .1)
        # A ramp makes the 127->0 wrap larger than every interior step.
        ramp = [x / 128 for y in range(128) for x in range(128) for channel in range(3)]
        self.assertAlmostEqual(scene.horizontal_step(ramp), 127 / 128)

    def test_new_scene_evidence_and_invariants_precede_candidate_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / 'capture'
            capture.mkdir()
            output = root / 'candidate'
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST,
                                      capture=capture, output=output)
            for filename in ('enhanced_skybox.frag', 'SkyAtmosphereLut.ipp',
                             'g_buffer.vert', 'g_buffer.frag', 'night-sky.pfm',
                             'terrain-blend.pfm', 'dusk-sky.pfm', 'instanced_mesh.vert',
                             'shrub-leaf.pfm', 'shrub-normal.pfm'):
                for bad in (None, b'', b'partial'):
                    self.capture(capture)
                    if bad is None:
                        (capture / filename).unlink()
                    else:
                        (capture / filename).write_bytes(bad)
                    with self.subTest(filename=filename, bad=bad), self.assertRaises((OSError, ValueError)):
                        scene.run(args)
                    self.assertFalse(output.exists())
            for name, values in (('night-sky', self.night()), ('terrain-blend', self.terrain())):
                # One wrong pixel is too small for MAE/p99, but must prevent even regeneration.
                values[0] += .01
                self.capture(capture)
                (capture / (name + '.pfm')).write_bytes(
                    b'PF\n128 128\n-1.0\n' + struct.pack('<' + 'f' * scene.CHANNELS, *values))
                for command in ('compare', 'regenerate'):
                    args.command = command
                    args.output = root / (name + '-' + command)
                    with self.subTest(name=name, command=command):
                        with self.assertRaisesRegex(ValueError, 'invariant'):
                            scene.run(args)
                        self.assertFalse(args.output.exists())
            # Dusk remains measured evidence without a failing/reference expectation.
            self.capture(capture)
            (capture / 'dusk-sky.pfm').write_bytes(
                b'PF\n128 128\n-1.0\n' + struct.pack('<' + 'f' * scene.CHANNELS,
                    *[.8 if x < 64 else .1 for y in range(128) for x in range(128) for channel in range(3)]))
            args.command = 'regenerate'
            args.output = output
            self.assertEqual(scene.run(args), 0)
            self.assertFalse((output / 'references/dusk-sky.ppm').exists())
            manifest, _ = scene.load_manifest(output / 'scenes.json')
            self.assertNotIn('dusk-sky', manifest['reference_sha256'])

    def test_new_inventory_metadata_and_last_reference_write_failure(self):
        original = json.loads(MANIFEST.read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'references').mkdir()
            for fixture in scene.FIXTURES:
                (root / fixture['reference']).write_bytes((MANIFEST.parent / fixture['reference']).read_bytes())
            changes = [{'diagnostics': []}, {'analytical': None}, {'analytical': {}},
                       {'analytical': dict(scene.ANALYTICAL, scale_y=[1, 1])}]
            for name in ('night-sky', 'terrain-blend'):
                for field in ('fixtures', 'reference_sha256', 'reference_capture', 'inputs'):
                    data = copy.deepcopy(original)
                    if field == 'fixtures':
                        data[field] = [value for value in data[field] if value['name'] != name]
                    else:
                        key = 'res/shaders/enhanced_skybox.frag' if field == 'inputs' else name
                        del data[field][key]
                    changes.append(data)
                for metadata in (None, {}, {'origin': 'llvmpipe', 'renderer': 'hardware', 'version': '4.5'}):
                    data = copy.deepcopy(original)
                    data['reference_capture'][name] = metadata
                    changes.append(data)
            for changed in changes:
                data = dict(original, **changed)
                path = root / 'scenes.json'
                path.write_text(json.dumps(data), encoding='utf-8')
                with self.assertRaises(ValueError):
                    scene.load_manifest(path)
            capture = root / 'capture'
            capture.mkdir()
            self.capture(capture)
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST,
                                      capture=capture, output=root / 'candidate')
            write = Path.write_bytes

            def fail_last(path, payload):
                if path.name == 'terrain-blend.ppm':
                    raise OSError('last reference failed')
                return write(path, payload)

            with patch.object(Path, 'write_bytes', fail_last), self.assertRaises(OSError):
                scene.run(args)
            self.assertFalse((args.output / 'scenes.json').exists())

    def test_runner_waits_finish_inside_every_ctest_deadline(self):
        worst = (software.DISPLAY_READY_SECONDS + software.CHILD_TIMEOUT_SECONDS
                 + 2 * software.SERVER_STOP_SECONDS)
        registrations = {'ShaderSceneSoftwareGl': ROOT / 'test/CMakeLists.txt',
                         'StaticPreviewSoftwareGL': ROOT / 'tools/static_preview/CMakeLists.txt'}
        for name, cmake in registrations.items():
            with self.subTest(test=name):
                text = cmake.read_text(encoding='utf-8')
                self.assertRegex(text, r'add_test\(NAME ' + name + r'\s+COMMAND[^)]*run_software_gl\.py')
                deadline = re.search(r'set_tests_properties\(' + name + r' PROPERTIES[^)]*?TIMEOUT (\d+)', text)
                self.assertIsNotNone(deadline)
                self.assertLess(worst, int(deadline.group(1)))

    def test_every_rendering_and_harness_path_routes_and_docs_do_not(self):
        sensitive = ('res/shaders/new.frag', 'src/luminumbra_client/rendering/new.cpp',
                     'test/rendering/shader_scenes/references/material.ppm',
                     'test/rendering/shader_scene_render_test.cpp', 'test/CMakeLists.txt',
                     'tools/gates/shader-scenes.py', 'tools/gates/test_shader_scenes.py',
                     'tools/static_preview/run_software_gl.py', 'tools/ci/ci_paths.py',
                     'tools/ci/test_ci_paths.py', 'tools/ci/compile_shaders.py',
                     'tools/ci/test_compile_shaders.py', 'tools/ci/shader_variants.json',
                     '.github/workflows/ci.yml', 'CMakeLists.txt', '.gitattributes')
        for path in sensitive:
            with self.subTest(path=path), patch.object(paths, 'changed_paths', return_value=[path]):
                self.assertTrue(paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head'))
                self.assertTrue(paths.needs_cpp(path))
        with patch.object(paths, 'changed_paths', return_value=['docs/readme.md']):
            self.assertFalse(paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head'))
        for revisions in (('', 'head'), ('base', '')):
            with self.assertRaises(ValueError):
                paths.classify_shader_scenes(ROOT, 'pull_request', *revisions)
        for result in ([], None):
            with patch.object(paths, 'changed_paths', return_value=result), self.assertRaises(ValueError):
                paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head')
        with patch.object(paths, 'changed_paths', side_effect=subprocess.CalledProcessError(1, 'git')):
            with self.assertRaises(subprocess.CalledProcessError):
                paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head')
        self.assertTrue(paths.classify_shader_scenes(ROOT, 'push', '', ''))

    def test_runner_exact_scene_inventory_rejects_missing_substituted_and_skipped_cases(self):
        source = (ROOT / 'test/rendering/shader_scene_render_test.cpp').read_text(encoding='utf-8')
        registered = re.findall(r'^TEST_F\(\s*(\w+)\s*,\s*(\w+)\s*\)', source, re.MULTILINE)
        self.assertEqual(set(registered), software.SHADER_CASES)
        root = ET.Element('testsuites')
        for classname, name in registered:
            ET.SubElement(root, 'testcase', classname=classname, name=name, status='run')
        software.validate_report(root, software.SHADER_CASES)
        for tag in ('failure', 'error', 'skipped'):
            bad = copy.deepcopy(root)
            ET.SubElement(bad[0], tag)
            with self.assertRaises(RuntimeError):
                software.validate_report(bad, software.SHADER_CASES)
        for field, value in (('name', 'substitute'), ('classname', 'other'), ('status', 'notrun'),
                             ('result', 'suppressed')):
            bad = copy.deepcopy(root)
            bad[0].set(field, value)
            with self.assertRaises(RuntimeError):
                software.validate_report(bad, software.SHADER_CASES)
        for bad in (ET.Element('testsuites'), ET.fromstring(ET.tostring(root))):
            if len(bad):
                bad.append(copy.deepcopy(bad[0]))
            with self.assertRaises(RuntimeError):
                software.validate_report(bad, software.SHADER_CASES)

    def test_runner_profiles_forward_output_and_force_software_without_native_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            for profile, expected in (('static-preview', software.EXPECTED_CASES),
                                      ('shader-scenes', software.SHADER_CASES)):
                output = Path(temporary) / profile

                def execute(command, **kwargs):
                    env = kwargs['env']
                    self.assertEqual(env['LIBGL_ALWAYS_SOFTWARE'], '1')
                    self.assertEqual(env['GALLIUM_DRIVER'], 'llvmpipe')
                    self.assertEqual(env['DISPLAY'], ':42')
                    if profile == 'shader-scenes':
                        self.assertEqual(env['LUMINUMBRA_SHADER_SCENE_OUTPUT'], str(output.resolve()))
                    root = ET.Element('testsuites')
                    for classname, name in sorted(expected):
                        ET.SubElement(root, 'testcase', classname=classname, name=name, status='run')
                    self.assertEqual(command, ['fixture-binary', '--gtest_output=xml:' + str(output / 'tests.xml')])
                    (output / 'tests.xml').write_bytes(ET.tostring(root))
                    return SimpleNamespace(returncode=0, stdout=b'fixture output')

                with patch.object(sys, 'argv', ['run_software_gl.py', '--binary', 'fixture-binary',
                                               '--output', str(output), '--profile', profile]), \
                        patch.object(software.subprocess, 'Popen') as server, \
                        patch.object(software.subprocess, 'run', side_effect=execute), \
                        patch.object(software.selectors, 'DefaultSelector') as selector, \
                        patch.object(software.os, 'read', return_value=b'42\n'):
                    server.return_value.poll.return_value = 0
                    selector.return_value.select.return_value = [True]
                    software.main()
                    self.assertTrue((output / 'tests.log').is_file())

    def test_runner_output_parent_allocates_a_fresh_capture_per_invocation(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary) / 'shader-scene-capture'
            outputs = []

            def execute(command, **kwargs):
                output = Path(kwargs['env']['LUMINUMBRA_SHADER_SCENE_OUTPUT'])
                self.assertEqual(output.parent, parent.resolve())
                outputs.append(output)
                root = ET.Element('testsuites')
                for classname, name in sorted(software.SHADER_CASES):
                    ET.SubElement(root, 'testcase', classname=classname, name=name, status='run')
                (output / 'tests.xml').write_bytes(ET.tostring(root))
                return SimpleNamespace(returncode=0, stdout=b'fixture output')

            for _ in range(2):
                with patch.object(sys, 'argv', ['run_software_gl.py', '--binary', 'fixture-binary',
                                               '--output-parent', str(parent), '--profile',
                                               'shader-scenes']), \
                        patch.object(software.subprocess, 'Popen') as server, \
                        patch.object(software.subprocess, 'run', side_effect=execute), \
                        patch.object(software.selectors, 'DefaultSelector') as selector, \
                        patch.object(software.os, 'read', return_value=b'42\n'):
                    server.return_value.poll.return_value = 0
                    selector.return_value.select.return_value = [True]
                    software.main()
            self.assertEqual(len(outputs), 2)
            self.assertNotEqual(outputs[0], outputs[1])
            for output in outputs:
                self.assertTrue((output / 'tests.log').is_file())
            with patch.object(sys, 'argv', ['run_software_gl.py', '--binary', 'fixture-binary',
                                           '--output', str(parent / 'a'), '--output-parent',
                                           str(parent)]), self.assertRaises(SystemExit):
                software.main()

    def test_fixture_hash_reconfigures_and_scene_demand_provisions_native_setup(self):
        cmake = (ROOT / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        block = cmake.split('if(LUMINUMBRA_SHADER_SCENES)', 1)[1].split('\nendif()', 1)[0]
        self.assertLess(block.index('CMAKE_CONFIGURE_DEPENDS'), block.index('string(SHA256 _shader_scene_hash'))
        self.assertIn('"${CMAKE_SOURCE_DIR}/test/rendering/shader_scene_render_test.cpp")', block)
        workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8').split('  fast-compile:', 1)[1].split('  tidy-shard:', 1)[0]
        demand = "steps.shader-scenes.outputs.needs_shader_scenes == 'true'"
        for step in ('Install fast validation dependencies', 'Restore fast dependency sources',
                     'Populate missing fast dependency sources',
                     'Verify and stage isolated fast dependency sources',
                     'Configure GCC and Clang databases and generate headers'):
            with self.subTest(step=step):
                block = workflow.split('- name: ' + step, 1)[1].split('      - name:', 1)[0]
                condition = next(line for line in block.splitlines() if line.strip().startswith('if:'))
                self.assertIn("steps.route.outputs.native != 'false' || " + demand, condition)
        render = workflow.split('- name: Render and compare software shader scenes', 1)[1].split('      - name:', 1)[0]
        self.assertNotIn('steps.route.outputs.native', render)

    def test_ci_registration_and_gate_share_reference_and_consumer(self):
        self.assertIn(('tools/gates/test_shader_scenes.py',), paths.python_test_commands(ROOT))
        cmake = (ROOT / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('add_test(NAME ShaderScenesPython', cmake)
        self.assertIn('add_test(NAME ShaderSceneSoftwareGl', cmake)
        self.assertLess(cmake.index('add_test(NAME ShaderScenesPython'), cmake.index('add_test(NAME RenderBenchmarkAssets'))
        workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8').split('  fast-compile:', 1)[1].split('  tidy-shard:', 1)[0]
        for step in ('Render and compare software shader scenes', 'Upload software shader scenes'):
            block = workflow.split('- name: ' + step, 1)[1].split('      - name:', 1)[0]
            self.assertIn("steps.shader-scenes.outputs.needs_shader_scenes == 'true'", block)
        self.assertIn('python3 -I -B tools/ci/ci_paths.py classify-shader-scenes', workflow)
        self.assertIn('python3 -I -B tools/gates/test_shader_scenes.py', workflow)
        configure_calls = [line for line in workflow.splitlines()
                           if 'cmake ' in line and ' -S . ' in line]
        self.assertEqual(len(configure_calls), 2)
        for call in configure_calls:
            self.assertIn('"-DPython3_EXECUTABLE=$(python3 -I -B -c '
                          "'import sys; print(sys.executable)')\"", call)
        self.assertIn('--target shader_scene_render_test', workflow)
        self.assertEqual(workflow.count('-DLUMINUMBRA_SHADER_SCENES="${{ steps.shader-scenes.outputs.needs_shader_scenes }}"'), 2)
        self.assertIn('tools/gates/shader-scenes.py compare', workflow)
        self.assertNotIn('continue-on-error', workflow.split('- name: Render and compare', 1)[1].split('- name: Replay', 1)[0])

    def test_scene_registration_shares_graphics_lock_only_when_enabled(self):
        source = (ROOT / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        start = source.index('    add_test(NAME ShaderSceneSoftwareGl')
        end = source.index('\nendif()', start)
        registration = source[start:end]
        with tempfile.TemporaryDirectory(prefix='shader scene registration ') as temporary:
            for enabled in ('OFF', 'ON'):
                with self.subTest(enabled=enabled):
                    project = Path(temporary) / enabled
                    project.mkdir()
                    build = project / 'build with spaces'
                    (project / 'CMakeLists.txt').write_text(
                        'cmake_minimum_required(VERSION 3.20)\n'
                        'project(ShaderSceneRegistration LANGUAGES NONE)\n'
                        'enable_testing()\n'
                        f'set(CMAKE_SOURCE_DIR [=[{ROOT.as_posix()}]=])\n'
                        f'set(Python3_EXECUTABLE [=[{Path(sys.executable).as_posix()}]=])\n'
                        'add_executable(shader_scene_render_test IMPORTED)\n'
                        'set_target_properties(shader_scene_render_test PROPERTIES\n'
                        f'    IMPORTED_LOCATION [=[{Path(sys.executable).as_posix()}]=])\n'
                        f'if({enabled})\n{registration}\nendif()\n', encoding='utf-8')
                    subprocess.run(['cmake', '-S', str(project), '-B', str(build), '-G', 'Ninja'],
                                   check=True, capture_output=True, text=True)
                    # Inspect registration metadata; never execute the native scene.
                    result = subprocess.run(['ctest', '--test-dir', str(build), '--show-only=json-v1'],
                                            check=True, capture_output=True, text=True)
                    tests = json.loads(result.stdout)['tests']
                    if enabled == 'OFF':
                        self.assertEqual(tests, [])
                        continue
                    self.assertEqual([test['name'] for test in tests], ['ShaderSceneSoftwareGl'])
                    properties = {item['name']: item['value'] for item in tests[0]['properties']}
                    self.assertEqual(properties.get('RESOURCE_LOCK'), ['luminumbra_gl_context'])
                    self.assertEqual(properties['TIMEOUT'], 180)
                    self.assertEqual(tests[0]['command'], [
                        Path(sys.executable).as_posix(), '-I', '-B',
                        (ROOT / 'tools/static_preview/run_software_gl.py').as_posix(),
                        '--binary', Path(sys.executable).as_posix(), '--profile', 'shader-scenes',
                        '--output-parent', (build / 'shader-scene-capture').as_posix()])
                    for forbidden in ('DISABLED', 'SKIP_RETURN_CODE', 'SKIP_REGULAR_EXPRESSION',
                                      'LABELS', 'RUN_SERIAL'):
                        self.assertNotIn(forbidden, properties)


if __name__ == '__main__':
    unittest.main()
