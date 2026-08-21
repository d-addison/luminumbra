"""Unit tests for the Blender-independent geometry-nodes bake contract."""

from __future__ import annotations

import json
import py_compile
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIRECTORY))

from geonodes_bake_core import (  # noqa: E402
    BakeConfigurationError,
    arguments_after_separator,
    build_export_settings,
    build_result_manifest,
    default_manifest_path,
    default_output_name,
    manifest_json,
    merge_parameters,
    normalize_output_path,
    parse_cli_args,
    parse_parameters_json,
    validate_parameters,
)


class ParameterTests(unittest.TestCase):
    def test_parse_and_canonicalize_parameter_object(self) -> None:
        parameters = parse_parameters_json(
            '{"Width": 2.5, "Seed": 17, "Enabled": true, "Offset": [1, 2, 3]}'
        )
        self.assertEqual(list(parameters), ["Enabled", "Offset", "Seed", "Width"])
        self.assertEqual(parameters["Offset"], [1, 2, 3])

    def test_rejects_non_object_and_non_finite_json(self) -> None:
        with self.assertRaisesRegex(BakeConfigurationError, "must contain an object"):
            parse_parameters_json("[1, 2]")
        with self.assertRaisesRegex(BakeConfigurationError, "non-finite"):
            parse_parameters_json('{"Scale": NaN}')

    def test_rejects_nested_and_oversized_parameter_values(self) -> None:
        with self.assertRaisesRegex(BakeConfigurationError, "two to four"):
            validate_parameters({"Curve": [1, 2, 3, 4, 5]})
        with self.assertRaisesRegex(BakeConfigurationError, "numbers or booleans"):
            validate_parameters({"Curve": [1, [2, 3]]})

    def test_rejects_case_ambiguous_names(self) -> None:
        with self.assertRaisesRegex(BakeConfigurationError, "differ only by case"):
            validate_parameters({"Density": 1, "density": 2})

    def test_merge_uses_later_case_insensitive_override(self) -> None:
        merged = merge_parameters({"Density": 1.0, "Height": 3}, {"density": 2.0})
        self.assertEqual(merged, {"density": 2.0, "Height": 3})


class NamingTests(unittest.TestCase):
    def test_default_name_is_portable_and_seeded(self) -> None:
        self.assertEqual(default_output_name("Oak Crown #1", 42), "oak-crown-1-seed-42.glb")
        self.assertEqual(default_output_name("Érable", -7), "erable-seed-neg7.glb")

    def test_output_suffix_is_normalized(self) -> None:
        self.assertEqual(normalize_output_path("build/tree", "Tree", 1), Path("build/tree.glb"))
        self.assertEqual(normalize_output_path("build/tree.GLB", "Tree", 1), Path("build/tree.glb"))
        with self.assertRaisesRegex(BakeConfigurationError, "must end in .glb"):
            normalize_output_path("build/tree.gltf", "Tree", 1)

    def test_manifest_name_is_paired_with_output(self) -> None:
        self.assertEqual(default_manifest_path(Path("out/tree.glb")), Path("out/tree.bake.json"))


class CommandLineTests(unittest.TestCase):
    def test_extracts_only_script_arguments(self) -> None:
        argv = ["blender", "-b", "scene.blend", "--", "--object", "Tree", "--seed", "3"]
        self.assertEqual(arguments_after_separator(argv), ["--object", "Tree", "--seed", "3"])
        self.assertEqual(arguments_after_separator(["blender", "-b", "scene.blend"]), [])

    def test_cli_combines_json_seed_and_overrides(self) -> None:
        request = parse_cli_args(
            [
                "--object",
                "Tree",
                "--output",
                "build/tree",
                "--params-json",
                '{"seed": 9, "Density": 1.0}',
                "--param",
                "density=2.5",
                "--frame",
                "12",
            ]
        )
        self.assertEqual(request.seed, 9)
        self.assertEqual(request.parameters, {"density": 2.5, "seed": 9})
        self.assertEqual(request.output_path, Path("build/tree.glb"))
        self.assertEqual(request.manifest_path, Path("build/tree.bake.json"))
        self.assertEqual(request.frame, 12)

    def test_cli_reads_parameter_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "params.json"
            path.write_text('{"Seed": 5, "Branches": 8}', encoding="utf-8")
            request = parse_cli_args(["--object", "Tree", "--params-json", f"@{path}"])
        self.assertEqual(request.parameters, {"Branches": 8, "seed": 5})
        self.assertEqual(request.output_path, Path("tree-seed-5.glb"))

    def test_cli_requires_seed_and_rejects_conflict(self) -> None:
        with self.assertRaisesRegex(BakeConfigurationError, "explicit integer seed"):
            parse_cli_args(["--object", "Tree"])
        with self.assertRaisesRegex(BakeConfigurationError, "conflicts"):
            parse_cli_args(
                ["--object", "Tree", "--seed", "4", "--params-json", '{"seed": 5}']
            )


class ExportProfileTests(unittest.TestCase):
    def test_restricted_profile_is_fully_pinned(self) -> None:
        settings = build_export_settings(Path("out.glb"))
        self.assertEqual(settings["filepath"], "out.glb")
        self.assertEqual(settings["export_format"], "GLB")
        self.assertTrue(settings["use_selection"])
        self.assertTrue(settings["export_apply"])
        self.assertTrue(settings["export_yup"])
        self.assertTrue(settings["export_texcoords"])
        self.assertTrue(settings["export_normals"])
        self.assertEqual(settings["export_influence_nb"], 4)
        for key in (
            "export_all_influences",
            "export_morph",
            "export_morph_animation",
            "export_cameras",
            "export_lights",
            "export_draco_mesh_compression_enable",
            "export_meshopt_compression_enable",
            "export_use_gltfpack",
            "export_extras",
        ):
            self.assertFalse(settings[key], key)

    def test_manifest_is_canonical_and_complete(self) -> None:
        validation = {"path": "tree.glb", "valid": True, "profile": "static", "findings": []}
        manifest = build_result_manifest(
            source_blend="scene.blend",
            object_name="Tree",
            output_path="tree.glb",
            frame=1,
            seed=5,
            parameters={"seed": 5, "Density": 2.0},
            blender_version="4.3.2",
            vertex_count=100,
            triangle_count=50,
            material_count=1,
            content_sha256="a" * 64,
            validation=validation,
        )
        self.assertEqual(manifest["export_profile"], "restricted-glb")
        self.assertEqual(manifest["parameters"], {"Density": 2.0, "seed": 5})
        self.assertEqual(manifest["geometry"], {"vertices": 100, "triangles": 50, "materials": 1})
        self.assertIs(manifest["validation"]["valid"], True)
        serialized = manifest_json(manifest)
        self.assertTrue(serialized.endswith("\n"))
        self.assertEqual(json.loads(serialized), manifest)

    def test_manifest_rejects_bad_hash_and_counts(self) -> None:
        arguments = dict(
            source_blend="scene.blend",
            object_name="Tree",
            output_path="tree.glb",
            frame=1,
            seed=5,
            parameters={"seed": 5},
            blender_version="4.3.2",
            vertex_count=3,
            triangle_count=1,
            material_count=1,
            content_sha256="bad",
            validation={"valid": True},
        )
        with self.assertRaisesRegex(BakeConfigurationError, "SHA-256"):
            build_result_manifest(**arguments)
        arguments["content_sha256"] = "b" * 64
        arguments["triangle_count"] = -1
        with self.assertRaisesRegex(BakeConfigurationError, "triangle count"):
            build_result_manifest(**arguments)


class SeamTests(unittest.TestCase):
    def test_core_does_not_import_blender(self) -> None:
        source = (SCRIPT_DIRECTORY / "geonodes_bake_core.py").read_text(encoding="utf-8")
        self.assertNotIn("import bpy", source)

    def test_blender_layer_compiles_without_importing_bpy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            compiled = Path(temporary_directory) / "geonodes_bake.pyc"
            py_compile.compile(
                str(SCRIPT_DIRECTORY / "geonodes_bake.py"),
                cfile=str(compiled),
                doraise=True,
            )
            self.assertTrue(compiled.is_file())


if __name__ == "__main__":
    unittest.main()
