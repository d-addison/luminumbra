"""Offline source contracts for the optional profile build (issue #395)."""

import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_configuration():
    cmake = read("cmake/tracy.cmake")
    code = re.sub(r"(?m)#.*$", "", cmake)
    require(re.search(r"option\(LUMINUMBRA_ENABLE_TRACY\s+\"[^\"]*\"\s+OFF\)", code),
            "Tracy must default to OFF")
    # All dependency operations belong to the enabled branch. The default branch
    # is an empty interface with a status message, without a network operation.
    enabled = code.split("if(LUMINUMBRA_ENABLE_TRACY)", 1)[1].split("else()", 1)
    require("FetchContent" not in code.split("if(LUMINUMBRA_ENABLE_TRACY)", 1)[0],
            "No Tracy fetch before the opt-in gate")
    require("FetchContent_MakeAvailable(tracy)" in enabled[0], "Missing opt-in fetch")
    require("FetchContent" not in enabled[1] and "target_link_libraries" not in enabled[1]
            and "target_compile_definitions" not in enabled[1], "Default must be profiler-free")
    presets = json.loads(read("CMakePresets.json"))
    configurations = {p["name"]: p for p in presets["configurePresets"]}

    def variables(name):
        preset = configurations[name]
        result = {}
        parents = preset.get("inherits", [])
        if isinstance(parents, str):
            parents = [parents]
        for parent in reversed(parents):
            result.update(variables(parent))
        result.update(preset.get("cacheVariables", {}))
        return result

    for name in configurations:
        expected = "ON" if name == "profile" else "OFF"
        require(variables(name).get("LUMINUMBRA_ENABLE_TRACY") == expected,
                f"Unexpected Tracy setting in {name}")
    require(variables("profile")["CMAKE_BUILD_TYPE"] == "RelWithDebInfo",
            "Profile needs optimized symbols")
    require(any(p["name"] == "profile" and p["configurePreset"] == "profile"
                for p in presets["buildPresets"]), "Missing profile build preset")
    common = read("src/luminumbra_common/CMakeLists.txt")
    require(re.search(r"target_link_libraries\(luminumbra_common PUBLIC\s+luminumbra_profiler", common),
            "Common worker code and public headers must share the profile definition")


def check_disabled():
    header = read("src/luminumbra_common/core/Profiler.h")
    disabled = header.split("\n#else\n", 1)[1].split("#endif", 1)[0]
    macros = ("FRAME", "FRAME_N", "ZONE", "ZONE_N", "PLOT", "MSG", "THREAD")
    for macro in macros:
        require(re.search(r"^#define LUMIN_PROFILE_" + macro + r"\([^\n)]*\)[ \t]*$",
                          disabled, re.MULTILINE), f"Disabled {macro} must expand to nothing")
    require("#define LUMIN_PROFILE_LOCK(type, variable, mutex, name) type variable(mutex)" in disabled,
            "Disabled locks must preserve the original declaration")
    require("#define LUMIN_PROFILE_WAIT(name, ...) (__VA_ARGS__)" in disabled,
            "Disabled waits must preserve exactly one evaluation")
    tests = read("test/CMakeLists.txt")
    require("add_test(NAME ProfilerSourceContract" in tests, "Source contract must be registered")
    require("add_library(profiler_disabled_compile OBJECT core/profiler_disabled_compile.cpp)" in tests,
            "Missing compile-only disabled-macro target")
    require(not re.search(r"target_link_libraries\(profiler_disabled_compile\b", tests),
            "Disabled compile target must not inherit instrumentation")
    fixture = read("test/core/profiler_disabled_compile.cpp")
    for macro in macros:
        require("PROFILE_STRING(LUMIN_PROFILE_" + macro + "(" in fixture,
                f"Missing compile-only expansion assertion for {macro}")
    job_header = read("src/luminumbra_common/core/JobSystem.h")
    require(re.search(r"#if defined\(LUMINUMBRA_ENABLE_TRACY\)\s+"
                      r"std::chrono::steady_clock::time_point enqueued_at\{\};\s+#endif", job_header),
            "Queue timestamp storage must disappear when disabled")


def check_disabled_gate():
    # Exercise the gate's actual regexes and allowlist without PowerShell. Only
    # named captures and the absolute end anchor need translation for Python.
    gate = read("tools/gates/validate-engine-frontier.ps1").split(
        "function Test-ProfilerDeterminismNeutral {", 1)[1].split(
        "\nfunction ", 1)[0]

    def pattern(name):
        match = re.search(r"\$" + name + r" = '([^']*)'", gate)
        require(match, f"Missing disabled gate pattern: {name}")
        expression = re.sub(r"\(\?<([a-z]+)>", r"(?P<\1>", match[1])
        return re.compile(expression.replace(r"\z", r"\Z"))

    comments = pattern("commentPattern")
    shape = pattern("shimPattern")
    conditional = pattern("enabledDirectivePattern")
    definition_block = re.search(r"\$disabledDefinitions = @\((.*?)\n\s*\)", gate, re.DOTALL)
    require(definition_block, "Gate must enumerate all disabled definitions")
    definitions = re.findall(r"'([^']+)'", definition_block[1])
    expected = ["#define LUMIN_PROFILE_" + event for event in (
        "FRAME()", "FRAME_N(name)", "ZONE()", "ZONE_N(name)",
        "PLOT(name, val)", "MSG(txt, size)", "THREAD(name)",
        "LOCK(type, variable, mutex, name) type variable(mutex)",
        "WAIT(name, ...) (__VA_ARGS__)")]
    require(definitions == expected, "Gate and disabled macro contracts must agree")
    for guard in (
            "[regex]::Replace($shimTxt, $commentPattern, '')",
            "[regex]::Match($shimCode, $shimPattern)",
            "-not $shimMatch.Success",
            "[regex]::IsMatch($shimMatch.Groups['enabled'].Value, $enabledDirectivePattern)",
            "$shimMatch.Groups['disabled'].Value -split '\\r?\\n'",
            "$definition = $line.Trim()",
            "$definition -cnotin $disabledDefinitions",
            "-not $seenDefinitions.Add($definition)",
            "[System.StringComparer]::Ordinal",
            "-not $seenDefinitions.Contains($definition)"):
        require(guard in gate, f"Missing disabled gate guard: {guard}")

    def accepts(header):
        match = shape.search(comments.sub("", header))
        if not match or conditional.search(match["enabled"]):
            return False
        lines = [line.strip() for line in match["disabled"].splitlines() if line.strip()]
        return len(lines) == len(definitions) and set(lines) == set(definitions)

    header = read("src/luminumbra_common/core/Profiler.h")
    for newline in ("\n", "\r\n"):
        candidate = header.replace("\n", newline)
        require(accepts(candidate), "Disabled gate must accept empty macros with LF or CRLF")
        enabled, separator, disabled = candidate.partition("#else" + newline)
        require(separator, "Missing disabled branch in mutation fixture")
        for definition in definitions:
            for replacement in ("", definition + " SideEffect()",
                                definition + " \\" + newline + " SideEffect()",
                                definition + newline + definition):
                changed = enabled + separator + disabled.replace(definition, replacement, 1)
                require(not accepts(changed),
                        f"Disabled gate accepted a missing, active or duplicate macro: {definition}")
        for injected in ('#include <tracy/Tracy.hpp>', 'int profiler_symbol;',
                         '#define EXTRA_EVENT() SideEffect()'):
            for changed in (injected + newline + candidate,
                            candidate + newline + injected,
                            candidate.replace("#else", "#else" + newline + injected, 1)):
                require(not accepts(changed), "Disabled gate accepted an include or symbol")
        require(not accepts(candidate.replace("#else", "#if 1" + newline + "#else", 1)),
                "Disabled gate must reject an ambiguous nested conditional")
        require(not accepts(candidate.replace("(__VA_ARGS__)",
                                              "((__VA_ARGS__), (__VA_ARGS__))")),
                "Disabled wait must preserve exactly one evaluation")


def function_body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 0
    # Ignore braces in comments and strings when locating the function's end.
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]',
                         source[start:], re.DOTALL)
    for token in tokens:
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:start + token.start()]
    raise AssertionError(f"Unclosed function: {signature}")


def check_sampling_zones():
    source = read("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
    # Scalar helpers are called per sample/cell/vertex, including warm-cache
    # hydrology hits. Neither read nor write acquisition may emit a zone here.
    for name in ("SampleHydroOffsetMeters", "ComputeShapedHeightSample",
                 "ComputeShapedHeightSampleImpl", "ComputeShapedHeight",
                 "GetTerrainHeightAtCoarse", "SurfaceVertexMaterial", "ComputeClimateSample"):
        body = function_body(source, f"SHIELD_WorldSystem::{name}(")
        require("LUMIN_PROFILE_" not in body, f"Per-sample profiler overhead in {name}")
    for name, zone in (
            ("ComputeShapedHeightGrid", "Terrain height grid batch"),
            ("ComputeShapedHeightsAtPositions", "Terrain height position batch"),
            ("PrefetchHydroRegions", "Hydrology prefetch batch")):
        body = function_body(source, f"SHIELD_WorldSystem::{name}(")
        require(body.lstrip().startswith(f'LUMIN_PROFILE_ZONE_N("{zone}");')
                and body.count("LUMIN_PROFILE_") == 1,
                f"{name} must profile once at the batch boundary, outside all loops")
    header = read("src/luminumbra_common/systems/SHIELD_WorldSystem.h")
    body = function_body(header, "acquire_worldgen_sample_scope() const")
    require(body.lstrip().startswith('LUMIN_PROFILE_ZONE_N("Worldgen epoch lock wait");')
            and body.count("LUMIN_PROFILE_") == 1,
            "Epoch retries must share one acquisition zone per sampling job")


def check_dependency():
    cmake = read("cmake/tracy.cmake")
    tag = re.search(r'set\(LUMINUMBRA_TRACY_TAG "(v[0-9.]+)"\)', cmake)
    revision = re.search(r'set\(LUMINUMBRA_TRACY_REVISION "([0-9a-f]{40})"\)', cmake)
    require(tag and revision, "Tracy requires an exact release tag and full commit hash")
    require(tag[1] == "v0.13.1"
            and revision[1] == "05cceee0df3b8d7c6fa87e9638af311dbabc63cb",
            "Tracy must use the verified v0.13.1 commit")
    require('GIT_TAG        "${LUMINUMBRA_TRACY_REVISION}"' in cmake,
            "FetchContent must use the immutable commit")
    require(not re.search(r"\bGIT_SHALLOW\s+(?:TRUE|ON|YES|1)\b", cmake, re.IGNORECASE),
            "A commit pin must not use shallow clone")
    notice = read("THIRD_PARTY_LICENSES.md")
    for token in (tag[1], revision[1], "Bartosz Taudul", "BSD-3-Clause",
                  "Redistribution and use in source and binary forms",
                  "Neither the name of the <organization>",
                  "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS"):
        require(token in notice, f"Missing Tracy notice: {token}")
    licence = re.search(r"### Tracy v0\.13\.1\n\n.*?```text\n(.*?)```", notice, re.DOTALL)
    require(licence, "Missing verbatim Tracy licence block")
    require(hashlib.sha256(licence[1].encode("utf-8")).hexdigest()
            == "16f7df4828bd89c880c17dd2f45e834b754b6d5e8c9a9ee886af1be4b89475ca",
            "Tracy licence must match the complete upstream v0.13.1 text")
    documentation = read("docs/render-benchmark.md")
    for token in (tag[1], "BSD-3-Clause", "../THIRD_PARTY_LICENSES.md#tracy-v0131"):
        require(token in documentation, f"Missing profile dependency documentation: {token}")


def check_zones():
    client = read("src/luminumbra_client/main_client.cpp")
    require('LUMIN_PROFILE_ZONE_N("Client frame")' in client
            and 'LUMIN_PROFILE_FRAME_N("Client")' in client, "Missing scoped client frame and marks")
    require(re.search(r'frame_limiter\.pace\(pacing\.frame_cap, frame_timing\);\s*'
                      r'LUMIN_PROFILE_ZONE_N\("Client frame"\);\s*'
                      r'const auto _frameStart', client),
            "Frame pacing must precede both the scoped profile zone and work timers")
    # Every presentation path, including capture and scenario paths, must keep
    # its wait zone when live settings update the swap interval.
    for source in (ROOT / "src/luminumbra_client").rglob("*.cpp"):
        text = source.read_text(encoding="utf-8")
        for call in re.finditer(r"\bglfwSwapBuffers\(window\)", text):
            require(text[:call.start()].rstrip().endswith(
                'LUMIN_PROFILE_WAIT("Client present wait",'),
                f"Missing presentation wait zone: {source.relative_to(ROOT)}")
    jobs = read("src/luminumbra_common/core/JobSystem.cpp")
    for name in ("Worker job execution", "Job queue delay (ms)", "Job queue lock wait",
                 "Job completion lock wait", "Job condition wait", "Worker condition wait"):
        require('"' + name + '"' in jobs, f"Missing worker zone/plot: {name}")
    require(jobs.count("slot.enqueued_at = std::chrono::steady_clock::now();") == 1,
            "Both queue lanes and both dispatch APIs must share one enqueue timestamp site")
    require('LUMIN_PROFILE_THREAD("Job worker")' in jobs, "Unnamed job worker")
    for path, thread, wait in (
            ("src/luminumbra_client/world/WorldgenPreview.cpp", "World preview worker", "Preview condition wait"),
            ("src/luminumbra_client/audio/AudioPropagationSystem.cpp", "Audio propagation worker", "Audio condition wait"),
            ("src/luminumbra_common/persistence/AsyncSavedWorldCatalog.cpp", "Saved world catalog worker", "Saved world catalog condition wait"),
            ("src/luminumbra_server/modes/RuntimeTcpSlot.cpp", "TCP accept worker", "TCP accept future wait")):
        source = read(path)
        require(f'LUMIN_PROFILE_THREAD("{thread}")' in source, f"Unnamed {thread}")
        require(f'LUMIN_PROFILE_WAIT("{wait}"' in source, f"Missing wait zone: {wait}")
    # Enumerate sibling shader implementations and pass entry points so additions
    # cannot silently introduce an unprofiled backend.
    for source in (ROOT / "src").rglob("*.cpp"):
        text = source.read_text(encoding="utf-8")
        for operation, name in (("CompileShader", "compile"), ("LinkProgram", "link")):
            for call in re.finditer(r"\bgl" + operation + r"\(", text):
                require(re.search(r'LUMIN_PROFILE_ZONE_N\("Shader ' + name
                                  + r'"\);\s*$', text[:call.start()]),
                        f"Unprofiled shader {name}: {source.relative_to(ROOT)}")
    for source in (ROOT / "src/luminumbra_client/rendering/passes").glob("*.cpp"):
        text = source.read_text(encoding="utf-8")
        for entry in re.finditer(r"\b(\w+Pass)::(execute\w*)\([^;{}]*\)(?: const)?\s*\{", text):
            require(text[entry.end():].lstrip().startswith(
                f'LUMIN_PROFILE_ZONE_N("GL {entry[1]}.{entry[2]}");'),
                f"Unprofiled pass: {entry[1]}.{entry[2]}")
    saves = read("src/luminumbra_common/persistence/WorldSaveService.cpp")
    for name in ("File save write", "File save replace"):
        require(saves.count('LUMIN_PROFILE_ZONE_N("' + name + '")') == 2,
                f"Windows and POSIX must both expose {name}")
    for name in ("File save read", "File world load"):
        require(f'LUMIN_PROFILE_ZONE_N("{name}")' in saves, f"Missing {name} zone")
    preset = read("src/luminumbra_common/world/TerrainPresetLoader.cpp")
    require('LUMIN_PROFILE_ZONE_N("File preset load")' in preset
            and 'LUMIN_PROFILE_ZONE_N("File preset override load")' in preset,
            "Missing preset load sibling zones")
    loader = function_body(preset, "LoadTerrainPreset(const std::filesystem::path& preset_path,")
    require(loader.lstrip().startswith('LUMIN_PROFILE_ZONE_N("File preset load");'),
            "Explicit runtime-root loads must share the file-load zone")
    wrapper = function_body(preset, "LoadTerrainPreset(const std::filesystem::path& preset_path)")
    require("return LoadTerrainPreset(preset_path," in wrapper
            and "LUMIN_PROFILE_" not in wrapper,
            "Inferred-root loads must delegate without duplicating the file-load zone")


if __name__ == "__main__":
    check_configuration()
    check_disabled()
    check_disabled_gate()
    check_sampling_zones()
    check_zones()
    check_dependency()
    print("Profiler source contracts passed")
