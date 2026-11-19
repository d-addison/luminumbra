"""Portable winding contracts for all shipping terrain SDF mesh producers.

The native meshing regression checks actual output. This source/table oracle
also exercises the disconnected-component counterexample without a renderer.
"""

import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CORNERS = ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1),
           (0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1))
EDGES = ((0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6),
         (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7))


def source(path):
    text = (ROOT / path).read_text(encoding="utf-8-sig")
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def subtract(a, b):
    return tuple(x - y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


class TerrainWindingContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mesher = source("src/luminumbra_common/world/MarchingCubes.cpp")
        table = source("src/luminumbra_common/world/MarchingCubesTables.inl")
        cls.rows = [tuple(map(int, re.findall(r"-?\d+", row))) for row in
                    re.findall(r"\{([^{}]+)\}", table.split("triTable[256][16] =", 1)[1])]

    def producers(self):
        starts = [match.start() for match in re.finditer(
            r"FarLodRegionMeshStats GenerateFarLodRegionMesh\(", self.mesher)]
        self.assertEqual(len(starts), 2)
        live = self.mesher.index("void PolygoniseTerrain(")
        return {
            "far tile": self.mesher[starts[0]:starts[1]],
            "far assembly": self.mesher[starts[1]:live],
            "live strides 1, 2, 4": self.mesher[live:self.mesher.index(
                "AddBoundaryTransitionSkirts(")],
        }

    def test_every_sdf_producer_preserves_component_winding(self):
        for name, body in self.producers().items():
            with self.subTest(producer=name):
                # These are the actual index-emission loops, not their comments.
                self.assertIn("triTable[", body)
                self.assertNotRegex(body, r"std::swap\((?:i1,\s*i2|b,\s*c)\)")
                self.assertNotRegex(body, r"dot\((?:face_normal|normal),\s*"
                                    r"(?:density_gradient|gradient)\)")
        # The separate volume mesher already follows this contract. Its edge
        # endpoint canonicalization may swap endpoints, never triangle vertices.
        volume = source("src/luminumbra_common/world/FarVolumeMesher.cpp")
        emission = volume[volume.index("for (int t = 0; triTable[configuration]"):]
        self.assertNotIn("std::swap", emission)

    def test_unequal_components_and_complements_have_opposite_outward_faces(self):
        self.assertEqual(len(self.rows), 256)
        # Four body diagonals, both amplitude orders, solids and cavities.
        # All loops are bounded by these 16 two-triangle cells.
        for first, second in ((0, 6), (1, 7), (2, 4), (3, 5)):
            for amplitudes in ((1.0, 10.0), (10.0, 1.0)):
                for polarity in (1.0, -1.0):
                    values = [polarity] * 8
                    values[first] = -polarity * amplitudes[0]
                    values[second] = -polarity * amplitudes[1]
                    mask = sum(1 << i for i, value in enumerate(values) if value < 0)
                    row = self.rows[mask]
                    self.assertEqual(row[6], -1)
                    normals = []
                    average = tuple(sum(value * (2 * corner[axis] - 1)
                                        for value, corner in zip(values, CORNERS))
                                    for axis in range(3))
                    for offset in (0, 3):
                        points = []
                        for edge in row[offset:offset + 3]:
                            a, b = EDGES[edge]
                            fraction = -values[a] / (values[b] - values[a])
                            points.append(tuple(x + fraction * (y - x)
                                                for x, y in zip(CORNERS[a], CORNERS[b])))
                        normal = cross(subtract(points[1], points[0]),
                                       subtract(points[2], points[0]))
                        center = tuple(sum(p[axis] for p in points) / 3 for axis in range(3))
                        component = min((first, second),
                                        key=lambda i: dot(subtract(center, CORNERS[i]),
                                                          subtract(center, CORNERS[i])))
                        outward = tuple(polarity * value
                                        for value in subtract(center, CORNERS[component]))
                        self.assertGreater(dot(normal, outward), 0)
                        for name, body in self.producers().items():
                            # Replay the shipping orientation decision, if present,
                            # on the real table geometry. Restoring the removed
                            # cell-average rule must fail this geometric oracle.
                            flip = re.search(r"dot\((?:face_normal|normal),\s*"
                                             r"(?:density_gradient|gradient)\) < 0.0f", body)
                            emitted = tuple(-v for v in normal) if (
                                flip and dot(normal, average) < 0) else normal
                            self.assertGreater(dot(emitted, outward), 0, name)
                        normals.append(normal)
                    self.assertLess(dot(normals[0], normals[1]), 0)
                    # Prove why the removed orientation rule cannot work here:
                    # its one direction reverses exactly one correct triangle.
                    self.assertEqual(sum(dot(n, average) < 0 for n in normals), 1)

    def test_live_normals_sample_each_vertex_instead_of_the_cell_center(self):
        compact = re.sub(r"\s+", " ", self.mesher)
        self.assertIn("EdgeDensityGradient(gridcell, e, vertices[vert_indices[e]].position)",
                      compact)
        self.assertIn("(position[axis] - gridcell.p[0][axis]) / "
                      "(gridcell.p[6][axis] - gridcell.p[0][axis])", compact)

        def table(name):
            found = re.search(rf"{name}\[[0-9]+\](?:\[3\])? = \{{(.*?)\}};", compact)
            self.assertIsNotNone(found, name)
            return [int(n) for n in re.findall(r"\d+", found[1])]

        # The edge stencil must use the live mesher's corner and edge numbering:
        # otherwise each vertex would differentiate along the wrong cell edge.
        offsets = re.search(r"const IVec3 corner_offsets\[8\] = \{(.*?)\}};", compact)
        self.assertIsNotNone(offsets)
        self.assertEqual(table("kBits"), [int(n) for n in re.findall(r"\d+", offsets[1])])
        self.assertEqual(table("kLowCorner"), table("kEdgeAnchorCorner"))
        self.assertEqual(table("kAxis"), table("kEdgeAxis"))
        # Along the edge: its own difference. Across it: two parallel
        # differences, interpolated by the edge parameter.
        self.assertIn("gradient[axis] = v[stencil.hi] - v[stencil.lo];", compact)
        self.assertIn("glm::mix(v[pair[0][1]] - v[pair[0][0]], v[pair[1][1]] - v[pair[1][0]], t)",
                      compact)
        self.assertNotIn("EstimateDensityGradient", compact)

    def test_contract_is_registered_and_runs_when_mesher_inputs_change(self):
        spec = importlib.util.spec_from_file_location("terrain_paths", ROOT / "tools/ci/ci_paths.py")
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        self.assertIn("tools/ci/test_terrain_winding_contract.py", gate.PYTHON_TESTS)
        for path in ("MarchingCubes.cpp", "MarchingCubes.h", "MarchingCubesTables.inl",
                     "FarVolumeMesher.cpp"):
            self.assertTrue(gate.needs_python("src/luminumbra_common/world/" + path), path)
        cmake = source("tools/CMakeLists.txt")
        self.assertRegex(cmake, r"NAME TerrainWindingContract\s+COMMAND "
                         r'"\$\{Python3_EXECUTABLE\}" -I -B\s+'
                         r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/ci/test_terrain_winding_contract.py"')
        for workflow in ("ci.yml", "windows-build.yml"):
            self.assertIn("tools/ci/ci_paths.py python-tests",
                          (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
