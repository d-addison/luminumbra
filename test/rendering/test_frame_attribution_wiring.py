"""Guard production attribution wiring beyond the pure bucket/count tests."""

from pathlib import Path
import ast
import hashlib
import json
import math
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "src/luminumbra_client"


def read(path):
    return path.read_text(encoding="utf-8")


def compact(text):
    return re.sub(r"\s+", " ", text).strip()


def block(text, marker):
    """Extract balanced C++ braces, ignoring comments and string literals."""
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), text, flags=re.S,
    )
    start = masked.index("{", text.index(marker))
    depth = 1
    for index in range(start + 1, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    raise AssertionError(f"Unclosed block: {marker}")


def cpp_expression(expression, **bindings):
    """Evaluate only scalar production predicates, without executing native code."""
    names = {}
    for index, (name, value) in enumerate(sorted(bindings.items(), key=lambda item: -len(item[0]))):
        token = f"value_{index}"
        expression = re.sub(r"(?<![\w.])" + re.escape(name) + r"(?![\w.])", token, expression)
        names[token] = value
    expression = expression.replace("&&", " and ").replace("||", " or ")
    expression = re.sub(r"!(?!=)", " not ", expression).replace("/", "//")
    expression = compact(re.sub(r"(?<=\d)'(?=\d)", "", expression))
    tree = ast.parse(expression, mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not,
               ast.BinOp, ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Mod,
               ast.Compare, ast.Eq, ast.NotEq, ast.Lt, ast.LtE, ast.Gt, ast.GtE,
               ast.Name, ast.Load, ast.Constant)
    if any(not isinstance(node, allowed) for node in ast.walk(tree)):
        raise AssertionError(f"Unsupported scalar expression: {expression}")
    return eval(compile(tree, "<production predicate>", "eval"), {"__builtins__": {}}, names)


class CaptureGateEvaluation:
    """Interpret the shipping scalar gate; this does not execute C++ or GL."""

    def __init__(self):
        self.source = block(read(CLIENT / "app/MenuSceneSchedule.h"), "class MenuCaptureGate")
        self.state = {}
        for name, assigned, braced in re.findall(
                r"(?:Time|std::size_t|bool) (\w+)(?:\s*=\s*([^;]+)|\{([^}]*)\});",
                self.source[self.source.index("private:"):]):
            expression = assigned or braced or "0"
            self.state[name] = cpp_expression(expression, true=True, false=False)

    def call(self, method, **inputs):
        body = block(self.source, method)
        body = re.sub(r"std::chrono::seconds\{(\d+)\}", r"\1", body)
        statement = re.compile(
            r"if\s*\(([^()]*)\)\s*return\s+([^;]+);|"
            r"return\s+([^;]+);|(\w+)\s*=\s*([^;]+);"
        )
        position = 0
        for match in statement.finditer(body):
            if body[position:match.start()].strip():
                raise AssertionError("Unsupported production gate control flow")
            position = match.end()
            condition, guarded_return, returned, name, assigned = match.groups()
            values = dict(self.state, **inputs, true=True, false=False)
            if assigned is not None:
                self.state[name] = cpp_expression(assigned, **values)
            elif condition is None or cpp_expression(condition, **values):
                expression = guarded_return if condition is not None else returned
                if expression.startswith("Result::"):
                    return expression.removeprefix("Result::")
                return cpp_expression(expression, **values)
        if body[position:].strip():
            raise AssertionError("Incomplete production gate control flow")
        return None

    def arm(self, now=0, index=0, thumbnail=False):
        return self.call("bool arm(", now=now, index=index, thumbnail=thumbnail)

    def decide(self, now, settled=False, drawable=True, content_ready=True):
        return self.call("Result decide(", now=now, settled=settled,
                         drawable=drawable, content_ready=content_ready)


class MenuCaptureEvaluation:
    """Trace production capture predicates, including the pre-deadline path.

    This scalar policy evaluation supplies render/readiness observations. It is
    not a renderer, and provides no native screenshot qualification.
    """

    def __init__(self, live=False):
        source = read(CLIENT / "app/MenuScreens.cpp")
        self.menu = block(source, "void RenderMenuScreens(")
        self.gate = CaptureGateEvaluation() if "if (!PrepareMenuCapture(" in self.menu else None
        self.live = live
        self.count = 0
        self.start = None
        regular = re.sub(r"//[^\n]*", "", block(self.menu, "else if (app.capture.ui_screenshot_settle <"))
        self.admission = re.fullmatch(r"\s*if \((.*?)\)\s*"
                                      r"\+\+app.capture.ui_screenshot_settle;\s*", regular, re.S).group(1)
        self.required = int(re.search(r"else if \(app.capture.ui_screenshot_settle < (\d+)\)", self.menu).group(1))
        self.content = re.search(r"const bool content_ready\s*=\s*(.*?);", source, re.S)

    def frame(self, now, rendered=False, preview=True, ready=False,
              drawable=True, failed=False, document=True, index=0, source_failed=False,
              candidate_ready=None):
        if self.start is None:
            self.start = now
        if self.gate:
            if self.gate.arm(now, index=index):
                self.count = 0
                self.start = now
            if self.gate.decide(now) == "TimedOut":
                return "TimedOut"
        values = dict(scene_rendered=rendered, scenic=False, previewActive=preview)
        if self.live:
            live = block(self.menu, "if (preview_live_screen)")
            live = live[live.index("const bool world_ready"):]
            reset = re.search(r"if \((.*?)\)\s*app.capture.ui_preview_settle_after_ready = 0;", live, re.S).group(1)
            increment = re.search(r"else if \((.*?)\)\s*\+\+app.capture.ui_preview_settle_after_ready;", live, re.S).group(1)
            values["world_ready"] = ready
            if cpp_expression(reset, **values):
                self.count = 0
            elif cpp_expression(increment, **values):
                self.count += 1
            expressions = dict(re.findall(r"const bool (settled|timed_out) = (.*?);", live, re.S))
            constants = dict((name, float(number)) for name, number in re.findall(
                r"constexpr (?:double|int) (\w+) =\s*(\d+(?:\.\d+)?);", live))
            values.update(constants)
            values.update({"app.capture.ui_preview_settle_after_ready": self.count,
                           "waited_s": now - self.start})
            for name in ("settled", "timed_out"):
                values[name] = cpp_expression(expressions[name], **values)
            requested = cpp_expression(re.search(r"ready_to_capture = (.*?);", live, re.S).group(1), **values)
        else:
            requested = self.count >= self.required
            if not requested and cpp_expression(self.admission, **values):
                self.count += 1
        content_ready = True
        if self.content:
            content_ready = cpp_expression(self.content.group(1), **{
                "previewState.capture_document_ready": document,
                "app.menu.menu_backdrop_active": False, "scenic": False,
                "expects_preview": preview, "previewActive": preview,
                "worldgenPreview": preview, "worldgenPreview->world_ready()": ready,
                "worldgenPreview->candidate_ready()": ready if candidate_ready is None else candidate_ready,
                "worldgenPreview->last_build_failed()": failed,
                "previewState.candidate_source_failed": source_failed})
            if not content_ready or not drawable:
                self.count = 0
                requested = False
        if self.gate:
            return self.gate.decide(now, settled=requested, drawable=drawable,
                                    content_ready=content_ready)
        return "Ready" if requested else "Waiting"


class GameplayPhotoRelease(unittest.TestCase):
    """Trace the shipping exit boundary without running native code or GL."""

    def setUp(self):
        self.main = read(CLIENT / "main_client.cpp")
        self.overlays = read(CLIENT / "app/DebugOverlays.cpp")
        self.photo_header = read(CLIENT / "app/PhotoEnvironment.h")
        self.player = True
        self.exposure = 4.0
        self.exposure_applied = True
        self.hold = True
        self.weather = "Storm"
        self.driven = True
        self.weather_payload = {"rain_intensity": 0.6, "storm_intensity": 0.8,
                                "wetness": 0.9, "wind_strength": 0.4}
        self.photo_env = True
        self.weather_snapshot = None
        self.photo = True
        self.photo_ui = True
        self.releases = 0

    def restore_photo_weather(self):
        restore = compact(block(self.photo_header, "void Restore("))
        self.assertTrue(restore.startswith("if (!applied_) { return; }"), restore)
        self.assertIn("if (state_.driven) { pipeline.set_weather_state(state_); } "
                      "else { pipeline.set_weather(weather_, intensity_); } "
                      "applied_ = false;", restore)
        if self.weather_snapshot is None:
            return
        self.weather, self.driven, self.weather_payload = self.weather_snapshot
        self.weather_snapshot = None

    def release_photo_exposure(self, photo_active):
        release = compact(block(self.photo_header, "void ReleaseIfInactive("))
        self.assertEqual(release, "if (photo_mode_active || !applied_) { return; } "
                         "pipeline.set_exposure_override(-1.0f); applied_ = false;")
        if photo_active or not self.exposure_applied:
            return
        self.exposure = -1.0
        self.exposure_applied = False

    def leave_photo_environment(self):
        """One gameplay frame after photo mode turned off, without a retirement."""
        frame = self.main[self.main.index("while (!glfwWindowShouldClose(window))"):]
        frame = block(frame, "while (!glfwWindowShouldClose(window))")
        self.assertIn("g_app.hud.photoWeather.Restore(renderPipeline);", frame)
        self.restore_photo_weather()
        update = block(self.overlays, "void UpdatePhotoEnvironment(")
        self.assertIn("app.hud.photoExposure.ReleaseIfInactive(renderPipeline, photo_active);",
                      update)
        self.release_photo_exposure(self.photo)
        self.assertIn("if (!photo_active || capture_pinned) { photo = {}; }", compact(update))
        if not self.photo:
            self.photo_env = False

    def apply_photo_weather(self, index):
        update = compact(block(self.overlays, "void UpdatePhotoEnvironment("))
        self.assertIn("if (selected.weather_active) { app.hud.photoWeather.Apply("
                      "renderPipeline, selected.weather, selected.intensity); }", update)
        apply = compact(block(self.photo_header, "void Apply(Pipeline& pipeline, "
                              "Luminumbra::Rendering::WeatherType"))
        self.assertIn("if (!applied_) { weather_ = pipeline.get_weather_type(); "
                      "intensity_ = pipeline.get_weather_intensity(); "
                      "state_ = pipeline.get_weather_state(); applied_ = true; }", apply)
        presets = re.search(r"constexpr std::array presets = \{([^}]+)\}",
                            self.photo_header).group(1)
        types = [entry.strip().split("::")[-1] for entry in presets.split(",")]
        self.assertEqual(len(types), 6)
        if self.weather_snapshot is None:
            self.weather_snapshot = (self.weather, self.driven, dict(self.weather_payload))
        self.photo_env = True
        self.weather = types[index]
        self.driven = False

    def retire(self):
        if "auto release_player_controller =" not in self.main:
            self.player = False
            return
        owner = compact(block(self.main, "auto release_player_controller ="))
        self.assertEqual(owner, "if (g_playerController) { "
                         "ReleasePhotoModeAndHud(g_app, renderPipeline); "
                         "g_playerController.reset(); }")
        if not self.player:
            return
        self.releases += 1
        release = compact(re.sub(r"//[^\n]*", "",
                                 block(self.overlays, "void ReleasePhotoModeAndHud(")))
        if "app.hud.photoMode = {};" in release:
            self.photo = False
        if "app.hud.photoModeUiShown = false;" in release:
            self.photo_ui = False
        if "app.hud.photoEnvironment = {};" in release:
            self.photo_env = False
        if "app.hud.photoWeather.Restore(renderPipeline);" in release:
            self.restore_photo_weather()
        if "app.hud.photoExposure.ReleaseIfInactive(renderPipeline, false);" in release:
            self.release_photo_exposure(False)
        for method, value in re.findall(r"renderPipeline\.(set_\w+)\(([^;]*)\);", release):
            if method == "set_time_of_day_hold":
                self.hold = cpp_expression(value, false=False, true=True)
            else:
                self.fail(f"release writes an unowned renderer setting: {method}")
        self.player = False

    def assert_released(self):
        self.assertEqual(self.exposure, -1.0, "manual exposure leaked into the menu preview")
        self.assertFalse(self.exposure_applied)
        self.assertFalse(self.hold)
        self.assertEqual(self.weather, "Storm")
        self.assertTrue(self.driven)
        self.assertFalse(self.photo_env)
        self.assertIsNone(self.weather_snapshot)
        self.assertFalse(self.photo)
        self.assertFalse(self.photo_ui)
        self.assertFalse(self.player)
        self.assertEqual(self.releases, 1)

    def test_quit_to_menu_releases_manual_exposure_before_preview(self):
        quit_body = block(self.main, 'else if (act == "quit")')
        retirement = ("release_player_controller();" if "release_player_controller();" in quit_body
                      else "g_playerController.reset();")
        self.assertLess(quit_body.index(retirement), quit_body.index("GameState::MAIN_MENU"))
        self.retire()
        self.assert_released()

    def test_release_preserves_weather_and_exposure_without_a_photo_override(self):
        for engaged, driven in ((False, False), (False, True), (True, False), (True, True)):
            with self.subTest(engaged=engaged, driven=driven):
                self.setUp()
                self.photo_env = engaged
                self.driven = driven
                self.exposure_applied = False
                before = (self.weather, self.driven, dict(self.weather_payload), self.exposure)
                self.retire()
                self.assertEqual(
                    (self.weather, self.driven, self.weather_payload, self.exposure), before)

    def test_each_photo_weather_override_restores_driven_weather_on_both_exits(self):
        for index in range(1, 6):
            for retire_player in (False, True):
                with self.subTest(index=index, retire_player=retire_player):
                    self.setUp()
                    payload = dict(self.weather_payload)
                    self.apply_photo_weather(index)
                    # Cycling again keeps the first live snapshot, not a photo preset.
                    self.apply_photo_weather(5 if index != 5 else 2)
                    if retire_player:
                        self.retire()
                        self.assert_released()
                    else:
                        self.photo = False
                        self.leave_photo_environment()
                    self.assertEqual((self.weather, self.driven), ("Storm", True))
                    self.assertEqual(self.weather_payload, payload)
                    self.assertIsNone(self.weather_snapshot)
                    self.assertFalse(self.photo_env)
                    self.assertEqual(self.exposure, -1.0)
                    # Repeated exit and a fresh photo session without cycling must
                    # not overwrite a later weather update from another owner.
                    self.weather = "Rain"
                    self.leave_photo_environment()
                    self.photo_env = True
                    self.leave_photo_environment()
                    self.assertEqual((self.weather, self.driven), ("Rain", True))

    def test_weather_pipeline_retains_payload_and_reenables_driven_classification(self):
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        direct = block(pipeline, "void RenderPipeline::set_weather(")
        direct = compact(re.sub(r"//[^\n]*", "", direct))
        self.assertEqual(direct, "m_weather_type = type; "
                         "m_weather_intensity = std::clamp(intensity, 0.0f, 1.0f); "
                         "m_weather_state.driven = false;")
        driven = compact(block(pipeline, "void RenderPipeline::set_weather_state("))
        self.assertIn("m_weather_state = state; m_weather_state.driven = true;", driven)
        self.assertIn("const auto classification = ClassifyWeatherState(m_weather_state); "
                      "m_weather_type = classification.type; "
                      "m_weather_intensity = classification.intensity;", driven)

    def test_save_refusals_preserve_photo_presentation(self):
        quit_body = block(self.main, 'else if (act == "quit")')
        retirement = quit_body.index("release_player_controller();" if
                                     "release_player_controller();" in quit_body else
                                     "g_playerController.reset();")
        self.assertIn("return;", block(quit_body, "if (!gameSession->SaveWorld())"))
        self.assertLess(quit_body.index("if (!gameSession->SaveWorld())"), retirement)
        self.assertRegex(quit_body[:retirement],
                         r"if \(!session_persistence_allowed\(\)\)\s+return;")
        self.assertEqual((self.exposure, self.hold, self.player), (4.0, True, True))

    def test_every_controller_retirement_uses_the_shared_release(self):
        # Includes quit, benchmark quit, immediate and staged load/create,
        # adopted-world refusal, rollback-to-menu and final shutdown.
        self.assertEqual(self.main.count("g_playerController.reset();"), 1)
        for marker in ("auto finish_world_replacement =", "auto refuse_world_entry =",
                       "auto finish_world_entry =", 'else if (act == "quit")',
                       "auto drain_entry =", "auto return_entry_to_menu =",
                       "if (saved && main_menu)"):
            with self.subTest(site=marker):
                self.assertIn("release_player_controller();", block(self.main, marker))
        replacement = block(self.main, "auto finish_world_replacement =")
        self.assertIn("release_player_controller();", block(
            replacement, "if (gameSession->WorldRevision() != entry_world_revision)"))
        shutdown = self.main[self.main.index('trace_shutdown("world_save_finished")'):]
        self.assertIn("release_player_controller();", shutdown)

    def test_rollback_and_shutdown_cannot_release_a_retired_player_twice(self):
        self.retire()
        self.retire()
        self.retire()
        self.assert_released()

    def test_no_player_leaves_unowned_renderer_settings_unchanged(self):
        self.player = False
        self.retire()
        self.assertEqual((self.exposure, self.hold, self.weather, self.releases),
                         (4.0, True, "Storm", 0))

    def test_replacement_refusal_and_adoption(self):
        replacement = block(self.main, "auto finish_world_replacement =")
        condition = re.search(r"if \((.*)\)", replacement).group(1)
        for revision, releases in ((10, 0), (11, 1)):
            with self.subTest(revision=revision):
                self.setUp()
                changed = cpp_expression(condition, **{
                    "gameSession->WorldRevision()": revision, "entry_world_revision": 10})
                if changed and "release_player_controller();" in replacement:
                    self.retire()
                if releases:
                    self.assert_released()
                else:
                    self.assertEqual((self.exposure, self.hold, self.player, self.releases),
                                     (4.0, True, True, 0))


class FrameAttributionWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = read(CLIENT / "main_client.cpp")
        cls.foliage = read(CLIENT / "rendering/passes/FoliagePass.cpp")
        cls.pass_header = read(CLIENT / "rendering/passes/FoliagePass.h")
        cls.header = read(CLIENT / "app/FrameAttribution.h")
        cls.collector = read(CLIENT / "app/FrameAttribution.cpp")

    def test_preview_restore_precedes_reload_and_gameplay_foliage(self):
        restore = self.main.index("if (worldgenPreview->active() &&")
        reload = self.main.index("const auto settle_workload_reload =")
        scatter = [match.start() for match in re.finditer(
            re.escape("UpdateFoliageScatter(g_app,"), self.main)]
        self.assertEqual(len(scatter), 3)
        preload_definition = self.main.index("auto wait_for_world_presentation =")
        activation_definition = self.main.index("auto activate_loaded_world =")
        preload_scatter = [position for position in scatter
                           if preload_definition < position < activation_definition]
        self.assertEqual(len(preload_scatter), 1)
        # The definition precedes the loop; its staged invocation follows restoration.
        advance = self.main.index("world_entry.advance(",
                                  self.main.index("if (world_entry.state() == WorldEntryState::Requested)"))
        self.assertLess(restore, advance)
        self.assertLess(restore, reload)
        for position in (position for position in scatter if position not in preload_scatter):
            self.assertLess(restore, position)
        self.assertLess(restore, self.main.index("RenderMenuScreens(g_app,"))
        self.assertIn("UpdateFoliageScatter(g_app,", block(self.main,
                      "const auto settle_workload_reload ="))
        self.assertLess(reload, self.main.index("if (!settle_workload_reload())"))

    def test_render_bucket_receives_exclusive_time_and_feeds_outputs(self):
        main = compact(self.main)
        self.assertIn("rb_render_bucket_ms = Attribution::exclusive_parent_ms("
                      "rb_render_call_ms, render_foliage_before, foliage_scope.snapshot().rebuild_ms);",
                      main)
        self.assertIn(".render = rb_render_bucket_ms", main)
        self.assertRegex(main, r"rb_stream_ms, rb_render_bucket_ms, _presentMs, _other,")
        self.assertIn("rb_render_call += rb_render_call_ms;", main)

    def test_slow_frame_keeps_far_reporting_with_exclusive_attribution(self):
        slow = compact(block(self.main, "if (_wallMs > 12.0"))
        self.assertIn("const auto slow_far = renderPipeline.far_world_reporting_state();", slow)
        self.assertIn("frame_attribution_enabled ? buckets.other(_wallMs, phase_scope.snapshot())", slow)
        self.assertRegex(slow, r"rb_stream_ms, rb_render_bucket_ms, _presentMs, _other,")
        for field in ("far_requested", "far_effective", "far_refusal", "far_suppressed",
                      "far_submission_refusal", "far_settled"):
            self.assertIn(field + "={}", slow)
        for value in ("FarWorldModeName(slow_far.selection.requested)",
                      "FarWorldModeName(slow_far.selection.effective)",
                      "FarWorldRefusalName(slow_far.selection.refusal)",
                      "FarWorldReportSuppressed(slow_far)",
                      "FarWorldSubmissionRefusalName( slow_far.submission_refusal)",
                      "slow_far.coverage_settled"):
            self.assertIn(value, slow)
        self.assertIn("foliage_frame.rebuild_ms, rb_menu_ms, rb_ui_ms,", slow)
        self.assertIn('"foliage={:.1f} menu={:.1f} ui={:.1f} | "', slow)
        self.assertIn("foliage-split:", slow)
        self.assertIn('"scratch_bytes={} backlog={} path={} fallback={} dirty_columns={} "', slow)
        self.assertIn("Attribution::fallback_name(foliage_attribution.fallback), "
                      "foliage_attribution.dirty_columns,", slow)

    def test_collection_and_split_log_require_explicit_opt_in(self):
        main = compact(self.main)
        self.assertIn('frame_csv_enabled = HasCommandLineFlag(argc, argv, "--frame-attribution-csv")', main)
        self.assertIn('frame_attribution_enabled = frame_csv_enabled || '
                      'HasCommandLineFlag(argc, argv, "--frame-attribution");', main)
        self.assertIn("FoliageFrameScope foliage_scope(frame_attribution_enabled);", main)
        self.assertIn("StageTimer menu_timer(rb_menu_ms, frame_attribution_enabled);", main)
        self.assertRegex(main, r'if \(frame_attribution_enabled\) LUMINUMBRA_CORE_WARN\(\s*"\s*foliage-split:')
        self.assertIn("explicit FoliageFrameScope(bool enabled = false);", self.header)
        self.assertIn("current_foliage_frame() = enabled ? &m_frame : nullptr;", self.collector)
        self.assertRegex(compact(self.foliage),
                         r"if \(collect_attribution\) \{ // Diagnostic[^\n]*? "
                         r"Attribution::ScatterSignature components\{\};")

    def test_reload_drains_all_gates_before_resuming_script(self):
        load = block(self.main, 'const auto settle_workload_reload =')
        for fragment in ('const Measurement::ReloadWaterPause water_pause(*world);',
                         'Measurement::ReloadSettlingWatchdog watchdog;',
                         'world->update(', 'UpdateFoliageScatter(',
                         'renderPipeline.render_frame(', 'benchmark_queries->checkpoint_passes();',
                         'glfwPollEvents();',
                         'Measurement::reload_residency_observed(reload_settling_passes,',
                         'budget.update_interval_frames)',
                         'streaming.quiescent()', 'uploads.terrain_upload_candidates == 0',
                         'uploads.water_upload_candidates == 0',
                         '(!foliage || !foliage->build_pending())',
                         'gameSession->GetRegionSchedulerFault()',
                         'watchdog.waiting(', 'watchdog.failed()'):
            self.assertIn(fragment, load)
        self.assertIn('reload_settling_pending = reload && !scenario_failed;', self.main)
        self.assertLess(self.main.index('switch (currentState)'),
                        self.main.index('if (reload_settling_pending)'))
        self.assertLess(self.main.index('if (reload_settling_pending)'),
                        self.main.index('if (currentState != GameState::WORLD_LOADING)'))
        self.assertNotIn('TickSimulation(', load)
        self.assertNotIn('glfwSwapBuffers(', load)
        self.assertNotIn('workload_frame_records.append', load)
        writer = read(CLIENT / 'app/RenderBenchmarkReport.h')
        self.assertIn('workload_observation_json', writer)
        self.assertIn('{"settling_passes", reload_settling_passes}', self.main)

    def test_all_foliage_update_callers_supply_photo_mode(self):
        calls = re.findall(r"UpdateFoliageScatter\(g_app,(.*?)\);", self.main, re.S)
        self.assertEqual(len(calls), 3)
        preload = block(self.main, 'auto wait_for_world_presentation =')
        preload_calls = re.findall(r"UpdateFoliageScatter\(g_app,(.*?)\);", preload, re.S)
        self.assertEqual(len(preload_calls), 1)
        self.assertEqual(compact(preload_calls[0]),
                         "root_dir, scenario_config, g_camera.get(), gameSession.get(), "
                         "renderPipeline, false, scatter_ms, foliage_ms")
        gameplay_calls = [call for call in calls if call != preload_calls[0]]
        self.assertEqual(len(gameplay_calls), 2)
        for call in gameplay_calls:
            self.assertEqual(compact(call),
                             "root_dir, scenario_config, g_camera.get(), gameSession.get(), "
                             "renderPipeline, g_playerController && "
                             "g_playerController->photo_mode_active(), " +
                             ("scatter_ms, foliage_ms" if "rb_scatter_ms" not in call
                              else "rb_scatter_ms, rb_foliage_ms"))

    def test_capture_simulation_decision_covers_ticks_physics_and_water(self):
        main = compact(self.main)
        self.assertIn('const bool simulate_frame = !timelapse_waiting && Measurement::frame_simulation_enabled('
                      'benchmark_v4, traversal.has_value(), measuring_v3, '
                      'g_app.hud.paused, g_app.capture.timeScale);', main)
        self.assertIn('const bool advance_frame_simulation = !timelapse_waiting && (!g_rb_active || simulate_frame);', main)
        session = block(self.main, 'if (!scenario_config.networked_session_smoke())')
        ticking = block(session, 'if (simulate_frame)')
        self.assertEqual(session.count('gameSession->TickSimulation('),
                         ticking.count('gameSession->TickSimulation('))
        self.assertEqual(ticking.count('gameSession->TickSimulation('), 4)
        physics = block(self.main, 'if (advance_frame_simulation)')
        self.assertIn('physics->update(deltaTime * g_app.capture.timeScale);', physics)
        self.assertIn('g_playerController && !benchmark_v4 && advance_frame_simulation)', main)
        self.assertIn('g_app.capture.profile_fly_seconds > 0.0 && g_playerController && g_camera && '
                      'advance_frame_simulation)', main)
        anchors = block(session, 'if (advance_frame_simulation)')
        self.assertEqual(anchors.count('SetLocalPlayerSimulationPosition('), 2)
        # The streaming call stays outside the tick gate on held warmup/menu frames.
        self.assertNotIn('GetWorldSystem()->update(', ticking)
        self.assertIn('gameSession->GetWorldSystem()->update(gameSession->GetRegistry(), '
                      'streaming_position, gameSession->GetPhysicsSystem(), '
                      'advance_frame_simulation);', compact(session))
        settling = compact(block(self.main, 'const auto settle_workload_reload ='))
        self.assertIn('world->update(gameSession->GetRegistry(), g_camera->Position, '
                      'gameSession->GetPhysicsSystem(), /*advance_simulation=*/false);', settling)
        world = read(ROOT / 'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp')
        single = block(world, 'void SHIELD_WorldSystem::update(entt::registry& registry,')
        self.assertIn('update(registry, std::vector<Vec3>{camera_position}, physics_system, '
                      'advance_simulation);', compact(single))
        multi = block(world, 'const std::vector<Vec3>& anchor_positions,')
        water = block(multi, 'if (advance_simulation && m_water_system)')
        self.assertEqual(compact(water),
                         'm_water_system->update(registry, m_streaming_state.chunks);')
        self.assertEqual(multi.count('m_water_system->update('), water.count('m_water_system->update('))
        for fragment in ('publish_completed_generation_jobs(', 'update_chunk_activation(',
                         'dispatch_deferred_meshing(', 'replace_chunk_collision('):
            self.assertIn(fragment, multi)
            self.assertNotIn(fragment, water)
        header = read(ROOT / 'src/luminumbra_common/systems/SHIELD_WorldSystem.h')
        self.assertEqual(header.count('bool advance_simulation = true'), 2)

    def test_capture_simulation_policy_handles_held_and_live_frames(self):
        source = compact(block(read(CLIENT / 'app/RenderWorkload.h'),
                               'inline bool frame_simulation_enabled('))
        rules = re.findall(r'(?:if \(([^()]*)\) )?return ([^;]+);', source)
        self.assertEqual(len(rules), 3)
        for workload, traversal in ((True, True), (False, True), (False, False)):
            for measuring in (False, True):
                for paused in (False, True):
                    for scale in (-1.0, 0.0, 0.5, 1.0, 16.0, float('nan')):
                        values = dict(workload_capture=workload, traversal_capture=traversal,
                                      measuring=measuring, paused=paused, time_scale=scale)
                        for condition, returned in rules:
                            if not condition or cpp_expression(condition, **values):
                                actual = cpp_expression(re.sub(r'(?<=\d)f\b', '', returned), **values)
                                break
                        expected = (measuring and not paused if workload else
                                    measuring if traversal else scale > 0)
                        with self.subTest(**values):
                            self.assertEqual(actual, expected)

    def test_const_scope_token_keeps_registered_observations_mutable(self):
        scope = compact(block(self.header, "class FoliageFrameScope"))
        self.assertIn("mutable FoliageFrame m_frame{};", scope)
        self.assertIn("current_foliage_frame() = enabled ? &m_frame : nullptr;", self.collector)
        self.assertIn("const Attribution::FoliageFrameScope foliage_scope", self.main)

    def test_const_scope_observations_have_mutable_storage(self):
        # Writes through the registered pointer must remain defined for const
        # scope tokens; runtime assertions alone cannot reliably expose this UB.
        scope = compact(block(self.header, "class FoliageFrameScope"))
        self.assertRegex(scope, r"\bmutable FoliageFrame m_frame\s*\{\s*\}\s*;")
        self.assertEqual(compact(block(scope, "const FoliageFrame& snapshot() const")),
                         "return m_frame;")

    def test_benchmark_does_not_enable_foliage_input_hashing(self):
        update = compact(block(self.main, "static void UpdateFoliageScatter("))
        self.assertIn("foliage->set_evidence_enabled(scenario_config.foliage_visual_smoke() && "
                      "!g_app.capture.play_paths);", update)
        self.assertNotIn("render_benchmark_path", update)

    def test_menu_input_ownership_and_exclusive_attribution_share_the_call(self):
        main = compact(self.main)
        self.assertRegex(main, r"RenderMenuScreens\([^;]*menuPreviewState, uiInputRouter\); "
                         r"menu_timer.stop\(\); rb_menu_ms = Attribution::exclusive_parent_ms\("
                         r" rb_menu_ms, menu_foliage_before, foliage_scope.snapshot\(\).rebuild_ms\);")
        menu = compact(block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens("))
        self.assertTrue(menu.startswith(
            "const bool previewMouseAllowed = previewState.UpdateInputOwnership(inputRouter);"))

    def test_cached_menu_retains_input_ownership_and_scene_state(self):
        header = read(CLIENT / "app/MenuScreens.h")
        state = block(header, "struct MenuSceneState : MenuPreviewState")
        self.assertIn("MenuSceneSchedule schedule;", state)
        self.assertIn("MenuSceneImage scene_image;", state)
        self.assertIn("MenuSceneState& previewState,", header)
        self.assertIn("const UiInputRouter& inputRouter", header)
        self.assertIn("MenuSceneState menuPreviewState;", self.main)
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        self.assertLess(menu.index("UpdateInputOwnership(inputRouter)"),
                        menu.index("schedule.update(now, key, interactive)"))
        self.assertIn("previewMouseAllowed &&", menu)
        self.assertIn("interactive = true;", block(menu, "if (previewState.dragging)"))

    def test_poll_attribution_keeps_ownership_refresh_and_registered_overlays(self):
        start = self.main.index("const auto _rb_poll_t0")
        poll_end = self.main.index("glfwPollEvents();", start)
        poll = self.main[start:poll_end]
        self.assertIn("poll_phases_before = Attribution::phase_total_ms();", poll)
        self.assertIn("poll_foliage_before = foliage_scope.snapshot().rebuild_ms;", poll)
        self.assertIn("RefreshInputOwnership(window);", poll)
        after_poll = compact(self.main[poll_end:])
        self.assertTrue(after_poll.startswith("glfwPollEvents(); RefreshInputOwnership(window);"))
        self.assertIn("developerConsole.Draw(developerFrame);", self.main)
        self.assertNotIn("DrawFrameStatusOverlays(", self.main)
        self.assertNotIn("DrawGpuProfilerOverlay(", self.main)
        self.assertRegex(compact(self.main),
                         r"menuPreviewState.schedule = \{\}; "
                         r"menuPreviewState.last_menu_frame = \{\}; "
                         r"//[^\n]*? if \(g_app.capture.timelapse_creatures\) "
                         r"DrawCreatureNameplates\(")

    def test_timelapse_day_drift_overrides_live_clock_by_frame_order(self):
        # Writer order: live clock, time-lapse drift, then scenario pins, so the drift beats the
        # live clock while a scenario's fixed time of day still beats the drift.
        main = self.main
        live = main.index("renderPipeline.set_time_of_day_hold(false);")
        start = main.index("if (g_app.capture.timelapse_frames > 0 &&\n", live)
        drift = compact(block(main[start:], "if (g_app.capture.timelapse_frames > 0 &&"))
        self.assertEqual(drift, "renderPipeline.set_time_of_day(g_app.capture.timelapse_tod);")
        self.assertIn("g_app.capture.timelapse_daystep > 0.0f",
                      main[start:main.index("renderPipeline.set_time_of_day(g_app.capture"
                                            ".timelapse_tod);", start)])
        pins = main.index("scenario_runner->onPreRenderCapturePins();", live)
        self.assertLess(start, pins)

    def test_capture_day_phase_uses_calendar_without_frame_drift(self):
        update = block(read(CLIENT / "rendering/RenderPipeline.cpp"),
                       "void RenderPipeline::update_time_of_day(")
        advance = compact(block(update, "if (!m_timeOfDayHold && !m_todTickDriven)"))
        self.assertIn("m_dayLengthOverride ? m_dayLengthTicks : "
                      "m_renderClock.calendar().dayLengthTicks", advance)
        self.assertRegex(advance, r"if \(const auto& seconds = "
                         r"Client::presentation_clock.capture_seconds\(\)\) "
                         r"m_elapsedDayPhase = std::fmod\(\*seconds / m_dayDurationSeconds, 1.0\); "
                         r"else m_elapsedDayPhase = std::fmod\(m_elapsedDayPhase \+ "
                         r"deltaTime / m_dayDurationSeconds, 1.0\);")
        self.assertIn("m_timeOfDay = Rendering::SolarTimeFromNoonElapsed("
                      "m_elapsedDayPhase, m_renderClock);", advance)
        self.assertIn("m_todTickDriven = false;", update)

    def test_disabled_timers_do_not_read_clocks(self):
        constructor = compact(block(self.header, "explicit StageTimer("))
        self.assertEqual(constructor, "if (m_running) m_start = std::chrono::steady_clock::now();")
        self.assertIn("m_running(enabled)", self.header)
        self.assertIn("time_point m_start{};", self.header)
        timer = block(self.header, "class StageTimer")
        self.assertEqual(timer.count("steady_clock::now()"), 2)
        self.assertTrue(compact(block(timer, "void stop()")).startswith("if (m_running) {"))
        scope = block(self.foliage, "class RebuildAttributionScope")
        self.assertIn("if (m_enabled) m_start = std::chrono::steady_clock::now();", compact(scope))
        self.assertTrue(compact(block(scope, "~RebuildAttributionScope()")).startswith(
            "if (!m_enabled) return;"))
        self.assertEqual(self.foliage.count("steady_clock::now()"), 2)
        self.assertNotRegex(self.foliage, r"Attribution::StageTimer\b")
        timers = re.findall(r"Attribution::FoliageStageTimer \w+\((.*?)\);", self.foliage, re.S)
        self.assertGreater(len(timers), 10)
        for arguments in timers:
            self.assertTrue(compact(arguments).startswith("m_rebuild_attribution, "))
            self.assertTrue(compact(arguments).endswith(", collect_attribution"))
        wrapper = compact(block(self.collector, "FoliageStageTimer::FoliageStageTimer("))
        self.assertEqual(wrapper, "")
        for guarded in ("m_stages_before(enabled ? rebuild.sum() : 0)",
                        "m_phases_before(enabled ? phase_total_ms() : 0)",
                        "m_timer(m_elapsed, enabled)"):
            self.assertIn(guarded, compact(self.collector))
        stop = compact(block(self.collector, "void FoliageStageTimer::stop()"))
        self.assertTrue(stop.startswith("if (!m_running) return; m_timer.stop();"))
        self.assertIn("m_ms += m_elapsed - (m_rebuild.sum() - m_stages_before) - "
                      "(phase_total_ms() - m_phases_before);", stop)
        self.assertTrue(stop.endswith("m_running = false;"))
        self.assertNotIn("steady_clock::now()", block(self.header, "class FoliageStageTimer"))

    def test_foliage_remainder_and_cpu_repacking_are_reported(self):
        rebuild = compact(block(self.foliage, "void FoliagePass::rebuild_instances("))
        self.assertIn("FoliageStageTimer repack_timer( m_rebuild_attribution, "
                      "m_rebuild_attribution.repack_ms, collect_attribution);", rebuild)
        self.assertIn("return total_ms - sum();", block(self.header, "struct FoliageRebuild"))
        self.assertIn("foliage_attribution.other(foliage_frame.rebuild_ms)", self.main)
        native = read(ROOT / "test/rendering/foliage_rebuild_test.cpp")
        counters = block(native, "TEST_F(FoliageRebuildTest, "
                         "CpuCountersDescribeInitialReusedAndCameraRebuilds)")
        self.assertIn("pass.rebuild_attribution()", counters)
        for field in ("admitted_columns", "reasons", "uploaded_bytes", "backlog", "path"):
            self.assertIn("work." + field, counters)
        self.assertIn("CheckWork(", counters)
        self.assertNotIn("_ms", counters)

    def test_water_subzones_share_the_opt_in_frame_and_csv(self):
        main = compact(self.main)
        self.assertIn("WaterAttribution::FrameScope water_scope(frame_attribution_enabled);", main)
        self.assertIn("foliage_frame.uploads, water_scope.snapshot())", main)
        self.assertRegex(main, r'const auto& water = water_scope.snapshot\(\); '
                              r'LUMINUMBRA_CORE_WARN\( " water-split:')
        for column in ("water_seed_inputs_ms", "water_initialization_ms", "water_sim_ms",
                       "water_seam_ms", "water_bookkeeping_ms", "water_fallback_columns"):
            self.assertIn(column, self.collector)

    def test_const_frame_scopes_allow_callback_collection(self):
        scope = block(self.header, "class FoliageFrameScope")
        self.assertIn("mutable FoliageFrame m_frame{};", scope)
        runtime = read(ROOT / "test/rendering/graphics_runtime_test.cpp")
        self.assertIn("const Attribution::FoliageFrameScope frame(true);", runtime)
        self.assertIn("return frame.snapshot().rebuild.reasons;", runtime)

    def test_both_frozen_entry_points_are_counted_once(self):
        rebuild = block(self.foliage, "void FoliagePass::rebuild_instances(")
        qualification = block(self.foliage, "bool FoliagePass::begin_qualification()")
        frozen = block(self.foliage, "void FoliagePass::rebuild_frozen_inputs()")
        for caller in (rebuild, qualification):
            self.assertEqual(caller.count("RebuildAttributionScope rebuild_scope(*this);"), 1)
            self.assertEqual(caller.count("rebuild_frozen_inputs();"), 1)
        self.assertNotIn("RebuildAttributionScope", frozen)
        self.assertNotIn("m_rebuild_attribution.dispatch_ms", rebuild)
        self.assertIn("m_rebuild_attribution = {};", qualification)
        self.assertIn("m_rebuild_attribution.reasons = Attribution::Initial;", qualification)
        self.assertIn("m_rebuild_attribution.path = Attribution::FoliagePath::FrozenGpu;", frozen)
        timer_position = frozen.index("Attribution::FoliageStageTimer timer(")
        for operation in ("glClearBufferData(", "dispatch_scatter();", "submit_foliage_readback();"):
            self.assertGreater(frozen.index(operation), timer_position)
        self.assertNotIn("timer.stop()", frozen)

    def test_foliage_cached_uniforms_retain_attributed_program_lifetimes(self):
        init = block(self.foliage, "void FoliagePass::init_compute(")
        destroy = block(self.foliage, "void FoliagePass::destroy_compute()")
        upload = block(self.foliage, "bool FoliagePass::rebuild_instances_gpu(")
        dispatch = block(self.foliage, "void FoliagePass::dispatch_scatter()")
        for program in ("m_compute_prog", "m_prefix_prog"):
            self.assertIn(f"AttributedGl::Program {program}{{}};", self.pass_header)
            self.assertIn(f"AttributedGl::DeleteProgram(glDeleteProgram, {program})", destroy)
            self.assertIn(f"{program} = {{}};", destroy)
        for cache, fields in (
            ("m_scatter_uniforms", ("chunk_count", "archetype_count", "total_weight",
                                     "max_candidates", "max_instances", "density_scale",
                                     "camera_xz", "fade_end_m", "wind_xz", "emit")),
            ("m_prefix_uniforms", ("group_count", "max_instances")),
        ):
            self.assertIn(f"{cache} = {{", init)
            self.assertIn(f"{cache} = {{}};", destroy)
            for field in fields:
                self.assertIn(f".{field} = glGetUniformLocation(", init)
                self.assertIn(f"{cache}.{field}", upload + dispatch)
        self.assertNotIn("glGetUniformLocation(", upload + dispatch)
        self.assertEqual(dispatch.count("AttributedGl::UseProgram("), 4)
        self.assertEqual(dispatch.count("AttributedGl::Submit("), 3)
        self.assertNotRegex(dispatch, r"\bgl(?:UseProgram|DispatchCompute)\s*\(")
        self.assertIn("m_uploaded_input_hash.reset();", destroy)
        self.assertIn("m_uploaded_chunk_count = 0;", destroy)
        self.assertIn("m_group_storage_bytes = 0;", destroy)

    def test_client_uses_only_the_opt_in_profiler_interface(self):
        targets = read(ROOT / "src/CMakeLists.txt")
        links = re.search(r"target_link_libraries\(luminumbra_client\s+(.*?)\)", targets, re.S)
        self.assertIsNotNone(links)
        self.assertIn("luminumbra_profiler", links.group(1))
        self.assertNotIn("TracyClient", links.group(1))
        tracy = read(ROOT / "cmake/tracy.cmake")
        self.assertRegex(tracy, r'option\(LUMINUMBRA_ENABLE_TRACY\s+"[^"\n]*" OFF\)')
        before, gated = tracy.split("if(LUMINUMBRA_ENABLE_TRACY)", 1)
        enabled = gated.split("else()", 1)[0]
        self.assertIn("FetchContent_MakeAvailable(tracy)", enabled)
        self.assertIn("target_link_libraries(luminumbra_profiler INTERFACE TracyClient)", enabled)
        self.assertNotIn("FetchContent", before.split("option(", 1)[1])

    def test_public_counts_use_readback_values_and_publish_unknown(self):
        for accessor, result in (("bool frame_instance_count_known()", ".has_value()"),
                                 ("std::size_t frame_instance_count()", ".value_or(0)")):
            body = compact(block(self.pass_header, accessor))
            self.assertIn("generated_instance_count( m_gpu_active, m_readback_enabled, "
                          "m_gpu_generated_count.has_value(), m_gpu_active ? "
                          "m_gpu_generated_count.value_or(0) : m_draw_instance_limit)", body)
            self.assertTrue(body.endswith(result + ";"))
        readback = block(self.foliage, "void FoliagePass::poll_foliage_readback()")
        self.assertIn("m_gpu_generated_count = count;", readback)
        self.assertIn("m_gpu_count_generation = generation;", readback)
        gpu = block(self.foliage, "bool FoliagePass::rebuild_instances_gpu(")
        self.assertIn("m_gpu_generated_count.reset();", gpu)
        scan = compact(read(CLIENT / "rendering/FrameScan.cpp"))
        self.assertIn("fol->frame_instance_count_known()", scan)
        self.assertIn('foliage["instances"] = report.foliage_instance_count_known ? '
                      'nlohmann::json(report.foliage_instances) : nlohmann::json(nullptr);', scan)
        pipeline = compact(block(read(CLIENT / "rendering/RenderPipeline.cpp"),
                                 "void RenderPipeline::execute_stage_foliage("))
        self.assertIn("!m_foliage_pass->frame_instance_count_known() && "
                      "m_foliage_pass->last_draw_calls() > 0", pipeline)
        self.assertIn("m_last_render_pass_stats.foliage_instance_count_known = "
                      "m_foliage_pass->frame_instance_count_known();", pipeline)
        overlay = compact(block(read(CLIENT / "app/DebugOverlays.cpp"),
                                "void DrawGpuProfilerContents("))
        self.assertNotIn("ImGui::Begin(", overlay)
        self.assertNotIn("ImGui::End(", overlay)
        self.assertIn('if (gp.foliage_instance_count_known) ImGui::Text(', overlay)
        self.assertIn('capacity (count unavailable)', overlay)
        main = compact(self.main)
        self.assertIn("foliage_frame.capacity, foliage_frame.count_known, "
                      "foliage_frame.generated_count, foliage_frame.count_generation, phase_scope.snapshot(), foliage_frame.uploads, water_scope.snapshot())", main)
        scope = compact(block(self.foliage, "~RebuildAttributionScope()"))
        self.assertIn("m_pass.frame_instance_count_known() ? "
                      "std::optional<std::size_t>(m_pass.frame_instance_count()) : std::nullopt", scope)

    def test_menu_phase_collection_removes_nested_work_from_parent_buckets(self):
        main = compact(self.main)
        self.assertIn("PhaseFrameScope phase_scope(frame_attribution_enabled);", main)
        for parent in ("menu", "render", "poll", "ui"):
            self.assertIn(f"const double {parent}_phases_before = Attribution::phase_total_ms();", main)
            self.assertIn(f"Attribution::phase_total_ms() - {parent}_phases_before", main)
        self.assertIn("buckets.other(_wallMs, phase_scope.snapshot())", main)
        self.assertIn("buckets.other(frame_ms, phases)", compact(self.collector))
        self.assertIn("count_generation, phases, uploads, water)",
                      compact(block(self.collector, "bool FrameRecorder::record(")))
        self.assertIn("Phase::WorldCreateCallback", block(self.main, "auto start_world_creation ="))
        self.assertIn("Phase::WorldLoadCallback", block(self.main, "auto start_world_load ="))
        ui = read(CLIENT / "ui/Rml_UIManager.cpp")
        for method, phase in (("Update()", "UiUpdate"), ("Render()", "UiRender"),
                              ("LoadDocument(", "DocumentLoad")):
            self.assertIn(f"Phase::{phase}", block(ui, f"void Rml_UIManager::{method}"))
        for path, marker in (("ui/Rml_Interfaces.cpp", "RmlRenderer::LoadTexture("),
                             ("ui/gl3/RmlUi_Renderer_GL3.cpp", "RenderInterface_GL3::LoadTexture("),
                             ("ui/Rml_Interfaces.cpp", "RmlRenderer::GenerateTexture("),
                             ("ui/gl3/RmlUi_Renderer_GL3.cpp", "RenderInterface_GL3::GenerateTexture(")):
            self.assertIn("Phase::TextureLoad", block(read(CLIENT / path), marker))
        preview = read(CLIENT / "world/WorldgenPreview.cpp")
        self.assertIn("Phase::PreviewAdoption", block(preview, "void WorldgenPreview::swap_pending_into_live("))
        for marker in ("bool WorldgenPreview::render(", "bool WorldgenPreview::render_to_backbuffer("):
            self.assertIn("Phase::MenuScene", block(preview, marker))
        backend = read(CLIENT / "ui/gl3/RmlUi_Renderer_GL3.cpp")
        self.assertIn("Phase::UiFilters", block(backend, "void RenderInterface_GL3::RenderFilters("))
        self.assertIn("Phase::ShaderLoad", block(backend, "static bool CreateShaders("))
        self.assertIn("AttributedGl::UseProgram(glUseProgram,",
                      block(backend, "void RenderInterface_GL3::UseProgram("))
        for path, marker in (("rendering/ImpostorBake.cpp", "AttributedGl::Program CompileBakeProgram("),
                             ("rendering/SkyAtmosphereLut.ipp", "AttributedGl::Program compile_compute("),
                             ("rendering/passes/LuminanceMeterPass.cpp", "AttributedGl::Program create_compute_program("),
                             ("rendering/passes/FoliagePass.cpp", "AttributedGl::Program compile_compute_program(")):
            self.assertIn("Phase::ShaderLoad", block(read(CLIENT / path), marker))
        legacy = read(CLIENT / "ui/Rml_Interfaces.cpp")
        self.assertIn("Phase::ShaderLoad", block(legacy, "RmlRenderer::RmlRenderer()"))
        self.assertIn("AttributedGl::UseProgram(glUseProgram,",
                      block(legacy, "void RmlRenderer::RenderGeometry("))
        self.assertRegex(block(legacy, "void RmlRenderer::RenderGeometry("),
                         r"AttributedGl::Submit\(\s*glDrawElements,")
        shader = read(CLIENT / "rendering/Shader.cpp")
        self.assertIn("Phase::ShaderLoad", block(shader, "AttributedGl::Program Shader::buildProgram("))
        self.assertIn("AttributedGl::UseProgram(glUseProgram,", block(shader, "void Shader::use()"))

    def test_all_program_lifetimes_and_submissions_use_first_use_attribution(self):
        # Enumerate the rendering and both UI backends, including state restores,
        # platform branches, compute kernels, indirect and instanced submissions.
        call = re.compile(r"\bgl(?:CreateProgram|DeleteProgram|UseProgram|"
                          r"Draw(?:Arrays|Elements)\w*|MultiDraw\w*|DispatchCompute\w*)\s*\(")
        count = 0
        for tree in (CLIENT / "rendering", CLIENT / "ui"):
            for path in sorted(tree.rglob("*")):
                if path.suffix not in (".cpp", ".h", ".ipp"):
                    continue
                source = read(path)
                code = re.sub(
                    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    " ", source, flags=re.S)
                self.assertIsNone(call.search(code), str(path.relative_to(ROOT)))
                count += code.count("AttributedGl::Submit(")
        self.assertGreater(count, 40)
        helper = read(CLIENT / "app/ProgramFirstUse.h")
        submit = compact(block(helper, "void submit("))
        self.assertTrue(submit.startswith("if (m_bound->pending) {"))
        self.assertNotIn("PhaseScope", submit)
        self.assertNotIn("lookup(", submit)
        self.assertLess(submit.index("m_bound->pending = false;"),
                        submit.index("if (shader_first_use_collection_enabled)"))
        for method, invocation in (("use_attributed(", "function(program);"),
                                   ("submit_attributed(", "function(std::forward<Args>(args)...);")):
            cold = compact(block(helper, "void " + method))
            self.assertLess(cold.index("++m_attributed_calls;"), cold.index("const PhaseScope"))
            self.assertLess(cold.index("const PhaseScope phase(Phase::ShaderFirstUse);"),
                            cold.index(invocation))
            self.assertIn("LUMINUMBRA_FIRST_USE_COLD void " + method, helper)
            self.assertNotIn("phase.stop", cold)
        use = compact(block(helper, "void use("))
        self.assertIn("if (static_cast<unsigned>(shader_first_use_collection_enabled) & "
                      "static_cast<unsigned>(m_bound->pending))", use)
        self.assertNotIn("PhaseScope", use)
        self.assertNotIn("lookup(", use)
        self.assertEqual(compact(block(helper, "void bind(const Program&")),
                         "m_bound = program.state;")
        self.assertIn("__declspec(noinline)", helper)
        self.assertIn("__attribute__((cold, noinline))", helper)
        gl = read(CLIENT / "rendering/AttributedGl.h")
        self.assertIn("return programs.created(function());", block(gl, "CreateProgram("))
        self.assertIn("programs.deleted(program);", block(gl, "DeleteProgram("))
        self.assertIn("programs.use(function, program);", block(gl, "UseProgram("))
        self.assertIn("programs.submit(function, std::forward<Args>(args)...);",
                      block(gl, "Submit("))
        self.assertNotIn("thread_local", gl)
        self.assertNotIn("PhaseScope", gl)
        self.assertIn("inline constinit bool shader_first_use_collection_enabled = false;",
                      self.header)
        for method in ("PhaseFrameScope::PhaseFrameScope(", "PhaseFrameScope::~PhaseFrameScope()"):
            self.assertIn("shader_first_use_collection_enabled = current_phases() != nullptr;",
                          block(self.collector, method))
        preview = read(ROOT / "tools/static_preview/CMakeLists.txt")
        self.assertIn('"${STATIC_PREVIEW_ROOT}/src/luminumbra_client/app/FrameAttribution.cpp"',
                      preview)

    def test_static_preview_compiles_the_diagnostic_id_codec_its_units_call(self):
        # The standalone preview DLL links no client library, so every unit it compiles that
        # submits diagnostic draws (DiagnosticDrawTable::submit -> EncodeDiagnosticId) needs the
        # codec unit in the same target; otherwise MSVC/MSYS2 fail to link the DLL.
        preview = read(ROOT / "tools/static_preview/CMakeLists.txt")
        target = re.search(r"add_library\(luminumbra_render_static SHARED(.*?)\)", preview, re.S)
        self.assertIsNotNone(target)
        sources = re.findall(r'"\$\{STATIC_PREVIEW_ROOT\}/(src/[^"]+\.cpp)"', target.group(1))
        callers = [source for source in sources
                   if re.search(r'#include "(?:rendering/)?DiagnosticDrawTable\.h"|EncodeDiagnosticId\(',
                                read(ROOT / source))]
        self.assertIn("src/luminumbra_client/rendering/GBufferTargets.cpp", callers)
        self.assertIn("src/luminumbra_client/rendering/DiagnosticCapture.cpp", sources)

    def test_program_owners_retain_the_first_use_bit_on_every_backend(self):
        for path, members in (
            ("rendering/Shader.h", ("m_id",)),
            ("rendering/passes/FoliagePass.h", ("m_compute_prog", "m_prefix_prog")),
            ("rendering/passes/FroxelPass.h", ("m_inject_program", "m_integrate_program")),
            ("rendering/passes/LuminanceMeterPass.h", ("m_reduce_program",)),
            ("rendering/SkyAtmosphereLut.h", ("m_skyview_compute_prog",
                                           "m_transmittance_compute_prog",
                                           "m_multiscatter_compute_prog")),
            ("ui/Rml_Interfaces.h", ("m_program",)),
        ):
            source = read(CLIENT / path)
            for member in members:
                self.assertIn(f"AttributedGl::Program {member}{{}};", source, path)
        gl3 = read(CLIENT / "ui/gl3/RmlUi_Renderer_GL3.cpp")
        self.assertIn("EnumArray<Luminumbra::Rendering::AttributedGl::Program, ProgramId>", gl3)
        self.assertIn("const AttributedGl::Program& prog",
                      read(CLIENT / "rendering/SkyAtmosphereLut.ipp"))
        # Helpers must not erase the retained bit by converting creation to GLuint.
        for path in CLIENT.rglob("*"):
            if path.suffix in (".cpp", ".h", ".ipp"):
                self.assertNotRegex(read(path),
                                    r"\b(?:GLuint|u32|std::uint32_t)\s+\w+\s*=\s*"
                                    r"(?:Luminumbra::Rendering::)?AttributedGl::CreateProgram\(",
                                    str(path.relative_to(ROOT)))

    def test_nested_phase_scopes_exclude_children_and_foliage(self):
        constructor = compact(block(self.collector, "PhaseScope::PhaseScope("))
        self.assertTrue(constructor.startswith("if (!enabled || !current_phases()) return;"))
        destructor = compact(block(self.collector, "PhaseScope::~PhaseScope()"))
        self.assertTrue(destructor.startswith("if (!m_totals) return;"))
        self.assertIn("m_totals->add(m_phase, elapsed, m_children_ms, foliage - m_child_foliage_ms)",
                      destructor)
        self.assertIn("m_parent->m_children_ms += elapsed;", destructor)
        self.assertIn("m_parent->m_child_foliage_ms += foliage;", destructor)
        rebuild = block(self.foliage, "~RebuildAttributionScope()")
        self.assertIn("Attribution::phase_total_ms() - m_phases_before", rebuild)

    def test_collectors_borrow_mutable_storage_through_thread_local_accessors(self):
        # Check the live accessors and every consumer, so an unused TLS helper
        # cannot hide a process-global pointer or a stale restoration path.
        for signature, declaration, result in (
            ("FoliageFrame*& current_foliage_frame()",
             "thread_local FoliageFrame* frame = nullptr;", "frame"),
            ("PhaseTotals*& current_phases()",
             "thread_local PhaseTotals* totals = nullptr;", "totals"),
            ("const PhaseScope*& current_phase_scope()",
             "thread_local const PhaseScope* scope = nullptr;", "scope"),
        ):
            self.assertIn(signature, self.collector)
            self.assertEqual(compact(block(self.collector, signature)),
                             f"{declaration} return {result};")
        self.assertEqual(len(re.findall(r"\bthread_local\b", self.collector)), 3)
        self.assertNotRegex(self.collector,
                            r"\b(?:current_phases|current_phase_scope)\b(?!\s*\()")
        for owner, storage in (("FoliageFrameScope", "FoliageFrame m_frame{};"),
                               ("PhaseFrameScope", "PhaseTotals m_totals{};")):
            self.assertIn("mutable " + storage, block(self.header, "class " + owner))
        self.assertNotIn("const_cast", self.collector + self.header)
        for marker, fragments in (
            ("PhaseFrameScope::PhaseFrameScope(",
             ("current_phases() = enabled ? &m_totals : nullptr;",)),
            ("PhaseFrameScope::~PhaseFrameScope()",
             ("current_phases() = m_previous;",)),
            ("bool PhaseFrameScope::is_current() const",
             ("return current_phases() == &m_totals;",)),
            ("double phase_total_ms()",
             ("const auto* totals = current_phases();", "return totals ? totals->sum() : 0;")),
            ("PhaseScope::PhaseScope(",
             ("m_totals = current_phases();", "m_parent = current_phase_scope();",
              "current_phase_scope() = this;")),
            ("PhaseScope::~PhaseScope()",
             ("m_parent->m_totals == m_totals", "current_phase_scope() = m_parent;")),
            ("bool PhaseScope::is_current() const",
             ("m_totals == current_phases() && current_phase_scope() == this",)),
        ):
            body = compact(block(self.collector, marker))
            for fragment in fragments:
                self.assertIn(fragment, body)
        self.assertIn(": m_previous(current_phases())", self.collector)

    def test_menu_cache_captures_before_ui_and_bypasses_render_on_reuse(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        self.assertLess(menu.index("copy_scene_image(true)"), menu.index("uiManager->Render()"))
        self.assertLess(menu.index("copy_scene_image(false)"), menu.index("uiManager->Render()"))
        refresh = block(menu, "if (drawable && (refresh || !has_image))")
        self.assertIn("render_to_backbuffer(renderPipeline, render_delta)", refresh)
        self.assertIn("renderPipeline.render_frame(", refresh)
        self.assertIn("schedule.rendered()", refresh)
        reuse = block(menu, "else if (previewState.schedule.valid())")
        self.assertEqual(compact(reuse), "previewState.scene_image.copy_scene_image(false);")
        self.assertIn("document_generation()", menu)
        self.assertIn("worldgenPreview->rebuild_generation()", menu)
        self.assertIn(".renderer_target = renderPipeline.resize_generation()", compact(menu))
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        setter = block(pipeline, "void RenderPipeline::set_render_scale(")
        self.assertIn("resize_graphics_targets(scale, m_shadow_pass->shadow_map().resolution)", setter)
        resize = block(pipeline, "bool RenderPipeline::resize_graphics_targets(")
        self.assertIn("return ApplySceneRenderScale(", resize)
        self.assertLess(resize.index("if (!m_render_registry.replace_targets("),
                        resize.index("++m_resize_generation;"))
        for method in ("void RenderPipeline::on_resize(", "bool RenderPipeline::resize_graphics_targets("):
            self.assertIn("++m_resize_generation;", block(pipeline, method))
        for getter in ("render_scale", "get_exposure_override", "get_cloud_quality",
                       "get_ssao_quality", "volumetric_quality", "taau_enabled",
                       "sky_lut_gpu_enabled", "auto_exposure_metered", "graphics_runtime_config"):
            self.assertIn(f"renderPipeline.{getter}()", menu)
        self.assertLess(menu.index("camera_revision.observe(camera_state())"),
                        menu.index("camera->Yaw = 95.0f"))
        self.assertGreater(menu.index("camera_revision.animated(camera_state())"),
                           menu.index("camera->updateCameraVectors()"))
        self.assertIn("schedule.update(now, key, interactive)", menu)
        self.assertIn("menu_backdrop_yaw += menu_delta", menu)
        self.assertIn("worldgenPreview->tick(menu_delta)", menu)
        self.assertIn("menuPreviewState.schedule = {};", self.main)
        self.assertLess(self.main.index("menuPreviewState.scene_image.release_scene_image();"),
                        self.main.index("glfwDestroyWindow(window);", self.main.index("menuPreviewState.scene_image.release_scene_image();")))

    def test_capture_samples_require_success_and_refusals_replay_without_progress(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        scene = block(menu, "if (scenic || previewActive)")
        refresh = block(scene, "if (drawable && (refresh || !has_image))")
        self.assertIn("bool scene_rendered = false;", menu[:menu.index("if (scenic || previewActive)")])
        self.assertEqual(menu.count("scene_rendered = true;"), 1)
        self.assertEqual(menu.count("schedule.rendered();"), 1)
        self.assertIn("bool rendered = false;", refresh)
        preview = compact(block(refresh, "if (previewActive)"))
        self.assertEqual(preview,
                         "rendered = worldgenPreview->render_to_backbuffer(renderPipeline, render_delta);")
        scenic = compact(block(refresh, "else"))
        self.assertTrue(scenic.endswith("rendered = true;"))
        self.assertLess(scenic.index("renderPipeline.render_frame("), scenic.index("rendered = true;"))
        success = compact(block(refresh, "if (rendered)"))
        self.assertTrue(success.startswith("scene_rendered = true;"))
        self.assertIn("previewState.schedule.rendered();", success)
        refused = compact(block(refresh, "else if (has_image && previewState.schedule.valid())"))
        self.assertEqual(refused, "previewState.scene_image.copy_scene_image(false);")
        self.assertLess(menu.index("uiManager->Render();"), menu.index("bool ready_to_capture = false;"))

    def test_capture_settling_evaluates_live_throttle_and_admission_predicates(self):
        # Evaluate the shipping scheduler and counter guards, not a separately
        # hardcoded cadence. Binding checks above keep render success outside replay.
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        regular = block(menu, "else if (app.capture.ui_screenshot_settle < 30)")
        regular = re.sub(r"//[^\n]*", "", regular)
        admission = re.fullmatch(r"\s*if \((.*?)\)\s*"
                                r"\+\+app.capture.ui_screenshot_settle;\s*", regular, re.S)
        self.assertIsNotNone(admission)
        predicate = admission.group(1)
        for scenic, preview, rendered in ((False, False, False), (True, False, False),
                                         (False, True, False), (True, False, True),
                                         (False, True, True)):
            self.assertEqual(cpp_expression(predicate, scene_rendered=rendered,
                                            scenic=scenic, previewActive=preview),
                             rendered or not (scenic or preview))
        schedule = read(CLIENT / "app/MenuSceneSchedule.h")
        update = block(schedule, "bool update(")
        render_delta = re.sub(r"//[^\n]*", "", block(schedule, "float render_delta_seconds() const"))
        self.assertEqual(compact(render_delta),
                         "return std::min(std::chrono::duration<float>(m_now - m_last_render).count(), 1.0f / 20.0f);")
        self.assertIn("m_last_render = m_now;", block(schedule, "void rendered()"))
        self.assertIn("m_now = now;", update)
        slot_expression = re.search(r"const auto slot = (.*?);", update, re.S).group(1)
        refresh_expression = re.search(r"return (.*?);", update, re.S).group(1)
        second = int(re.search(r"kSecond = ([\d']+);", schedule).group(1).replace("'", ""))
        last_slot, valid, samples, replayed = -1, False, 0, 0
        for tick in range(1001):  # 1000 Hz UI over exactly one elapsed second
            slot = cpp_expression(slot_expression, elapsed=tick * 1_000_000, kSecond=second)
            refresh = cpp_expression(refresh_expression, **{
                "key.width": 1920, "key.height": 1080, "m_valid": valid,
                "due": slot != last_slot, "interactive": False})
            last_slot = slot
            if refresh:
                valid = True
            else:
                replayed += 1
            samples += bool(cpp_expression(predicate, scene_rendered=refresh,
                                           scenic=True, previewActive=False))
            if tick == 30:
                self.assertEqual(samples, 1)  # 31 UI frames cannot settle a scene
            if tick == 966:
                self.assertEqual(samples, 29)
        self.assertEqual((samples, replayed), (31, 970))
        for width, height, interactive, expected in ((0, 1080, True, False),
                                                      (1920, 0, True, False),
                                                      (1920, 1080, True, True),
                                                      (1920, 1080, False, False)):
            self.assertEqual(cpp_expression(refresh_expression, **{
                "key.width": width, "key.height": height, "m_valid": True,
                "due": False, "interactive": interactive}), expected)

    def test_render_delta_matches_shipping_ceiling_and_preserves_elapsed_consumers(self):
        schedule = read(CLIENT / "app/MenuSceneSchedule.h")
        body = re.sub(r"//[^\n]*", "", block(schedule, "float render_delta_seconds() const"))
        bounded = re.fullmatch(
            r"\s*return std::min\(std::chrono::duration<float>\(m_now - m_last_render\)"
            r"\.count\(\), ([\d.]+)f / ([\d.]+)f\);\s*", body)
        self.assertIsNotNone(bounded)
        numerator, denominator = map(float, bounded.groups())
        ceiling = numerator / denominator
        shipping = re.search(
            r"deltaTime = benchmark_v4 \? ([\d.]+)f / ([\d.]+)f : "
            r"std::min\(deltaTime, ([\d.]+)f / ([\d.]+)f\);", self.main)
        self.assertIsNotNone(shipping)
        capture_delta = float(shipping[1]) / float(shipping[2])
        self.assertEqual(capture_delta, 1.0 / 30.0)
        self.assertEqual(ceiling, float(shipping[3]) / float(shipping[4]))
        self.assertEqual(ceiling, 0.05)
        # Evaluate the scalar operation extracted above, without native execution.
        for elapsed, expected in ((0, 0), (0.001, 0.001), (0.033, 0.033),
                                  (0.049, 0.049), (0.05, 0.05), (0.051, 0.05),
                                  (0.730, 0.05), (60, 0.05), (99.9, 0.05),
                                  (-0.001, -0.001)):
            with self.subTest(elapsed=elapsed):
                self.assertEqual(min(elapsed, ceiling), expected)
        self.assertEqual(compact(block(schedule, "double elapsed_seconds() const")),
                         "return std::chrono::duration<double>(m_now - m_epoch).count();")
        self.assertEqual(compact(block(schedule, "void invalidate()")), "m_valid = false;")
        self.assertEqual(compact(block(schedule, "void rendered()")),
                         "m_valid = true; m_last_render = m_now;")
        update = block(schedule, "bool update(")
        self.assertEqual(compact(block(update, "if (!m_started)")),
                         "m_started = true; m_epoch = now; m_last_render = now;")
        self.assertEqual(update.count("m_last_render ="), 1)
        self.assertEqual(update.count("m_epoch ="), 1)
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        self.assertIn("std::chrono::duration<float>(menu_now - previewState.last_menu_frame).count()", menu)
        self.assertIn("app.menu.menu_backdrop_yaw += menu_delta;", menu)
        self.assertIn("worldgenPreview->tick(menu_delta);", menu)
        self.assertIn("std::chrono::steady_clock::now()", menu)
        refresh = block(menu, "if (drawable && (refresh || !has_image))")
        self.assertIn("const float render_delta = previewState.schedule.render_delta_seconds();", refresh)
        self.assertIn("worldgenPreview->render_to_backbuffer(renderPipeline, render_delta)", refresh)
        self.assertIn("*camera, render_delta,", compact(refresh))
        preview = block(read(CLIENT / "world/WorldgenPreview.cpp"), "bool WorldgenPreview::render_to_backbuffer(")
        self.assertIn("pipeline.render_frame(m_registry, *m_world, cam, dt,", preview)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        prepare = block(pipeline, "void RenderPipeline::prepare_frame(")
        self.assertIn("m_particle_pass->update(deltaTime);", prepare)
        self.assertIn("m_frame_prepared.delta_time = deltaTime;", prepare)
        self.assertIn("RenderMenuScreens(g_app, window, deltaTime,", compact(self.main))
        self.assertLess(shipping.start(), self.main.index("RenderMenuScreens(g_app,"))
        thumbnails = block(menu, "if (app.capture.ui_thumbs > 0")
        self.assertIn("*camera, deltaTime,", compact(thumbnails))

    def test_preview_capture_requires_ready_successes_and_keeps_the_timeout(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        preview = block(menu, "if (preview_live_screen)")
        readiness = preview[preview.index("const bool world_ready"):]
        reset = re.search(r"if \((.*?)\)\s*app.capture.ui_preview_settle_after_ready = 0;",
                          readiness, re.S).group(1)
        increment = re.search(r"else if \((.*?)\)\s*\+\+app.capture.ui_preview_settle_after_ready;",
                              readiness, re.S).group(1)
        settled = re.search(r"const bool settled = (.*?);", readiness, re.S).group(1)
        timeout = re.search(r"const bool timed_out = (.*?);", readiness, re.S).group(1)
        self.assertRegex(compact(readiness), r"kPreviewTimeoutSeconds = 45\.0;")
        self.assertRegex(compact(readiness), r"kPreviewSettleAfterReady = 40;")
        self.assertIn("ready_to_capture = settled || timed_out;", readiness)
        self.assertIn("worldgenPreview && worldgenPreview->world_ready()", preview)
        armed = block(preview, "if (!s_preview_wait_armed)")
        self.assertIn("app.capture.ui_preview_settle_after_ready = 0;", armed)
        self.assertIn("s_preview_wait_start = std::chrono::steady_clock::now();", armed)
        self.assertEqual(menu.count("s_preview_wait_start = std::chrono::steady_clock::now();"), 1)
        count = 0
        # Pending/missing world, successful renders, cache replays, refused renders,
        # readiness loss, inactive preview, then a fresh uninterrupted warmup.
        for ready, active, rendered, expected in (
                [(False, True, False, 0)] + [(True, True, True, n) for n in range(1, 40)] +
                [(True, True, False, 39), (False, True, False, 0),
                 (True, True, True, 1), (True, False, False, 0)] +
                [(True, True, True, n) for n in range(1, 41)]):
            values = dict(world_ready=ready, previewActive=active, scene_rendered=rendered)
            if cpp_expression(reset, **values):
                count = 0
            elif cpp_expression(increment, **values):
                count += 1
            self.assertEqual(count, expected)
            self.assertEqual(cpp_expression(settled, **{
                "world_ready": ready, "app.capture.ui_preview_settle_after_ready": count,
                "kPreviewSettleAfterReady": 40}), ready and count >= 40)
        for waited, expected in ((0, False), (44.999, False), (45, True), (46, True)):
            self.assertEqual(cpp_expression(timeout, waited_s=waited,
                                            kPreviewTimeoutSeconds=45.0), expected)

    def test_capture_invalidation_batch_advancement_and_thumbnail_siblings(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        transition = block(menu, "if (!previewState.capture_key || key != *previewState.capture_key)")
        for reset in ("app.capture.ui_screenshot_settle = 0;",
                      "app.capture.ui_preview_settle_after_ready = 0;",
                      "previewState.capture_key = key;"):
            self.assertIn(reset, transition)
        self.assertLess(menu.index("previewState.capture_key = key;"),
                        menu.index("schedule.update(now, key, interactive)"))
        self.assertIn("std::optional<MenuSceneKey> capture_key;", read(CLIENT / "app/MenuScreens.h"))
        without_scene = block(menu[menu.index("} else if (previewState.schedule.valid())"):], "else {")
        reset = block(without_scene, "if (previewState.capture_key)")
        for fragment in ("app.capture.ui_screenshot_settle = 0;",
                         "app.capture.ui_preview_settle_after_ready = 0;",
                         "previewState.capture_key.reset();"):
            self.assertIn(fragment, reset)
        # The inner ready log has no capture writes; use the final ready block.
        capture = block(menu[menu.index("} else if (app.capture.ui_screenshot_settle < 30)"):],
                        "if (ready_to_capture)")
        self.assertIn("s_preview_wait_armed = false;", capture)
        self.assertLess(capture.index("WriteUiScreenshot("), capture.index("++app.capture.ui_screen_index;"))
        for fragment in ("app.capture.ui_screenshot_settle = 0;",
                         "app.capture.ui_preview_settle_after_ready = 0;",
                         "previewState.capture_key.reset();", "RequestLoadDocument(",
                         "glfwSetWindowShouldClose(window, GLFW_TRUE);"):
            self.assertIn(fragment, capture)
        self.assertIn("app.capture.ui_screen_index < app.capture.ui_screens.size()", menu)
        thumbs = block(menu, "if (app.capture.ui_thumbs > 0")
        self.assertNotIn("schedule.update", thumbs)
        self.assertLess(thumbs.index("renderPipeline.render_frame("), thumbs.index("++app.capture.ui_thumbs_settle;"))
        self.assertIn("app.capture.ui_thumbs_settle < 28", thumbs)
        self.assertIn("app.capture.ui_thumbs_settle = 0;", thumbs)
        self.assertIn("++app.capture.ui_thumbs_index;", thumbs)

    def test_capture_gate_expires_refused_ordinary_and_live_preview(self):
        for content_ready in (False, True):
            gate = CaptureGateEvaluation()
            self.assertTrue(gate.arm())
            self.assertEqual(gate.decide(44.999, content_ready=content_ready), "Waiting")
            self.assertFalse(gate.arm(44.999))
            self.assertEqual(gate.decide(45, content_ready=content_ready), "TimedOut")
            self.assertEqual(gate.decide(46, settled=True), "TimedOut")
        gate = CaptureGateEvaluation()
        self.assertEqual(gate.decide(45, settled=True), "Waiting")
        gate.arm()
        self.assertEqual(gate.decide(44.999, settled=True), "Ready")
        self.assertEqual(gate.decide(45, settled=True), "TimedOut")

    def test_real_capture_policy_refused_first_candidate_fails_both_modes(self):
        for live in (False, True):
            capture = MenuCaptureEvaluation(live=live)
            for tick in range(45):
                self.assertEqual(capture.frame(tick), "Waiting")
                self.assertEqual(capture.count, 0)
            self.assertEqual(capture.frame(45), "TimedOut")

    def test_real_capture_policy_wait_replay_refusal_and_actual_renders(self):
        for live, needed in ((False, 30), (True, 40)):
            capture = MenuCaptureEvaluation(live=live)
            self.assertEqual(capture.frame(0), "Waiting")
            for tick in range(1, 40):
                self.assertEqual(capture.frame(tick / 100, ready=True), "Waiting")
                self.assertEqual(capture.count, 0)
            # A rejected new candidate may leave an older world drawable.
            self.assertEqual(capture.frame(1, rendered=True, ready=True, failed=True), "Waiting")
            self.assertEqual(capture.count, 0)
            for sample in range(1, needed + 1):
                result = capture.frame(2 + sample / 100, rendered=True, ready=True)
                self.assertEqual(capture.count, sample)
                self.assertEqual(result, "Ready" if live and sample == needed else "Waiting")
            self.assertEqual(capture.frame(3, ready=True), "Ready")

    def test_real_capture_policy_ui_layout_and_readiness_loss(self):
        capture = MenuCaptureEvaluation()
        for tick in range(30):
            self.assertEqual(capture.frame(tick / 100, preview=False), "Waiting")
        self.assertEqual(capture.frame(0.3, preview=False), "Ready")
        for missing in (dict(drawable=False), dict(document=False)):
            self.assertEqual(capture.frame(1, preview=False, **missing), "Waiting")
            self.assertEqual(capture.count, 0)
        self.assertEqual(capture.frame(45, preview=False), "TimedOut")

    def test_refused_source_cannot_be_cleared_by_an_older_successful_build(self):
        for live in (False, True):
            capture = MenuCaptureEvaluation(live=live)
            self.assertEqual(capture.frame(0, failed=True, source_failed=True), "Waiting")
            # An earlier/default candidate can finish after this request was refused.
            for sample in range(1, 100):
                self.assertEqual(capture.frame(sample / 10, rendered=True, ready=True,
                                               source_failed=True), "Waiting")
                self.assertEqual(capture.count, 0)
            self.assertEqual(capture.frame(45, rendered=True, ready=True,
                                           source_failed=True), "TimedOut")
            self.assertEqual(capture.frame(46, rendered=True, ready=True, index=1), "Waiting")
            self.assertEqual(capture.count, 1)

        source = read(CLIENT / "world/WorldgenPreview.cpp")
        admission = block(source, "bool WorldgenPreview::set_candidate(")
        refused = block(admission, "if (!result.ok)")
        self.assertIn("return false;", refused)
        self.assertNotIn("set_params(", refused)
        self.assertLess(admission.index("return false;"), admission.index("m_data_root = data_root;"))
        self.assertLess(admission.index("set_params(result.params, seed);"),
                        admission.index("return true;"))
        candidate = block(read(CLIENT / "app/MenuScreens.cpp"), "if (sig != previewState.lastSig)")
        self.assertIn("previewState.candidate_source_failed = "
                      "!worldgenPreview->set_candidate(resolved, data_root, seedVal);", compact(candidate))

    def test_older_world_cannot_settle_a_pending_or_refused_replacement(self):
        readiness = block(read(CLIENT / "world/WorldgenPreview.h"), "bool candidate_ready() const")
        predicate = re.fullmatch(r"\s*return (.*?);\s*", readiness, re.S).group(1)
        for world, dirty, inflight, failed in ((False, False, False, False),
                                             (True, True, False, False),
                                             (True, False, True, False),
                                             (True, False, False, True),
                                             (True, False, False, False)):
            self.assertEqual(cpp_expression(predicate, **{
                "world_ready()": world, "m_pending_dirty": dirty,
                "m_build_inflight.load()": inflight, "m_last_build_failed": failed}),
                world and not (dirty or inflight or failed))
        for live, required in ((False, 30), (True, 40)):
            capture = MenuCaptureEvaluation(live=live)
            for sample in range(100):
                self.assertEqual(capture.frame(sample / 10, rendered=True, ready=True,
                                               candidate_ready=False), "Waiting")
                self.assertEqual(capture.count, 0)
            for sample in range(required):
                capture.frame(11 + sample / 100, rendered=True, ready=True, candidate_ready=True)
            self.assertEqual(capture.count, required)
            self.assertEqual(capture.frame(12, ready=True, candidate_ready=True), "Ready")
            self.assertEqual(capture.frame(13, rendered=True, ready=True, candidate_ready=False), "Waiting")
            self.assertEqual(capture.count, 0)
            self.assertEqual(capture.frame(45, ready=True, candidate_ready=False), "TimedOut")

    def test_capture_gate_rearms_for_screen_advance_and_mode_changes_only(self):
        gate = CaptureGateEvaluation()
        gate.arm()
        self.assertEqual(gate.decide(44, settled=True), "Ready")
        self.assertTrue(gate.arm(44, index=1))
        self.assertEqual(gate.decide(45), "Waiting")
        for tick in range(45, 89):
            self.assertFalse(gate.arm(tick, index=1))
        self.assertEqual(gate.decide(89), "TimedOut")
        self.assertTrue(gate.arm(89, index=1, thumbnail=True))
        self.assertEqual(gate.decide(90, settled=True), "Ready")
        self.assertFalse(gate.arm(133, index=1, thumbnail=True))
        self.assertEqual(gate.decide(134), "TimedOut")
        gate.call("void reset()")
        self.assertEqual(gate.decide(150, settled=True), "Waiting")
        self.assertTrue(gate.arm(150, index=1, thumbnail=True))

    def test_capture_gate_uses_real_settling_admission_for_wait_replay_and_ui(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        regular = re.sub(r"//[^\n]*", "", block(menu, "else if (app.capture.ui_screenshot_settle < 30)"))
        predicate = re.fullmatch(r"\s*if \((.*?)\)\s*"
                                 r"\+\+app.capture.ui_screenshot_settle;\s*", regular, re.S).group(1)
        for scenic, preview in ((True, False), (False, True), (False, False)):
            gate = CaptureGateEvaluation()
            gate.arm()
            count = 0
            for tick in range(120):
                rendered = tick >= 60 and tick % 2 == 0
                settled = count >= 30  # The shipping ordinary branch captures on the next frame.
                if not settled and cpp_expression(predicate, scene_rendered=rendered,
                                                   scenic=scenic, previewActive=preview):
                    count += 1
                expected = "Ready" if (tick >= 30 if not (scenic or preview) else tick == 119) else "Waiting"
                self.assertEqual(gate.decide(tick / 60, settled=settled), expected)
            self.assertEqual(count, 30)

    def test_capture_target_or_content_loss_cannot_publish_or_extend_deadline(self):
        for drawable, content in ((False, True), (True, False), (False, False)):
            gate = CaptureGateEvaluation()
            gate.arm()
            for tick in range(45):
                self.assertEqual(gate.decide(tick, settled=True, drawable=drawable,
                                            content_ready=content), "Waiting")
                self.assertFalse(gate.arm(tick))
            self.assertEqual(gate.decide(45, settled=True, drawable=drawable,
                                        content_ready=content), "TimedOut")

    def test_capture_deadline_is_armed_before_every_render_and_failure_closes(self):
        source = read(CLIENT / "app/MenuScreens.cpp")
        menu = block(source, "void RenderMenuScreens(")
        setup = block(source, "bool PrepareMenuCapture(")
        capturing = block(setup, "if (capturing)")
        self.assertLess(capturing.index("capture_gate.arm("), capturing.index("capture_gate.decide("))
        self.assertLess(menu.index("if (!PrepareMenuCapture("), menu.index("renderPipeline.render_frame("))
        self.assertRegex(compact(menu), r"if \(!PrepareMenuCapture\([^;]*\)\) return;")
        self.assertIn("const bool capturing = thumbnails_pending || screenshot_pending;", compact(setup))
        self.assertIn("!thumbnails_pending && !app.capture.ui_screens.empty()", menu)
        failure = block(source, "void FailMenuCapture(")
        self.assertIn("previewState.capture_failed = true;", failure)
        self.assertIn("glfwSetWindowShouldClose(window, GLFW_TRUE);", failure)
        self.assertRegex(compact(self.main), r"if \(menuPreviewState.capture_failed && exit_code == 0\) exit_code = 1;")
        self.assertNotIn("static bool s_preview_wait_armed", source)
        self.assertIn("previewState.preview_wait_armed = false;", capturing)

    def test_capture_refusal_missing_document_and_zero_size_reset_settling(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        content = re.search(r"const bool content_ready\s*=\s*(.*?);", read(CLIENT / "app/MenuScreens.cpp"), re.S).group(1)
        for fragment in ("previewState.capture_document_ready", "previewActive && worldgenPreview",
                         "worldgenPreview->world_ready()", "!worldgenPreview->last_build_failed()",
                         "worldgenPreview->candidate_ready()", "!previewState.candidate_source_failed"):
            self.assertIn(fragment, content)
        ready = {"previewState.capture_document_ready": True,
                 "app.menu.menu_backdrop_active": False, "scenic": False,
                 "previewActive": True, "expects_preview": True, "worldgenPreview": True,
                 "worldgenPreview->world_ready()": True,
                 "worldgenPreview->candidate_ready()": True,
                 "worldgenPreview->last_build_failed()": False,
                 "previewState.candidate_source_failed": False}
        self.assertTrue(cpp_expression(content, **ready))
        for name in ("previewState.capture_document_ready", "previewActive", "worldgenPreview",
                     "worldgenPreview->world_ready()", "worldgenPreview->last_build_failed()",
                     "worldgenPreview->candidate_ready()", "previewState.candidate_source_failed"):
            refused = dict(ready)
            refused[name] = not refused[name]
            self.assertFalse(cpp_expression(content, **refused))
        ui_only = dict(ready, expects_preview=False, previewActive=False, worldgenPreview=False)
        self.assertTrue(cpp_expression(content, **ui_only))
        ui_only["app.menu.menu_backdrop_active"] = True
        self.assertFalse(cpp_expression(content, **ui_only))
        reset = block(menu, "if (!content_ready || !drawable)")
        for fragment in ("app.capture.ui_screenshot_settle = 0;",
                         "app.capture.ui_preview_settle_after_ready = 0;", "ready_to_capture = false;"):
            self.assertIn(fragment, reset)
        candidate = block(menu, "if (sig != previewState.lastSig)")
        self.assertLess(candidate.index("candidate_source_failed = true;"), candidate.index("ReadPreviewPreset("))
        reader = block(read(CLIENT / "app/MenuScreens.cpp"), "bool ReadPreviewPreset(")
        self.assertIn("Persistence::ReadSavedJson(", reader)
        self.assertIn("!missing", reader)
        self.assertIn("previewState.candidate_source_failed = "
                      "!worldgenPreview->set_candidate(resolved, data_root, seedVal);", compact(candidate))
        setup = block(read(CLIENT / "app/MenuScreens.cpp"), "bool PrepareMenuCapture(")
        self.assertIn("requested && requested->IsVisible()", setup)
        self.assertIn("context->GetDocument(documents - 1)", setup)
        self.assertIn("requested->GetSourceURL().ends_with(", setup)
        self.assertIn("if (armed || capture_width != previewState.capture_width", setup)
        self.assertEqual(setup.count("capture_gate.arm("), 1)
        drawable = re.search(r"const bool drawable = (.*?);", menu[menu.index("const bool expects_preview"):], re.S).group(1)
        for width, height in ((0, 0), (0, 720), (1280, 0), (-1, 720), (1280, -1), (1280, 720)):
            self.assertEqual(cpp_expression(drawable, capture_width=width, capture_height=height),
                             width > 0 and height > 0)

    def test_capture_final_decisions_and_output_errors_precede_advancement(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        setup = block(read(CLIENT / "app/MenuScreens.cpp"), "bool PrepareMenuCapture(")
        self.assertEqual((menu + setup).count("MenuCaptureGate::Result::TimedOut"), 3)
        capture = menu[menu.index("const bool expects_preview"):]
        self.assertIn("ready_to_capture = decision == MenuCaptureGate::Result::Ready;", capture)
        self.assertIn("capture_gate.decide( std::chrono::steady_clock::now(), "
                      "ready_to_capture, drawable, content_ready)", compact(capture))
        self.assertLess(capture.index("MenuCaptureGate::Result::TimedOut"), capture.index("if (ready_to_capture)"))
        for body, writer, advance in ((capture, "if (!WriteUiScreenshot(", "++app.capture.ui_screen_index;"),
                                      (block(menu, "if (app.capture.ui_thumbs > 0"),
                                       "if (!WritePixelBufferPpm(", "++app.capture.ui_thumbs_index;")):
            refusal = block(body, writer)
            self.assertIn("fail_capture(", refusal)
            self.assertIn("return;", refusal)
            self.assertLess(body.index(writer), body.index(advance))
        output = block(read(CLIENT / "app/MenuScreens.cpp"), "bool WritePixelBufferPpm(")
        self.assertLess(output.index("if (!rgba_bytes)"), output.index("std::vector<unsigned char>"))
        self.assertIn("(*rgba_bytes / 4) * 3", output)
        self.assertIn("return WritePixelBufferPpm(shot, width, height, pixels);", output)
        self.assertIn("return false;", block(output, "catch (const std::exception& error)"))

    def test_capture_writer_reports_write_flush_and_close_failures(self):
        writer = block(read(CLIENT / "rendering/PixelIo.cpp"), "bool WritePixelBufferPpm(")
        stream_writer = block(read(CLIENT / "rendering/PixelIo.cpp"), "bool WritePixelBufferPpmStream(")
        self.assertIn("output.write(", stream_writer)
        self.assertIn("return FlushCaptureStream(output);", stream_writer)
        self.assertIn("const bool written = WritePixelBufferPpmStream(output, width, height, pixels);", writer)
        self.assertIn("const bool closed = CloseCaptureFile(output);", writer)
        tail = writer[writer.index("const bool written ="):]
        helper = read(CLIENT / "rendering/PixelIo.h")
        flush = block(helper, "bool FlushCaptureStream(")
        self.assertLess(flush.index("output.flush();"), flush.index("return static_cast<bool>(output);"))
        close = block(helper, "bool CloseCaptureFile(")
        self.assertLess(close.index("FlushCaptureStream(output)"), close.index("output.close();"))
        self.assertIn("return flushed && static_cast<bool>(output);", close)
        failure = block(tail, "if (!written || !closed)")
        self.assertIn("return false;", failure)
        self.assertIn("Failed to finish screenshot artifact", failure)
        self.assertLess(tail.index("if (!written || !closed)"), tail.index("return true;"))

    def test_thumbnail_keys_and_readback_refusals_preserve_batch_progress(self):
        source = read(CLIENT / "app/MenuScreens.cpp")
        pending = re.fullmatch(r"\s*return (.*?);\s*",
                               block(source, "bool MenuThumbnailsPending("), re.S).group(1)
        for requested, index in ((0, 0), (-1, 0), (1, 0), (1, 1), (1, 2), (2, 1)):
            self.assertEqual(cpp_expression(pending, **{
                "app.capture.ui_thumbs": requested, "app.capture.ui_thumbs_index": index}),
                requested > 0 and index < requested)
        menu = block(source, "void RenderMenuScreens(")
        thumbs = block(menu, "if (app.capture.ui_thumbs > 0")
        scene = block(menu, "if (scenic || previewActive)")
        self.assertEqual(compact(block(thumbs, ".settings =")),
                         compact(block(scene, ".settings =")))
        for fragment in (".width = capture_width", ".height = capture_height",
                         ".document = previewState.capture_document",
                         ".renderer_target = renderPipeline.resize_generation()",
                         ".camera = previewState.camera_revision.observe(MenuCameraState(*camera))",
                         ".wireframe = app.overlay.wireframe_mode"):
            self.assertIn(fragment, thumbs)
        self.assertLess(thumbs.index("camera->updateCameraVectors();"), thumbs.index("thumbnail_key{"))
        changed = block(thumbs, "if (previewState.capture_key != thumbnail_key)")
        self.assertIn("app.capture.ui_thumbs_settle = 0;", changed)
        self.assertIn("previewState.capture_key = thumbnail_key;", changed)
        self.assertNotIn("capture_gate.arm", changed)
        zero = block(thumbs, "if (capture_width <= 0 || capture_height <= 0)")
        self.assertIn("app.capture.ui_thumbs_settle = 0;", zero)
        self.assertIn("return;", zero)
        self.assertIn("app.capture.ui_thumbs_index >= app.capture.ui_thumbs && "
                      "app.capture.ui_screen_index >= app.capture.ui_screens.size()", compact(thumbs))
        readback = block(source, "bool WriteUiScreenshot(")
        self.assertRegex(compact(readback), r"if \(!WritePixelBufferPpm\(shot, width, height\)\) return false;")
        self.assertLess(readback.index("return false;"), readback.index("UI screenshot written"))

    def test_blur_reuses_geometry_and_kernels_without_changing_the_passes(self):
        backend = read(CLIENT / "ui/gl3/RmlUi_Renderer_GL3.cpp")
        quad = block(backend, "void RenderInterface_GL3::DrawFullscreenQuad(Rml::Vector2f")
        identity = block(quad, "if (uv_offset == Rml::Vector2f() && uv_scaling == Rml::Vector2f(1.f))")
        self.assertEqual(compact(identity), "DrawFullscreenQuad(); return;")
        hit = block(quad, "if (const auto* cached = filter_quad_cache.find(key))")
        self.assertNotIn("CompileGeometry", hit)
        self.assertNotIn("ReleaseGeometry", hit)
        self.assertIn("return;", hit)
        self.assertIn("filter_quad_cache.store(key, geometry)", quad)
        kernel = block(backend, "static void SetBlurWeights(")
        hit = block(kernel, "if (const auto* cached = kernels.find(key))")
        self.assertIn("glUniform1fv", hit)
        self.assertNotIn("Exp(", hit)
        self.assertIn("kernels.store(key, weights)", kernel)

    def test_nullable_frame_scan_count_requires_v2_for_all_capture_paths(self):
        writer = compact(block(read(CLIENT / "rendering/FrameScan.cpp"),
                               "bool WriteFrameScanReport("))
        self.assertIn('j["schema"] = "luminumbra.frame_scan.v2";', writer)
        self.assertIn('foliage["instances"] = report.foliage_instance_count_known ? '
                      'nlohmann::json(report.foliage_instances) : nlohmann::json(nullptr);', writer)
        self.assertIn('foliage["instance_capacity"] = report.foliage_instance_capacity;', writer)
        self.assertIn('j["foliage"] = std::move(foliage);', writer)
        for source in (self.main, read(CLIENT / "rendering/SceneSurvey.cpp")):
            self.assertIn("WriteFrameScanReport(", source)
            self.assertNotIn("luminumbra.frame_scan.", source)

    def test_recorder_propagates_writer_failure_and_checks_stream_state(self):
        record = compact(block(self.collector, "bool FrameRecorder::record("))
        self.assertTrue(record.startswith("if (!active()) return false; return write_csv_row(m_out,"))
        self.assertEqual(compact(block(self.header, "bool active() const")),
                         "return m_out.is_open() && m_out.good();")

    def test_worker_counters_reach_frame_totals_csv_and_split_log(self):
        for name in ("worker_dispatched", "worker_completed", "worker_stale_rejected",
                     "worker_queue_age_frames"):
            self.assertIn(f"std::uint64_t {name} = 0;", self.header)
            self.assertIn(f"total.{name}", self.collector)
            self.assertIn(f"foliage.{name}", self.collector)
            self.assertIn(f"foliage_attribution.{name}", self.main)
            self.assertIn(f"m_rebuild_attribution.{name}", self.foliage)
        self.assertIn("std::max(total.worker_queue_age_frames, rebuild.worker_queue_age_frames)",
                      compact(self.collector))

    def test_worker_budget_follows_every_rendered_frame_including_nested_loops(self):
        self.assertNotIn("foliage->begin_ground_frame();", self.main)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        constructor = block(pipeline, "RenderPipeline::RenderPipeline()")
        self.assertIn("m_foliage_pass->begin_ground_frame();", constructor)
        render = block(pipeline, "void RenderPipeline::render_frame(")
        self.assertIn("m_prev_time = static_cast<float>(Client::presentation_clock.now(glfwGetTime));",
                      render)
        self.assertEqual(render.count("m_foliage_pass->begin_ground_frame();"), 1)
        self.assertGreater(render.index("m_foliage_pass->begin_ground_frame();"),
                           render.index("dispatch_stages(camera);"))
        self.assertTrue(compact(render).endswith("m_foliage_pass->begin_ground_frame();"))
        # Every nested loop uses this common boundary, including callers that do
        # not rebuild foliage themselves (survey, boot metrics, menu screenshots).
        for path in ("main_client.cpp", "world/WorldgenPreview.cpp",
                     "rendering/SceneSurvey.cpp", "app/MenuScreens.cpp",
                     "core/ScenarioRunnerCapture.cpp"):
            self.assertIn("render_frame(", read(CLIENT / path))
        self.assertEqual(self.foliage.count("prepare_ground_samples(chunk)"), 2)
        self.assertIn("if (!m_ground_frame_driven) m_ground_queue.begin_frame(); "
                      "else m_ground_queue.clear_stats();", compact(self.foliage))
        queue = read(CLIENT / "rendering/FoliageGroundWork.h")
        self.assertIn("kInFlight = 8;", queue)
        self.assertIn("kPerFrame = 4;", queue)
        self.assertNotIn("std::function", queue)
        self.assertNotIn("glBind", queue)
        self.assertNotIn("JobSystem::wait", self.foliage)
        self.assertIn("m_ground_queue.invalidate(key);", self.foliage)
        self.assertIn("m_ground_queue.invalidate();", self.foliage)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        self.assertIn("m_foliage_pass->attach_job_system(job_system);", pipeline)

    def test_ground_query_publication_releases_resident_payloads_on_both_paths(self):
        synchronous = compact(block(self.foliage, "if (!mesh.query)"))
        self.assertIn("mesh.query = std::make_shared<const FoliageGroundMesh>(", synchronous)
        self.assertIn("mesh.payload.reset();", synchronous)
        self.assertLess(synchronous.index("mesh.payload->indices);"),
                        synchronous.index("mesh.payload.reset();"))
        self.assertLess(self.foliage.index("mesh.payload.reset();"),
                        self.foliage.index("mesh.query->sample(x, z);"))
        polling = block(self.foliage, "void FoliagePass::poll_ground_builds(")
        publication = compact(block(polling, "if (resident != column->second.meshes.end())"))
        self.assertEqual(publication, "resident->second.query = mesh.query; "
                         "resident->second.payload.reset();")
        # Only validated completions publish; stale or failed jobs are discarded.
        worker = read(CLIENT / "rendering/FoliageGroundWork.h")
        polling = compact(block(worker, "void poll(Validate validate, Consume consume)"))
        self.assertIn("if (slot.invalidated || !validate(slot.build->input)) "
                      "++m_stats.stale_rejected; else if (!slot.build->failed) "
                      "consume(*slot.build); slot.build.reset();", polling)
        self.assertIn("const FoliageGroundRequest input;", worker)
        self.assertIn("queries.push_back({mesh.id, {}, std::move(query)});", worker)
        self.assertNotIn("queries.push_back({mesh.id, mesh.payload,", worker)

    def test_preview_rebuilds_current_support_even_with_cached_scatter_inputs(self):
        preview = read(CLIENT / "world/WorldgenPreview.cpp")
        rebuild = block(preview, "void WorldgenPreview::rebuild_foliage_if_needed(")
        cached = block(rebuild, "if (m_last_foliage_generation != m_rebuild_generation)")
        self.assertIn("m_foliage_scatter.clear();", cached)
        self.assertNotIn("return;", cached.split("for (", 1)[0])
        self.assertNotIn("rebuild_instances(", cached)
        self.assertNotIn("m_last_foliage_generation == m_rebuild_generation", rebuild)
        self.assertIn("Rendering::FoliagePass::Preparation::Complete", rebuild)
        self.assertEqual(rebuild.count("rebuild_instances("), 1)
        for entry in ("bool WorldgenPreview::render(", "bool WorldgenPreview::render_to_backbuffer("):
            self.assertIn("rebuild_foliage_if_needed(pipeline);", block(preview, entry))

    def test_sweep_qualification_and_capture_callers_require_complete_preparation(self):
        sweep = read(CLIENT / "core/scenarios/WorldVisualSweep.cpp")
        # Scatter is now built after residency; warmup has no stale spawn scatter.
        self.assertEqual(sweep.count("rebuild_instances("), 1)
        self.assertEqual(sweep.count("FoliagePass::Preparation::Complete"), 1)
        self.assertGreater(sweep.index("chunk_scatter = BuildSweepFoliageScatter(world_system);"),
                           sweep.index("if (!streaming_ready)"))
        drive = block(read(CLIENT / "core/ScenarioRunnerDrive.cpp"),
                      "bool ScenarioRunnerImpl::onPreRenderFoliage()")
        self.assertIn("FoliagePass::Preparation::Complete", drive)
        update = block(self.main, "static void UpdateFoliageScatter(")
        preparation = block(self.main, "static bool CompleteFoliagePreparation(")
        self.assertIn("CompleteFoliagePreparation(g_app, scenario_config, photo_mode_active)",
                      compact(update))
        for mode in ("scenario_config.active()", "capture.scene_active",
                     "capture.render_benchmark_path.empty()", "capture.frame_scan_active",
                     "capture.render_parity_active", "capture.timelapse_frames > 0",
                     "capture.survey_active", "photo_mode_active"):
            self.assertIn(mode, preparation)
        self.assertIn("g_playerController->photo_mode_active()", self.main)
        self.assertIn("FoliagePass::Preparation::Complete", update)
        self.assertIn("FoliagePass::Preparation::Deferred", update)
        rebuild = compact(block(self.foliage, "void FoliagePass::rebuild_instances("))
        self.assertIn("if (preparation == Preparation::Complete) m_worker_ground_active = false;",
                      rebuild)
        self.assertIn("preparation == Preparation::Complete ? std::numeric_limits<int>::max() : 4;",
                      rebuild)
        gpu = compact(block(self.foliage, "bool FoliagePass::rebuild_instances_gpu("))
        self.assertIn("m_readback_enabled || preparation == Preparation::Complete", gpu)

    def test_worker_polling_preserves_generation_safe_gpu_readback(self):
        rebuild = block(self.foliage, "void FoliagePass::rebuild_instances(")
        signature = rebuild.index("std::uint64_t sig =")
        self.assertLess(rebuild.index("poll_ground_builds(chunks, camera_pos);"), signature)
        # Frozen and unchanged inputs may drain their matching snapshots. A
        # changed input must advance its generation before consuming GPU output.
        before_signature = rebuild[:signature]
        frozen = block(before_signature, "if (m_qualification.frozen_inputs)")
        self.assertEqual(before_signature.count("poll_foliage_readback();"),
                         frozen.count("poll_foliage_readback();"))
        changed = rebuild[rebuild.index("m_uploaded_input_hash.reset();", signature):]
        self.assertLess(changed.index("++m_build_generation;"),
                        changed.index("poll_foliage_readback();"))
        self.assertLess(changed.index("m_gpu_generated_count.reset();"),
                        changed.index("poll_foliage_readback();"))
    def test_sampler_availability_is_in_both_signatures_before_cache_reuse(self):
        rebuild = block(self.foliage, "void FoliagePass::rebuild_instances(")
        reuse = rebuild.index("if (m_scatter_built && sig == m_last_scatter_sig")
        # Evaluate the actual availability predicate in each signature for both
        # transitions, rather than relying on a phase/configuration change.
        for mixer in ("mix", "config_mix"):
            match = re.search(r"\b" + mixer + r"\((query != nullptr) \? (\d+)ull : (\d+)ull\);",
                              rebuild[:reuse])
            self.assertIsNotNone(match, mixer + " must include sampler availability")
            values = [int(match.group(2)) if cpp_expression(match.group(1),
                      query=1 if present else None, nullptr=None) else int(match.group(3))
                      for present in (False, True)]
            self.assertNotEqual(values[0], values[1])
        self.assertGreater(rebuild.index("query == nullptr"), reuse)
        native = read(ROOT / "test/rendering/foliage_invalidation_test.cpp")
        for name, initial in (("SamplerLossInvalidatesCompletedScatter", "true"),
                              ("SamplerRestorationInvalidatesInactiveScatter", "false")):
            test = block(native, "TEST_F(FoliageInvalidationTest, " + name + ")")
            self.assertIn("ExpectSamplerTransition(" + initial + ")", test)
        transition = block(native, "static void ExpectSamplerTransition(")
        for fragment in ("for (const bool gpu : {false, true})",
                         "for (const bool rendered_ground : {false, true})",
                         "EXPECT_EQ(pass.build_generation(), generation + 1);",
                         "EXPECT_EQ(pass.build_generation(), generation + 2);",
                         "EXPECT_EQ(changed.uploaded_bytes, 0u);",
                         "ExpectSame(before, Records(pass, gpu));"):
            self.assertIn(fragment, transition)
        self.assertIn("rendering/foliage_invalidation_test.cpp", read(ROOT / "test/CMakeLists.txt"))

    def test_froxel_publishes_programs_and_volumes_only_after_complete_initialization(self):
        source = read(CLIENT / "rendering/passes/FroxelPass.cpp")
        inject = block(source, "bool FroxelPass::execute_inject(")
        self.assertIn("if (m_initialization_state == InitializationState::Uninitialized)", inject)
        attempt = block(inject, "if (m_initialization_state == InitializationState::Uninitialized)")
        shader_guard = "if (pending.inject == 0 || pending.integrate == 0)"
        volume_guard = 'if (!make_volume(pending.scatter, "froxel.scatter")'
        self.assertIn("return false;", block(attempt, shader_guard))
        self.assertIn("return false;", block(attempt, volume_guard))
        self.assertLess(attempt.index(shader_guard), attempt.index(volume_guard))
        self.assertIn('!make_volume(pending.integrated, "froxel.integrated")', attempt)
        self.assertIn("if (!ValidateLayout(pending.inject, inject_layout))", attempt)
        self.assertLess(attempt.index("if (!ValidateLayout("), attempt.index(volume_guard))
        members = (("m_inject_program", "inject", "AttributedGl::Program{}"),
                   ("m_integrate_program", "integrate", "AttributedGl::Program{}"),
                   ("m_scatter_tex", "scatter", "0"), ("m_integrated_tex", "integrated", "0"))
        ready = attempt.index("m_initialization_state = InitializationState::Ready;")
        for member, pending, empty in members:
            assignment = f"{member} = std::exchange(pending.{pending}, {empty});"
            self.assertEqual(inject.count(member + " ="), 1)
            self.assertLess(attempt.index(volume_guard), attempt.index(assignment))
            self.assertLess(attempt.index(assignment), ready)
        cleanup = compact(block(source, "~PendingFroxelResources()"))
        for member in ("inject", "integrate"):
            self.assertIn(f"if ({member}) AttributedGl::DeleteProgram(glDeleteProgram, {member});", cleanup)
        for member in ("scatter", "integrated"):
            self.assertIn(f"if ({member}) glDeleteTextures(1, &{member});", cleanup)
        make_volume = compact(block(attempt, "auto make_volume ="))
        self.assertIn("[](GLuint& t, const char* label) -> bool", attempt)
        self.assertIn("if (t == 0) return false;", make_volume)
        self.assertIn("glGetTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);", make_volume)
        self.assertTrue(make_volume.endswith("return immutable == GL_TRUE;"))
        self.assertIn("if (file.bad() || source.fail()) return {};", compact(attempt))

    def test_froxel_failure_is_latched_until_explicit_destroy_and_retry_is_registered(self):
        source = read(CLIENT / "rendering/passes/FroxelPass.cpp")
        inject = compact(block(source, "bool FroxelPass::execute_inject("))
        failed = "if (m_initialization_state == InitializationState::Failed) return false;"
        attempt = "m_initialization_state = InitializationState::Failed;"
        self.assertIn(failed, inject)
        self.assertLess(inject.index(failed), inject.index(attempt))
        self.assertLess(inject.index("if (ctx.frozen_replay) return false;"), inject.index(attempt))
        self.assertLess(inject.index(attempt), inject.index("PendingFroxelResources pending;"))
        integrate = compact(block(source, "void FroxelPass::execute_integrate("))
        self.assertIn("m_initialization_state != InitializationState::Ready", integrate)
        destroy = compact(block(source, "void FroxelPass::destroy()"))
        self.assertTrue(destroy.endswith("m_initialization_state = InitializationState::Uninitialized;"))
        native = read(ROOT / "test/rendering/pass_context_test.cpp")
        regression = block(native, "TEST(PassContext, FroxelInitializationFailureCleansUpAndRequiresExplicitRetry)")
        for fragment in ("failure < 9", "calls.failed_shader =", "calls.failed_texture =",
                         "calls.failed_storage =", "calls.invalid_layout =", "failure == 7",
                         "EXPECT_THROW(pass.execute_inject(ctx, root), std::runtime_error);",
                         "EXPECT_EQ(calls.live_programs, 0);", "EXPECT_EQ(calls.live_textures, 0);",
                         "EXPECT_FALSE(pass.execute_inject(ctx, root));", "pass.destroy();",
                         "ASSERT_TRUE(pass.execute_inject(ctx, root));",
                         "EXPECT_EQ(calls.texture_calls, textures + 2);",
                         "EXPECT_EQ(calls.dispatches, 2);"):
            self.assertIn(fragment, regression)
        self.assertIn("TEST(PassContext, FroxelOffAndUninitializedFrozenReplayDoNotAllocateOrLatchFailure)", native)
        self.assertIn("rendering/pass_context_test.cpp", read(ROOT / "test/CMakeLists.txt"))

    def test_job_callback_state_outlives_shutdown_on_returns_and_dispatch_exceptions(self):
        source = read(ROOT / "test/common/JobSystem_test.cpp")
        # Declaration order protects fatal assertion returns and partial dispatch
        # exceptions; a successful wait alone cannot establish that contract.
        sites = (
            ("DispatchRunsThousandsOfIndependentJobs", "RunningJobSystem system;",
             ("std::atomic<int> completed{0};", "std::mutex mutex;", "std::condition_variable completed_cv;")),
            ("BatchDispatchCompletesManyIndependentBatches", "RunningJobSystem system;",
             ("std::array<std::atomic<int>, kBatchCount> batch_counts;", "std::atomic<int> completed{0};",
              "std::mutex mutex;", "std::condition_variable completed_cv;")),
            ("BoundedDispatchRetainsPayloadUntilExecutionThenReleasesStorage", "GatedSingleWorker system;",
             ("std::atomic<int> completed{0};",)),
            *[(name, "GatedSingleWorker gated;", ("std::mutex order_mutex;", "std::vector<int> completion_order;"))
              for name in ("HighJobsCompleteBeforeQueuedNormalBacklog", "NormalJobsAreNotStarvedByHighBacklog",
                           "RuntimeStatsReportPerLaneQueueDepths")],
            ("BatchCountersCanBeReusedAcrossSequentialBatches", "RunningJobSystem system;",
             ("std::atomic<int> completed{0};",)),
            ("RingGrowsAndRecyclesAcrossManyCycles", "RunningJobSystem system;",
             ("std::atomic<long long> grand_total{0};", "std::atomic<int> ran{0};")),
            ("NestedDispatchCompletesFully", "RunningJobSystem system;",
             ("std::atomic<int> inner_ran{0};", "std::mutex handles_mutex;",
              "std::vector<Luminumbra::JobHandle> inner_handles;")),
            ("ThrowingJobsStillDrainTheBarrier", "RunningJobSystem system;",
             ("std::atomic<int> survived{0};", "std::atomic<int> after{0};")),
        )
        for name, owner, declarations in sites:
            with self.subTest(site=name):
                test = block(source, ", " + name + ")")
                for declaration in declarations:
                    self.assertLess(test.index(declaration), test.index(owner))
        completion = block(source, "void CheckCaptureCompletion(")
        self.assertLess(completion.index("std::atomic<int> reused{0};"),
                        completion.index("Luminumbra::JobSystem job_system;"))
        throughput = read(ROOT / "test/common/jobsystem_throughput_test.cpp")
        self.assertLess(throughput.index("std::atomic<int> completed{0};"),
                        throughput.index("Luminumbra::JobSystem job_system;"))
        # Shared payloads own their captures; the explicit close/reject test
        # cannot run callbacks after its already-completed shutdown.
        bounded = block(source, ", BoundedDispatchChecksBothLanesAndDrainsAcceptedPayloadsOnClose)")
        self.assertIn("std::make_shared<std::atomic<int>>(0)", bounded)
        closed = block(source, ", DispatchAfterShutdownRejectsWithoutHang)")
        self.assertLess(closed.index("job_system.shutdown();"), closed.index("dispatch_batch("))
        self.assertIn("common/JobSystem_test.cpp", read(ROOT / "test/CMakeLists.txt"))

    def test_metadata_replacement_does_not_upload_a_redundant_command(self):
        gpu = block(self.foliage, "bool FoliagePass::rebuild_instances_gpu(")
        self.assertIn("const bool initialize_command = !m_metadata_valid;", gpu)
        reset = compact(block(gpu, "if (initialize_command || !metadata_changed)"))
        for operation in ("glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_count_ssbo);",
                          "glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(draw_command), draw_command);",
                          "m_rebuild_attribution.uploaded_bytes += sizeof(draw_command);",
                          "++m_rebuild_attribution.uploads;"):
            self.assertIn(operation, reset)
        self.assertEqual(gpu.count("sizeof(draw_command), draw_command)"), 1)
        # Count never consumes the old command. Prefix writes every word, then
        # the storage barrier makes those writes visible before emit/draw.
        scatter = read(ROOT / "res/shaders/grass_scatter.comp")
        for name in ("append_count", "draw_vertex_count", "draw_instance_count",
                     "draw_first_vertex", "draw_base_instance"):
            self.assertEqual(len(re.findall(r"\b" + name + r"\b", scatter)), 2 if name == "append_count" else 1)
        prefix = compact(block(read(ROOT / "res/shaders/grass_compact_prefix.comp"), "void main()"))
        for assignment in ("accepted_count = total;", "vertex_count = 12u;",
                           "instance_count = min(total, u_max_instances);",
                           "first_vertex = 0u;", "base_instance = 0u;"):
            self.assertIn(assignment, prefix)
        dispatch = compact(block(self.foliage, "void FoliagePass::dispatch_scatter()"))
        self.assertIn("Luminumbra::Rendering::AttributedGl::Submit(glDispatchCompute, 1u, 1u, 1u); "
                      "glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); "
                      "Luminumbra::Rendering::AttributedGl::UseProgram(glUseProgram, m_compute_prog); "
                      "glUniform1i(m_scatter_uniforms.emit, GL_TRUE);", dispatch)

    def test_csv_schema_preserves_existing_indices_and_appends_dirty_columns(self):
        header = block(self.collector, "void write_csv_header(")
        static_header, phase_header = header.split("    for (std::size_t index", 1)
        encoded = "".join(json.loads(literal) for literal in re.findall(r'"(?:\\.|[^"\\])*"', static_header))
        self.assertEqual(compact("for (std::size_t index" + phase_header),
                         "for (std::size_t index = 0; index < kPhaseCount; ++index) "
                         "out << ',' << phase_name(static_cast<Phase>(index)) << \"_ms\"; "
                         'out << ",dirty_columns,water_selection_ms," '
                         '"water_seed_inputs_ms,water_initialization_ms,water_sim_ms,water_seam_ms," '
                         '"water_bookkeeping_ms,water_precomputed_columns,water_fallback_columns," '
                         '"water_stale_columns,worker_dispatched,worker_completed,worker_stale_rejected," '
                         '"worker_queue_age_frames,foliage_other_ms\\n";')
        self.assertEqual(encoded.rstrip("\n").split(","), [
            "frame", "frame_ms", "sim_ms", "stream_ms", "render_ms", "present_ms",
            "foliage_ms", "scatter_ms", "rebake_ms", "poll_ms", "menu_ms", "ui_ms", "other_ms",
            "signature_ms", "sort_ms", "surface_ms", "repack_ms", "hash_ms", "upload_ms",
            "palette_scratch_ms", "dispatch_ms", "cpu_fallback_ms", "rebuild_calls", "reason_mask",
            "admitted_columns", "surface_misses", "deferred_columns", "copied_bytes", "hashed_bytes",
            "uploaded_bytes", "scratch_bytes", "backlog", "path", "fallback", "instance_capacity",
            "generated_count", "count_generation",
            "terrain_work_ms", "terrain_overshoot_ms", "terrain_attempts", "terrain_adopted",
            "terrain_work_bytes", "terrain_byte_overshoot", "terrain_pending", "water_work_ms",
            "water_overshoot_ms", "water_attempts", "water_adopted", "water_work_bytes",
            "water_byte_overshoot", "water_pending",
        ])
        phase_names = re.findall(r'"([a-z_]+)"', block(self.collector, "kPhaseNames"))
        self.assertEqual(len(encoded.split(",")), 51)
        self.assertEqual(phase_names, [
            "ui_update", "ui_render", "ui_filters", "menu_scene", "preview_adoption",
            "document_load", "texture_load", "shader_load", "shader_first_use",
            "world_create_callback", "world_load_callback",
        ])
        water_columns = ["water_selection_ms", "water_seed_inputs_ms", "water_initialization_ms",
                         "water_sim_ms", "water_seam_ms", "water_bookkeeping_ms",
                         "water_precomputed_columns", "water_fallback_columns", "water_stale_columns"]
        worker_columns = ["worker_dispatched", "worker_completed",
                          "worker_stale_rejected", "worker_queue_age_frames"]
        columns = encoded.split(",") + [name + "_ms" for name in phase_names] + ["dirty_columns"] + water_columns + worker_columns + ["foliage_other_ms"]
        self.assertEqual(len(columns), 77)
        self.assertEqual(columns[62], "dirty_columns")
        self.assertEqual(columns[63:72], water_columns)
        self.assertEqual(columns[72:76], worker_columns)
        documentation = read(ROOT / "docs/render-benchmark.md")
        self.assertIn("The CSV has 77 fields.", documentation)
        phase_table = documentation.split("| Column | Work measured |", 1)[1].split("\n\n", 1)[0]
        phase_columns = re.findall(r"^\| `([a-z_]+)` \|", phase_table, re.M)
        self.assertEqual(phase_columns, [name + "_ms" for name in phase_names])
        documented = []
        for first, last, names in re.findall(r"^\| (\d+)(?:–(\d+))? \| (.*?) \|$",
                                              documentation, re.M):
            self.assertEqual(int(first), len(documented))
            if first == "51":
                self.assertEqual(last, "61")
                names = phase_columns
            else:
                names = re.findall(r"`([a-z_]+)`", names)
            self.assertEqual(len(names), int(last or first) - int(first) + 1)
            documented.extend(names)
        self.assertIn("| 62 | `dirty_columns` |", documentation)
        self.assertEqual(documented, columns)
        row = compact(block(self.collector, "bool write_csv_row("))
        self.assertIn("if (count_known) out << generated_count; out << ','; "
                      "if (count_known) out << count_generation;", row)
        self.assertTrue(row.endswith("for (const double value : phases.milliseconds()) "
                                    "out << ',' << value; "
                                    "out << ',' << foliage.dirty_columns; "
                                    "out << ',' << water.selection_ms << ',' << water.seed_inputs_ms << ',' "
                                    "<< water.initialization_ms << ',' << water.sim_ms << ',' << water.seam_ms << ',' "
                                    "<< water.bookkeeping_ms << ',' << water.precomputed_columns << ',' << water.fallback_columns "
                                    "<< ',' << water.stale_columns; "
                                    "out << ',' << foliage.worker_dispatched << ',' << foliage.worker_completed << ',' "
                                    "<< foliage.worker_stale_rejected << ',' << foliage.worker_queue_age_frames << ',' "
                                    "<< foliage.other(buckets.foliage) << '\\n'; "
                                    "return out.good();"))
        self.assertEqual(row.count("foliage.dirty_columns"), 1)

    def test_camera_transition_fixture_starts_with_an_attributed_baseline(self):
        native = read(ROOT / "test/rendering/foliage_rebuild_test.cpp")
        transition = compact(block(native, "TEST_F(FoliageRebuildTest, "
                                   "CameraReorderUploadsMetadataOnceAndPrefixReplacesEveryCommandWord)"))
        baseline, movement = transition.split("for (const glm::vec3 camera :", 1)
        self.assertIn("const auto initial = RebuildAttributed(reused.pass(), chunks, 1);", baseline)
        self.assertIn("EXPECT_EQ(initial.reasons, Attribution::Initial);", baseline)
        self.assertNotIn("Rebuild(reused.pass(), chunks, 1);", baseline)
        self.assertIn("glm::vec3{kCamera.x + 32, kCamera.y, kCamera.z}, kCamera", movement)
        self.assertIn("RebuildAttributed(reused.pass(), chunks, ++frame, camera)", movement)
        self.assertIn("EXPECT_EQ(work.reasons, Attribution::CameraCell);", movement)
        self.assertIn("for (const bool gpu : {false, true})", movement)
        self.assertIn("Rebuild(fresh.pass(), chunks, frame, camera);", movement)

    def test_legacy_hash_oracle_uses_the_callers_camera_and_preserves_the_default(self):
        rebuild = read(ROOT / "test/rendering/foliage_rebuild_test.cpp")
        self.assertIn('#include "foliage_rebuild_fixture.h"', rebuild)
        native = read(ROOT / "test/rendering/foliage_rebuild_fixture.h")
        for counter in ("uploaded_bytes", "uploads", "surface_upload_bytes", "surface_uploads"):
            self.assertIn(f"mutable std::size_t {counter} = 0;", rebuild)
        self.assertRegex(compact(native), r"LegacyUploadedHash\(const FoliagePass& pass, "
                         r"std::size_t count, const glm::vec3& camera = kCamera\)")
        oracle = compact(block(native, "static std::uint64_t LegacyUploadedHash("))
        self.assertIn("{camera.x, camera.z, pass.density_scale(), pass.fade_end_m(), total_weight}",
                      oracle)
        self.assertNotIn("kCamera", oracle)
        transition = compact(block(rebuild, "TEST_F(FoliageRebuildTest, "
                                   "CameraReorderUploadsMetadataOnceAndPrefixReplacesEveryCommandWord)"))
        self.assertIn("EXPECT_EQ(hash, LegacyUploadedHash(reused.pass(), chunks.size(), camera));",
                      transition)

    def test_csv_nullable_counts_precede_both_complete_upload_lanes(self):
        writer = compact(block(self.collector, "bool write_csv_row("))
        self.assertIn("if (count_known) out << generated_count; out << ','; "
                      "if (count_known) out << count_generation;", writer)
        self.assertLess(writer.index("if (count_known)"), writer.index("append_uploads("))
        self.assertIn("append_uploads(uploads.terrain, uploads.terrain_pending); "
                      "append_uploads(uploads.water, uploads.water_pending); "
                      "for (const double value : phases.milliseconds()) out << ',' << value; "
                      "out << ',' << foliage.dirty_columns;",
                      writer)
        append = compact(block(writer, "const auto append_uploads ="))
        self.assertEqual(append, "out << ',' << work.elapsed_ms << ',' << work.overshoot_ms "
                         "<< ',' << work.attempts << ',' << work.adopted << ',' << work.bytes "
                         "<< ',' << work.byte_overshoot << ',' << pending;")

    def test_deterministic_work_gates_are_registered_on_standard_targets(self):
        cmake = read(ROOT / "test/CMakeLists.txt")
        for target, source in (
            ("common_tests", "common/DeterministicWorkBudget_test.cpp"),
            ("pass_context_test", "rendering/foliage_rebuild_test.cpp"),
            ("pass_context_test", "rendering/foliage_invalidation_test.cpp"),
            ("foliage_ground_work_test", "rendering/foliage_ground_work_test.cpp"),
        ):
            sources = re.search(r"add_executable\(" + target + r"\s+(.*?)^\)",
                                cmake, re.S | re.M)
            self.assertIsNotNone(sources)
            self.assertIn(source, sources.group(1))
            roster = re.search(r"set\(LUMINUMBRA_GTEST_TARGETS\s+(.*?)^\)",
                               cmake, re.S | re.M)
            self.assertIsNotNone(roster)
            self.assertIn(target, roster.group(1).split())
        native = read(ROOT / "test/common/DeterministicWorkBudget_test.cpp")
        for name in ("EveryPinnedLimitRejectsAnInjectedExcess",
                     "IdleRejectsOneUploadByteAndOneHashByteIndependently",
                     "TerrainAndWaterUploadWorkAndProgressStayPinned",
                     "LiveSubsystemMovementEditAndDrainStayBounded"):
            self.assertIn("TEST(DeterministicWorkBudget, " + name + ")", native)
        for name in ("DeterministicWorkBudgetMovementEditDrainAndIdle",
                     "OutdatedRevisionReadbackIsDiscardedBeforeAdoption"):
            self.assertIn("TEST_F(FoliageRebuildTest, " + name + ")",
                          read(ROOT / "test/rendering/foliage_rebuild_test.cpp"))

    def test_reviewed_work_budget_data_has_integer_units_and_exact_idle_zero(self):
        data = json.loads(read(ROOT / "test/fixtures/deterministic-work-budgets.json"))
        self.assertEqual(data["schema"], "luminumbra.deterministic_work_budgets.v1")
        self.assertEqual(set(data["limits"]),
                         {"idle", "foliage", "simulation", "terrain_upload", "water_upload"})
        for limits in data["limits"].values():
            self.assertTrue(limits)
            for name, limit in limits.items():
                self.assertIs(type(limit), int)
                self.assertGreaterEqual(limit, 0)
                self.assertLess(limit, 2**64 - 1)
                self.assertNotRegex(name, r"(?i)ms|time|seconds|fps")
        self.assertEqual(data["limits"]["idle"]["uploaded_bytes"], 0)
        self.assertEqual(data["limits"]["idle"]["hashed_bytes"], 0)
        self.assertTrue(all(value == 0 for value in data["limits"]["idle"].values()))

    def test_recorder_finalization_failure_reaches_shutdown_error_path(self):
        finish = compact(block(self.collector, "bool FrameRecorder::finish()"))
        self.assertIn("return finalize_csv(m_out);", finish)
        failure = compact(block(self.main, "if (!frame_recorder.finish())"))
        self.assertIn('LUMINUMBRA_CORE_ERROR("Frame attribution CSV finalization failed");', failure)
        self.assertIn("if (exit_code == 0) exit_code = 1;", failure)
        self.assertLess(self.main.index("if (!frame_recorder.finish())"),
                        self.main.index('trace_shutdown("query_cleanup_started")'))
        self.assertIn("return exit_code;", self.main)


    def test_boot_seed_preparation_is_bounded_owned_and_owner_published(self):
        water = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        boot = compact(block(water, "void WaterSystem::initialize_water_chunks("))
        self.assertIn("kBootSeedBatch = 64", boot)
        self.assertIn("m_boot_settle_mode && m_job_system", boot)
        self.assertIn("std::min(kBootSeedBatch, pending.size() - begin)", boot)
        self.assertIn("pending.subspan(begin, count)", boot)
        self.assertIn("prepare_high_water_seed_inputs(to_init, parallel_boot, terrain_seed)", boot)
        self.assertIn("[task = std::move(task)] { task->compute(); }", boot)
        self.assertLess(boot.index("dispatch_batch(jobs)"), boot.index("wait(handle)"))
        self.assertLess(boot.index("jobs.clear()"), boot.index("wait(handle)"))
        self.assertLess(boot.index("fence->load(std::memory_order_acquire)"),
                        boot.index("seed_chunk_water("))
        self.assertLess(boot.index("inputs->ready_for(m_sim_resolution)"),
                        boot.index("seed_chunk_water("))
        task = compact(block(water, "struct BootWaterSeedTask"))
        self.assertIn("std::shared_ptr<const SHIELD_WorldSystem> sampler", task)
        self.assertNotRegex(task, r"Chunk\s*[*&]|WaterSystem\s*[*&]|\[this")
        self.assertNotIn("seed_chunk_water", task)
        release = compact(block(water, "~BootWaterSeedTask()"))
        self.assertLess(release.index("inputs.reset()"), release.index("memory_order_release"))
        self.assertLess(release.index("sampler.reset()"), release.index("memory_order_release"))
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        capture = compact(block(world, "SHIELD_WorldSystem::capture_water_seed_sampler() const"))
        self.assertIn("acquire_worldgen_sample_scope()", capture)
        self.assertIn("new SHIELD_WorldSystem(*this, WaterSeedSamplerTag{})", capture)
        constructor = compact(block(world, "SHIELD_WorldSystem::SHIELD_WorldSystem("
                                    "const SHIELD_WorldSystem& source"))
        self.assertIn("hydro_lock(source.m_hydro_mutex)", constructor)
        self.assertIn("m_hydro_cache = source.m_hydro_cache", constructor)
        self.assertIn("m_job_system(nullptr)", world)
        self.assertIn("m_params(source.m_params)", world)
        self.assertIn("m_seed(source.m_seed)", world)
        for node in ("terrain", "cave", "spaghetti", "worley", "island_mask",
                     "continentalness", "erosion", "peaks", "warp", "temperature",
                     "humidity", "river"):
            self.assertIn(f"m_{node}_generator(source.m_{node}_generator)", world)

        cmake = read(ROOT / "test/CMakeLists.txt")
        common = re.search(r"add_executable\(common_tests\s+(.*?)^\)", cmake, re.S | re.M)
        self.assertIsNotNone(common)
        self.assertIn("common/WaterSeedInputs_test.cpp", common.group(1))
        native = read(ROOT / "test/common/WaterSeedInputs_test.cpp")
        for name in ("BootMediumAndHighFallbackQueueBoundedInputsBeforeOwnerPublication",
                     "BootMixedReadyStaleAndLateInputsKeepSerialHashTrajectory",
                     "RejectedBootDispatchUsesOwnedInputsAndPausedBootDoesNothing",
                     "DetachedSamplerSurvivesSourceResetPrefetchAndDestruction",
                     "BoundedSamplerRefusesUnsupportedResolutionAndPartialTerrain",
                     "RejectedResolutionBootAndLiveKeepConfiguredGridAndInlineHash",
                     "UnpublishedPartialAndWrongResolutionInputsFallBackInBootAndLive",
                     "LakeDisabledBootAndLiveRetireInputsAndKeepInlineHash",
                     "InvalidSessionResolutionsLeaveTheConfiguredGridUnchanged",
                     "SavedUltraChunkRestoresAndMigratesWithoutSelectingUltraSession",
                     "BootSurfaceAndGenerationJobsRespectPresetAndResolutionEligibility",
                     "ReadinessRequiresPublishedSupportedAndMatchingResolution"):
            self.assertIn("TEST(WaterSeedInputs, " + name + ")", native)

    def test_water_seed_inputs_require_supported_preset_and_matching_publication(self):
        header = read(ROOT / "src/luminumbra_common/core/WaterSeedInputs.h")
        self.assertIn("static constexpr bool supports_resolution(", header)
        self.assertIn("bool ready_for(", header)
        types = read(ROOT / "include/luminumbra/core/Types.h")
        constants = {name: int(re.search(r"\b" + name + r"\s*=\s*(\d+)", text).group(1))
                     for name, text in (("kResolution", header), ("CHUNK_SIZE_X", types),
                                        ("CHUNK_SIZE_Z", types))}
        supported = compact(block(header, "static constexpr bool supports_resolution("))
        supported = re.fullmatch(r"return (.*);", supported).group(1)
        readiness = compact(block(header, "bool ready_for("))
        readiness = re.fullmatch(r"return (.*);", readiness).group(1)
        readiness = readiness.replace("supports_resolution(cells_per_side)", "supported")
        readiness = readiness.replace("ready.load(std::memory_order_acquire)", "published")
        for requested in (-1, 0, 1, 2, 3, 4, 8, 16, 17, 32, 2**31 - 1):
            accepts = cpp_expression(supported, cells_per_side=requested, **constants)
            self.assertEqual(accepts, requested in (2, 4, 8, 16))
            for published in (False, True):
                for resolution in (0, 2, 4, 8, 16, 32):
                    self.assertEqual(
                        cpp_expression(readiness, supported=accepts, published=published,
                                       resolution=resolution, cells_per_side=requested,
                                       true=True, false=False),
                        accepts and published and resolution == requested,
                    )
        # The empty default span is outside the function body.
        compute = compact(block(header.replace("std::span<const float> terrain = {}",
                                               "std::span<const float> terrain"),
                                "void ComputeWaterSeedInputs("))
        self.assertIn("if (!HighWaterSeedInputs::supports_resolution(resolution)) return;", compute)
        self.assertIn("if (!terrain.empty() && terrain.size() != cells) return;", compute)
        self.assertLess(compute.index("inputs.resolution = resolution;"),
                        compute.index("inputs.ready.store(true, std::memory_order_release);"))

        water = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        boot = compact(block(water, "void WaterSystem::initialize_water_chunks("))
        eligibility = re.search(r"const bool use_seed_inputs = (.*?);", boot).group(1)
        eligibility = eligibility.replace(
            "HighWaterSeedInputs::supports_resolution(m_sim_resolution)", "supported")
        for accepts in (False, True):
            for lakes in (False, True):
                self.assertEqual(
                    cpp_expression(eligibility, supported=accepts,
                                   **{"m_shield_system->get_params().lakes_enabled": lakes}),
                    accepts and lakes,
                )
        self.assertIn("const bool parallel_boot = use_seed_inputs && "
                      "m_boot_settle_mode && m_job_system;", boot)
        self.assertIn("auto high_seed_inputs = use_seed_inputs ? "
                      "prepare_high_water_seed_inputs(to_init, parallel_boot, terrain_seed) : "
                      "std::vector<std::shared_ptr<HighWaterSeedInputs>>{};", boot)
        parallel = compact(block(water, "if (parallel_boot)"))
        self.assertIn("std::make_shared<HighWaterSeedInputs>()", parallel)
        self.assertIn("ComputeWaterSeedInputs(*inputs,", parallel)
        self.assertIn("if (!use_seed_inputs || m_sim_resolution != "
                      "HighWaterSeedInputs::kResolution) "
                      "to_init[i]->stage_water_seed_inputs({});", boot)
        self.assertIn("seed_chunk_water(to_init[i], terrain_seed[i], "
                      "inputs && inputs->ready_for(m_sim_resolution) ? inputs : nullptr);", boot)
        live = compact(block(water, "WaterSystem::prepare_high_water_seed_inputs("))
        self.assertIn("if (inputs && inputs->ready_for(m_sim_resolution))", live)
        consumer = compact(block(water, "void WaterSystem::seed_chunk_water("))
        guard = "if (inputs && !inputs->ready_for(initial_resolution)) inputs = nullptr;"
        self.assertIn(guard, consumer)
        self.assertLess(consumer.index(guard), consumer.index("inputs->rest[cell]"))
        self.assertLess(consumer.index(guard), consumer.index("inputs->terrain[cell]"))
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        request = compact(block(world, "SHIELD_WorldSystem::request_water_seed_inputs("))
        self.assertIn("!m_params.lakes_enabled", request)
        self.assertIn("m_water_system->sim_resolution() != HighWaterSeedInputs::kResolution", request)
        self.assertLess(request.index("return {};"),
                        request.index("std::make_shared<HighWaterSeedInputs>()"))

        native = read(ROOT / "test/common/WaterSeedInputs_test.cpp")
        regression = compact(block(native, "TEST(WaterSeedInputs, "
                                   "RejectedResolutionBootAndLiveKeepConfiguredGridAndInlineHash)"))
        self.assertIn("for (const int resolution : {2, 4, 8, 16})", regression)
        self.assertIn("for (const bool boot : {false, true})", regression)
        self.assertIn("accelerated.water().SetBootSettleMode(boot);", regression)
        self.assertIn("accelerated.water().SetSimResolution(32);", regression)
        self.assertIn("inputs->resolution = 32;", regression)
        for fixture in ("accelerated", "inline_path"):
            self.assertIn(f"{fixture}.water().SetSimResolution(resolution);", regression)
            self.assertIn(f"ASSERT_EQ({fixture}.water().sim_resolution(), resolution);", regression)
            self.assertIn(f"ExpectInlineSeed({fixture}, resolution);", regression)
        self.assertIn("ExpectSameWater(accelerated, inline_path);", regression)
        oracle = compact(block(native, "void ExpectInlineSeed("))
        self.assertIn("EXPECT_EQ(fixture.world().debug_water_state_hash().hash, expected_hash);", oracle)

    def test_water_resolution_setter_retains_integral_cell_contract(self):
        water = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        setter = block(water, "void WaterSystem::SetSimResolution(")
        # Pin the complete mutation/refusal path, including short circuiting before
        # modulo zero. This is the original contract for every possible int value.
        self.assertEqual(compact(re.sub(r"//[^\n]*", "", setter)),
                         "if (cells_per_side <= 1 || CHUNK_SIZE_X % cells_per_side != 0 || "
                         "CHUNK_SIZE_Z % cells_per_side != 0) { return; } "
                         "m_sim_resolution = cells_per_side;")
        refusal = re.search(r"if \((.*?)\) \{", compact(setter)).group(1)
        types = read(ROOT / "include/luminumbra/core/Types.h")
        constants = {name: int(re.search(r"\b" + name + r"\s*=\s*(\d+)", types).group(1))
                     for name in ("CHUNK_SIZE_X", "CHUNK_SIZE_Z")}
        for requested in (-(2**31), -1, *range(66), 2**31 - 1):
            for configured in (2, 4, 8, 16):
                rejected = cpp_expression(refusal, cells_per_side=requested, **constants)
                actual = configured if rejected else requested
                self.assertEqual(actual, requested if requested in (2, 4, 8, 16) else configured)

    def test_water_seed_defensive_predicates_directly_refuse_unreachable_grids(self):
        # Use the existing scalar production-expression seam: no setter bypass or
        # runtime route to an unsupported session grid is needed. This evaluates
        # the actual consumer guard, independently of upstream input filtering.
        header = read(ROOT / "src/luminumbra_common/core/WaterSeedInputs.h")
        types = read(ROOT / "include/luminumbra/core/Types.h")
        constants = {name: int(re.search(r"\b" + name + r"\s*=\s*(\d+)", text).group(1))
                     for name, text in (("kResolution", header), ("CHUNK_SIZE_X", types),
                                        ("CHUNK_SIZE_Z", types))}
        supported = re.fullmatch(r"return (.*);", compact(block(
            header, "static constexpr bool supports_resolution("))).group(1)
        readiness = re.fullmatch(r"return (.*);", compact(block(
            header, "bool ready_for("))).group(1)
        readiness = readiness.replace("supports_resolution(cells_per_side)", "supported")
        readiness = readiness.replace("ready.load(std::memory_order_acquire)", "published")
        water = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        initialize = compact(block(water, "void WaterSystem::initialize_water_chunks("))
        eligibility = re.search(r"const bool use_seed_inputs = (.*?);", initialize).group(1)
        eligibility = eligibility.replace(
            "HighWaterSeedInputs::supports_resolution(m_sim_resolution)", "supported")
        consumer = compact(block(water, "void WaterSystem::seed_chunk_water("))
        guard_match = re.search(r"if \((.*?)\) inputs = nullptr;", consumer)
        self.assertIsNotNone(guard_match, "consumer must independently reject invalid payloads")
        guard = guard_match.group(1)
        guard = guard.replace("inputs->ready_for(initial_resolution)", "ready_for")
        # Both payload reads must remain behind the guard.
        for field in ("rest", "terrain"):
            self.assertLess(consumer.index("inputs = nullptr;"),
                            consumer.index(f"inputs->{field}[cell]"))
        for requested in (-(2**31), -1, 0, 1, 2, 3, 4, 8, 16, 17, 32, 2**31 - 1):
            accepts = cpp_expression(supported, cells_per_side=requested, **constants)
            for lakes in (False, True):
                eligible = cpp_expression(
                    eligibility, supported=accepts, true=True, false=False,
                    **{"m_shield_system->get_params().lakes_enabled": lakes})
                self.assertEqual(eligible, lakes and requested in (2, 4, 8, 16))
            for present in (False, True):
                for published in (False, True):
                    # Zero metadata covers partial/uncomputed and unknown payloads;
                    # the other values cover matching and mismatched publications.
                    for resolution in (0, 2, 4, 8, 16, 32):
                        ready = cpp_expression(
                            readiness, supported=accepts, published=published,
                            resolution=resolution, cells_per_side=requested, true=True, false=False)
                        discarded = cpp_expression(guard, inputs=present, ready_for=ready,
                                                   true=True, false=False)
                        consumes = present and not discarded
                        self.assertEqual(consumes, present and published and
                                         requested in (2, 4, 8, 16) and resolution == requested)


class SkySunProjectionWiring(unittest.TestCase):
    def test_capture_projects_the_resolved_rendered_direction(self):
        source = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        projection = re.search(r"const bool sun_on_screen = ProjectDirectionToScreen\((.*?);",
                               source, re.S)
        self.assertIsNotNone(projection)
        self.assertIn("-renderPipeline.sun_direction()", projection[1])
        self.assertNotIn("TowardSunDirection(", projection[1])
        sky_pass = read(CLIENT / "rendering/passes/SkyboxPass.cpp")
        self.assertIn("const glm::vec3 toward_sun = -ctx.sun.direction;", sky_pass)
        self.assertIn('setVec3("u_sunDirection", toward_sun)', sky_pass)
        self.assertIn('setFloat("u_sunCosZenith", toward_sun.y)', compact(sky_pass))
        aiming = block(read(CLIENT / "core/scenarios/SkyboxVisual.cpp"),
                       "Luminumbra::Vec3 TowardSunDirection(")
        self.assertIn("ComputeSunGeometry(time_of_day, 0.0f)", aiming)
        self.assertNotIn("std::sin", aiming)
        self.assertNotIn("std::cos", aiming)


class CloudRenderingWiring(unittest.TestCase):
    """Exercise scalar shader policy and its shipping pass bindings without GL."""

    @staticmethod
    def scalar(expression, **values):
        def smoothstep(lo, hi, value):
            t = max(0.0, min(1.0, (value - lo) / (hi - lo)))
            return t * t * (3.0 - 2.0 * t)

        tree = ast.parse(compact(expression), mode="eval")
        allowed = (ast.Expression, ast.Call, ast.Name, ast.Load, ast.Constant,
                   ast.BinOp, ast.Add, ast.Sub, ast.Mult, ast.Div, ast.UnaryOp, ast.USub)
        if any(not isinstance(node, allowed) for node in ast.walk(tree)):
            raise AssertionError("Unsupported cloud scalar expression")
        values.update(smoothstep=smoothstep, mix=lambda a, b, t: a * (1.0 - t) + b * t)
        return eval(compile(tree, "<cloud scalar>", "eval"), {"__builtins__": {}}, values)

    def test_cloud_horizon_is_continuous_for_clear_and_storm(self):
        source = read(ROOT / "res/shaders/enhanced_skybox.frag")
        fade = block(source, "float cloudHorizonFade(")
        expression = re.search(r"return (.*?);", fade, re.S).group(1)
        march = block(source, "vec3 renderClouds(")
        self.assertIn("if (viewDir.y <= 0.0) return baseColor;", march)
        self.assertIn("cloudHorizonFade(viewDir.y, structureWeight)", march)
        for structure in (0.0, 0.5, 1.0):
            def sample(elevation):
                return self.scalar(expression, elevation=elevation, structureWeight=structure)
            self.assertEqual(sample(-0.01), 0.0)
            self.assertEqual(sample(0.0), 0.0)
            self.assertLess(sample(0.00001), 1e-6)
            self.assertLess(abs(sample(0.020001) - sample(0.019999)), 0.0001)
            self.assertEqual(sample(1.0), 1.0)

    def test_cloud_volume_has_independent_height_and_advects_both_scales(self):
        source = read(ROOT / "res/shaders/enhanced_skybox.frag")
        noise = compact(block(source, "float cloudNoise3D("))
        self.assertIn("vec3 cell = floor(p)", noise)
        self.assertEqual(noise.count("hash("), 8)
        self.assertEqual(noise.count("f.x)"), 4)
        self.assertEqual(noise.count("f.z)"), 2)
        self.assertIn("return mix(a, b, f.y)", noise)
        density = block(source, "float cloudDensity(")
        self.assertIn("advected = p + vec3(u_cloudScrollOffset.x, 0.0, u_cloudScrollOffset.y)", density)
        self.assertIn("cloudNoise3D(advected * (1.0 / 430.0))", density)
        self.assertIn("cloudNoise3D(advected * (1.0 / 120.0))", density)
        self.assertNotIn("vec2(p.y", density)
        profile = re.search(r"float profile = (.*?);", density, re.S).group(1)
        base = re.search(r"float base = (.*?);", density, re.S).group(1)
        for shape in (0.0, 0.5, 1.0):
            height = self.scalar(base, shape=shape)
            for hf in (-0.1, 0.0, 1.0, 1.1):
                self.assertEqual(self.scalar(profile, base=height, hf=hf), 0.0)
            self.assertGreater(self.scalar(profile, base=height, hf=0.49), 0.99)
        march = compact(block(source, "vec3 renderClouds("))
        self.assertIn("float jitter = hash(gl_FragCoord.xy)", march)
        self.assertIn("float lightDt = (cloudSlabTop() - cloudSlabBottom()) / float(kLightSteps)", march)
        self.assertIn("kSteps = 28", march)
        self.assertIn("kLightSteps = 5", march)

    def test_cloud_opacity_is_applied_once_and_empty_volume_preserves_sky(self):
        source = read(ROOT / "res/shaders/enhanced_skybox.frag")
        march = compact(block(source, "vec3 renderClouds("))
        self.assertIn("vec3 stepColor = sunTerm + ambient;", march)
        self.assertIn("scatter += transmittance * stepColor * (1.0 - stepExt)", march)
        self.assertIn("vec3 cloudColor = scatter / max(1.0 - transmittance, 1e-4)", march)
        self.assertIn("return mix(baseColor, cloudColor, alpha)", march)
        self.assertIn("float alpha = clamp((1.0 - transmittance) * horizonFade, 0.0, 1.0)", march)

    def test_night_clouds_receive_no_sunlight_or_warm_power_grade(self):
        source = read(ROOT / "res/shaders/enhanced_skybox.frag")
        march = block(source, "vec3 renderClouds(")
        expression = re.search(r"float sunlight = (.*?);", march, re.S).group(1)
        for elevation in (-1.0, -0.1, -0.08):
            self.assertEqual(self.scalar(expression, u_sunCosZenith=elevation), 0.0)
        for elevation in (0.06, 0.5, 1.0):
            self.assertEqual(self.scalar(expression, u_sunCosZenith=elevation), 1.0)
        self.assertIn("sunTransmittance(u_sunCosZenith) * (2.4 * sunlight)", march)
        self.assertIn("mix(vec3(0.025, 0.028, 0.035), vec3(0.42, 0.47, 0.58), dayFactor)", march)
        self.assertNotIn("cloudColor * 0.12", march)
        self.assertIn("pow(graded, mix(vec3(1.0), vec3(0.9, 0.95, 1.05), dayFactor))", source)

    def test_taau_sky_bypass_precedes_history_and_sharpen_in_live_and_replay(self):
        shader = read(ROOT / "res/shaders/taau_resolve.frag")
        main = block(shader, "void main()")
        early = block(main, "if (u_history_valid == 0 || skyDepth <= 0.0)")
        self.assertIn("FragColor = vec4(curr, 1.0)", early)
        self.assertIn("return;", early)
        self.assertLess(main.index("texture(u_sceneDepth"), main.index("texture(u_history"))
        self.assertLess(main.index("texture(u_sceneDepth"), main.index("vec3 mean"))
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        context = compact(block(pipeline, "RenderContext RenderPipeline::make_taau_context()"))
        self.assertRegex(context, r'ctx\.gbuffer_depth = m_render_registry\.adopt_texture\(\s*'
                                  r'"gbuffer_depth", m_gbuffer_pass->gbuffer\(\)\.depth_texture\)')
        stage = block(pipeline, "void RenderPipeline::execute_stage_taau_resolve(")
        self.assertIn("make_taau_context()", stage)
        self.assertIn("m_far_world_paired ? &m_far_world_pair_taau : nullptr", stage)
        execute = compact(block(read(CLIENT / "rendering/passes/TaauPass.cpp"), "void TaauPass::execute("))
        self.assertIn("glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id)", execute)
        self.assertIn('m_shader->setInt("u_sceneDepth", 3)', execute)

    def test_taau_only_interior_sky_bypasses_accumulation(self):
        main = block(read(ROOT / "res/shaders/taau_resolve.frag"), "void main()")
        self.assertIn("if (skyDepth <= 0.0)", main)
        scan = block(main, "if (skyDepth <= 0.0)")
        self.assertIn("float skyDepth = texture(u_sceneDepth, TexCoords).r;", main)
        rows = block(scan, "for (int dy = -1; dy <= 1; ++dy)")
        samples = compact(block(rows, "for (int dx = -1; dx <= 1; ++dx)"))
        self.assertEqual(samples,
                         "vec2 depthUV = clamp(TexCoords + vec2(float(dx), float(dy)) * u_texel, "
                         "vec2(0.0), vec2(1.0)); "
                         "skyDepth = max(skyDepth, texture(u_sceneDepth, depthUV).r);")
        bypass = re.search(r"if \((u_history_valid == 0 \|\| [^\n]+)\)", main).group(1)
        early = block(main, "if (" + bypass + ")")
        self.assertEqual(compact(early), "FragColor = vec4(curr, 1.0); return;")
        self.assertLess(main.index(scan), main.index(early))
        # Evaluate the production bypass after the pinned max-depth reduction.
        # Cover every neighbour (including diagonals), tiny positive depths, and
        # border replication for a one-texel image and all four image corners.
        grids = [("interior", [[0.0] * 3 for _ in range(3)], 1, 1, True),
                 ("one sky texel", [[0.0]], 0, 0, True),
                 ("one geometry texel", [[1.0]], 0, 0, False)]
        for y in range(3):
            for x in range(3):
                for depth in (1e-7, 1.0):
                    grid = [[0.0] * 3 for _ in range(3)]
                    grid[y][x] = depth
                    grids.append((f"geometry {x},{y} depth {depth}", grid, 1, 1, False))
        for x, y in ((0, 0), (2, 0), (0, 2), (2, 2)):
            clear = [[0.0] * 3 for _ in range(3)]
            edge = [[0.0] * 3 for _ in range(3)]
            edge[1][1] = 0.5
            grids.extend((("clear corner", clear, x, y, True),
                          ("edge corner", edge, x, y, False)))
        for name, grid, x, y, interior in grids:
            for valid in (0, 1):
                with self.subTest(case=name, history_valid=valid):
                    depths = [grid[max(0, min(len(grid) - 1, y + dy))]
                                  [max(0, min(len(grid[0]) - 1, x + dx))]
                              for dy in range(-1, 2) for dx in range(-1, 2)]
                    skipped = cpp_expression(bypass, u_history_valid=valid, skyDepth=max(depths))
                    self.assertEqual(skipped, not valid or interior)
        # Once the bypass falls through, in-bounds edge history must reach both
        # the existing clamp and blend without another unconditional early exit.
        accumulation = main[main.index(early) + len(early) + 1:]
        offscreen = re.search(r"if \((histUV.x < .*?)\)\s*\{", accumulation).group(1)
        self.assertFalse(cpp_expression(offscreen, **{"histUV.x": 0.5, "histUV.y": 0.5}))
        for x, y in ((-0.01, 0.5), (1.01, 0.5), (0.5, -0.01), (0.5, 1.01)):
            self.assertTrue(cpp_expression(offscreen, **{"histUV.x": x, "histUV.y": y}))
        offscreen_body = block(accumulation, "if (" + offscreen + ")")
        self.assertEqual(accumulation.count("return;"), offscreen_body.count("return;"))
        self.assertLess(accumulation.index("hist = clamp(hist, nmin, nmax);"),
                        accumulation.index("vec3 resolved = mix(curr, hist, blend);"))

    def test_taau_copy_uses_screen_source_and_internal_destination(self):
        execute = block(read(CLIENT / "rendering/passes/TaauPass.cpp"), "void TaauPass::execute(")
        copies = re.findall(r"glBlitFramebuffer\((.*?)\);", execute, re.S)
        self.assertEqual(len(copies), 1)
        arguments = [compact(arg) for arg in copies[0].split(",")]
        self.assertEqual(arguments[8:], ["GL_COLOR_BUFFER_BIT", "GL_NEAREST"])
        self.assertIn("glViewport(0, 0, ctx.screen_width, ctx.screen_height);", execute)
        self.assertIn("glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo);", execute)
        self.assertIn("glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lit_fbo);", execute)
        for width, height, internal_w, internal_h in (
                (1920, 1080, 960, 540), (1919, 1079, 960, 540),
                (1920, 1080, 1920, 1080), (1, 1, 1, 1), (1920, 1080, 0, 0)):
            with self.subTest(screen=(width, height), internal=(internal_w, internal_h)):
                dest_w, dest_h = internal_w or width, internal_h or height
                values = {"ctx.screen_width": width, "ctx.screen_height": height,
                          "ctx.internal_w()": dest_w, "ctx.internal_h()": dest_h}
                rectangle = [cpp_expression(arg, **values) for arg in arguments[:8]]
                self.assertEqual(rectangle, [0, 0, width, height, 0, 0, dest_w, dest_h])

class StormLightningWiring(unittest.TestCase):
    def test_every_scenario_projects_world_geometry_without_screen_remapping(self):
        sweep = read(CLIENT / "core/scenarios/WorldVisualSweep.cpp")
        drive = read(CLIENT / "core/ScenarioRunnerDrive.cpp")
        callers = (sweep, block(drive, "bool ScenarioRunnerImpl::driveWeatherVisual()"),
                   block(drive, "bool ScenarioRunnerImpl::drivePrecipitation()"))
        for caller in callers:
            with self.subTest(caller=caller[:80]):
                self.assertIn("ProjectLightningBolt(", caller)
                self.assertIn("ComposeLiveStrikes(", caller)
                self.assertIn("kMaxBoltSegmentPoints", caller)
                self.assertIn("LiveBoltViewMargin(", caller)
                self.assertNotIn("map_point", caller)
                self.assertNotIn("cloud_darkness", caller)
                self.assertNotIn("cloud_anchor_ndc", caller)
        weather = block(sweep, "const auto apply_weather")
        self.assertIn("glm::vec3(anchor) + glm::vec3(150.0f, 0.0f, 0.0f)", weather)
        self.assertNotIn("camera->Front", weather)
        self.assertNotIn("camera->Position) +", weather)
        self.assertIn("cstate.coverage_amount = 0.85f", weather)
        for caller in callers[1:]:
            self.assertIn("gameSession->GetMetadata().spawnPoint", caller)
            self.assertIn("TowardSunDirection(0.04f)", caller)
            self.assertIn("GetProjectionMatrix(std::max(1, width), std::max(1, height))", caller)
        self.assertIn("world ? world->GetTerrainHeightAt(strike_xz.x, strike_xz.z) : spawn.y",
                      compact(callers[1]))
        for caller in (weather, callers[2]):
            self.assertIn("std::abs(projected.strike_ndc.x) <= 1.0f", caller)
            self.assertIn("std::abs(projected.strike_ndc.y) <= 1.0f", caller)
            self.assertIn("ground_on_screen ?", caller)
            guard = re.search(r"const bool ground_on_screen =\s*(.*?);", caller, re.S)[1]
            guard = guard.replace("Luminumbra::Rendering::", "").replace("1.0f", "1.0")
            for x in (-1.1, -1.0, 0.0, 1.0, 1.1):
                for y in (-1.1, -1.0, 0.0, 1.0, 1.1):
                    with self.subTest(x=x, y=y):
                        visible = cpp_expression(guard, **{
                            "IsLiveBoltPenUp(projected.strike_ndc)": False,
                            "std::abs(projected.strike_ndc.x)": abs(x),
                            "std::abs(projected.strike_ndc.y)": abs(y)})
                        self.assertEqual(visible, abs(x) <= 1.0 and abs(y) <= 1.0)
            self.assertFalse(cpp_expression(guard, **{
                "IsLiveBoltPenUp(projected.strike_ndc)": True,
                "std::abs(projected.strike_ndc.x)": 3.0,
                "std::abs(projected.strike_ndc.y)": 3.0}))

    def test_projection_preserves_offscreen_coordinates_and_breaks_invalid_strokes(self):
        source = read(CLIENT / "rendering/LightningBolt.h")
        self.assertIn("inline ProjectedLiveStrike ProjectLightningBolt(", source)
        project = block(source, "inline ProjectedLiveStrike ProjectLightningBolt(")
        self.assertIn("view_projection * glm::vec4(point, 1.0f)", project)
        self.assertIn("const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);", project)
        self.assertNotIn("clamp", project)
        live = block(read(CLIENT / "main_client.cpp"), "const auto project =")
        for projection, sentinel_return in (
                (project, "return kLiveBoltPenUp;"),
                (live, "return Luminumbra::Rendering::kLiveBoltPenUp;")):
            predicate = re.search(r"if \(([^\n]+)\) \{", projection)
            self.assertIsNotNone(predicate)
            self.assertIn("!std::isfinite(clip.w)", predicate[1])
            for w, accepted in ((-1, False), (0, False), (float("nan"), False),
                                (float("inf"), False), (-float("inf"), False),
                                (0.001, True), (10, True)):
                with self.subTest(w=w):
                    expression = predicate[1].replace("0.0f", "0.0").replace(
                        "std::isfinite(clip.w)", "true" if math.isfinite(w) else "false")
                    self.assertEqual(not cpp_expression(expression, true=True, false=False,
                                                        **{"clip.w": w}), accepted)
            invalid = block(projection, "if (!std::isfinite(ndc.x)")
            self.assertIn(sentinel_return, invalid)
            predicate = re.search(r"if \((!std::isfinite\(ndc.x\)[^\n]+)\) \{", projection)[1]
            for x in (0.0, 2.0, float("nan"), float("inf")):
                for y in (0.0, -2.0, float("nan"), -float("inf")):
                    with self.subTest(x=x, y=y):
                        expression = predicate.replace(
                            "std::isfinite(ndc.x)", "true" if math.isfinite(x) else "false").replace(
                            "std::isfinite(ndc.y)", "true" if math.isfinite(y) else "false")
                        self.assertEqual(cpp_expression(expression, true=True, false=False),
                                         not (math.isfinite(x) and math.isfinite(y)))
        self.assertIn("project_stroke(bolt.main_channel)", project)
        self.assertIn("project_stroke(branch)", project)
        self.assertIn("project(bolt.ground_point)", project)

    def test_flash_has_finite_support_and_scales_with_scene_luminance(self):
        shader = read(ROOT / "res/shaders/lightning_overlay.frag")
        self.assertIn("float flashResponse(", shader)
        response = block(shader, "float flashResponse(")
        expression = re.search(r"return\s+([^;]+);", response)[1]

        def clamp(value, low, high):
            return max(low, min(high, value))

        def smoothstep(low, high, value):
            fraction = clamp((value - low) / (high - low), 0.0, 1.0)
            return fraction * fraction * (3.0 - 2.0 * fraction)

        tree = ast.parse(expression, mode="eval")
        allowed = (ast.Expression, ast.BinOp, ast.Add, ast.Sub, ast.Mult, ast.Call,
                   ast.Name, ast.Load, ast.Constant)
        self.assertTrue(all(isinstance(node, allowed) for node in ast.walk(tree)))
        self.assertTrue(all(node.func.id in ("smoothstep", "clamp")
                            for node in ast.walk(tree) if isinstance(node, ast.Call)))
        code = compile(tree, "<shader flash response>", "eval")

        def evaluate(distance, luma):
            return eval(code, {"__builtins__": {}}, dict(
                smoothstep=smoothstep, clamp=clamp,
                distanceToStrike=distance, sceneLuma=luma))

        for distance in (0.0, 0.8, 1.6, 4.0):
            for luma in (0.0, 0.001, 0.01, 0.1, 1.0):
                with self.subTest(distance=distance, luma=luma):
                    value = evaluate(distance, luma)
                    self.assertTrue(math.isfinite(value))
                    self.assertGreaterEqual(value, 0.0)
                    self.assertLessEqual(value, 1.0)
                    if distance >= 1.6 or luma == 0.0:
                        self.assertEqual(value, 0.0)
        self.assertAlmostEqual(evaluate(0.8, 0.01), 10 * evaluate(0.8, 0.001))
        self.assertGreater(evaluate(0.0, 0.1), evaluate(0.8, 0.1))
        main = block(shader, "void main()")
        self.assertIn("length((ndc - centre) * vec2(u_aspect, 1.0))", main)
        self.assertIn("dot(color, vec3(0.2126, 0.7152, 0.0722))", main)
        self.assertIn("flashResponse(distanceToStrike, sceneLuma)", main)
        self.assertIn("flash = max(flash, u_flashPulse[i] * response);", main)
        self.assertIn("color += u_color * flash;", main)
        self.assertIn("if (u_active == 1 && u_pulse > 0.0)", main)
        self.assertNotIn("u_cloud", shader)
        self.assertNotIn("fbm(", shader)
        lighting = read(CLIENT / "rendering/passes/LightingPass.cpp")
        self.assertNotIn('setVec2("u_cloudNdc"', lighting)
        self.assertNotIn('setFloat("u_cloudDark"', lighting)

    def test_offscreen_strongest_strike_keeps_a_bounded_scene_flash(self):
        shader = read(ROOT / "res/shaders/lightning_overlay.frag")
        self.assertIn("float offscreenFlashFloor(", shader)

        def clamp(value, low, high):
            return max(low, min(high, value))

        def smoothstep(low, high, value):
            fraction = clamp((value - low) / (high - low), 0.0, 1.0)
            return fraction * fraction * (3.0 - 2.0 * fraction)

        def compiled(signature):
            expression = re.search(r"return\s+([^;]+);", block(shader, signature))[1]
            tree = ast.parse(expression, mode="eval")
            allowed = (ast.Expression, ast.BinOp, ast.Add, ast.Sub, ast.Mult, ast.Call,
                       ast.Name, ast.Load, ast.Constant)
            self.assertTrue(all(isinstance(node, allowed) for node in ast.walk(tree)))
            return compile(tree, "<shader " + signature + ">", "eval")

        response = compiled("float flashResponse(")
        floor = compiled("float offscreenFlashFloor(")
        names = dict(smoothstep=smoothstep, clamp=clamp)

        def flash(strike_ndc, pixel_ndc, luma, aspect=1.777):
            distance = math.hypot((pixel_ndc[0] - strike_ndc[0]) * aspect,
                                  pixel_ndc[1] - strike_ndc[1])
            extent = max(abs(strike_ndc[0]), abs(strike_ndc[1]))
            local = eval(response, {"__builtins__": {}},
                         dict(names, distanceToStrike=distance, sceneLuma=luma))
            ambient = eval(floor, {"__builtins__": {}},
                           dict(names, strikeExtent=extent, sceneLuma=luma))
            if strike_ndc == (-3.0, -3.0):
                local = 0.0
            if extent <= 1.0:
                ambient = 0.0
            return max(local, ambient)

        pen_up = (-3.0, -3.0)
        visible_bolt = (0.6, 0.2)
        for luma in (0.001, 0.05, 0.5):
            with self.subTest(luma=luma):
                self.assertGreater(flash(pen_up, visible_bolt, luma), 0.0)
                self.assertGreater(flash(pen_up, (-0.9, 0.9), luma), 0.0)
                self.assertLessEqual(flash(pen_up, visible_bolt, luma), 0.35)
                self.assertGreater(flash((5.0, 0.0), (-1.0, 0.0), luma), 0.0)
        for extent in (0.0, 0.5, 1.0):
            with self.subTest(extent=extent):
                self.assertEqual(eval(floor, {"__builtins__": {}},
                                      dict(names, strikeExtent=extent, sceneLuma=1.0)), 0.0)
        self.assertEqual(flash(pen_up, visible_bolt, 0.0), 0.0)
        previous = 0.0
        for step in range(0, 41):
            extent = 0.9 + step * 0.01
            value = eval(floor, {"__builtins__": {}},
                         dict(names, strikeExtent=extent, sceneLuma=1.0))
            self.assertGreaterEqual(value, previous)
            self.assertLessEqual(value - previous, 0.05)
            previous = value
        main = block(shader, "void main()")
        self.assertIn("max(abs(centre.x), abs(centre.y))", main)
        offscreen = block(main, "if (u_flashOnscreen[i] == 0)")
        self.assertIn("offscreenFlashFloor(strikeExtent, sceneLuma)", offscreen)
        local = block(main, "if (!all(equal(centre, kLiveBoltPenUp)))")
        self.assertIn("response = flashResponse(distanceToStrike, sceneLuma);", local)
        self.assertNotIn("flashResponse(", offscreen)
        self.assertNotIn("offscreenFlashFloor(", local)
        self.assertEqual(main.count("offscreenFlashFloor("), 1)
        self.assertIn("if (i >= u_flashCount) break;", main)
        self.assertIn("for (int i = 0; i < MAX_STRIKE_FLASHES; ++i)", main)
        update = re.search(r"flash = (max\(flash, u_flashPulse\[i\] \* response\));", main)[1]
        update = update.replace("u_flashPulse[i]", "pulse")
        code = compile(ast.parse(update, mode="eval"), "<shader flash max>", "eval")

        def combined(strikes, pixel, luma, aspect=1.777):
            value = 0.0
            for centre, pulse in strikes:
                value = eval(code, {"__builtins__": {}}, dict(
                    max=max, flash=value, pulse=pulse,
                    response=flash(centre, pixel, luma, aspect)))
            return value

        # A visible weaker strike retains its full localized response, even when
        # a stronger behind-camera strike contributes the offscreen floor.
        strikes = ((pen_up, 0.9), (visible_bolt, 0.4))
        self.assertEqual(combined(strikes, visible_bolt, 0.5), 0.4)
        self.assertEqual(combined(tuple(reversed(strikes)), visible_bolt, 0.5), 0.4)
        self.assertAlmostEqual(combined(strikes, (-0.9, 0.9), 0.5), 0.9 * 0.35)
        # A behind-camera sentinel receives only the bounded floor.
        self.assertEqual(flash(pen_up, (-1.0, -1.0), 1.0, 0.01), 0.35)
        # Two onscreen centres each win at their own pixel; overlapping flashes use
        # a max rather than a sum. The old strongest-only result is zero on the left.
        pair = (((-0.8, 0.0), 0.2), ((0.8, 0.0), 0.4))
        self.assertEqual(combined(pair, (-0.8, 0.0), 0.5), 0.2)
        self.assertEqual(combined(pair, (0.8, 0.0), 0.5), 0.4)
        self.assertEqual(combined(((visible_bolt, 0.4),) * 2, visible_bolt, 0.5), 0.4)
        self.assertEqual(combined((), visible_bolt, 0.5), 0.0)
        self.assertEqual(combined(strikes, visible_bolt, 0.0), 0.0)
        for centre in ((0.0, 0.0), (-1.0, 1.0), (1.0, -1.0)):
            for pixel in ((0.0, 0.0), (-0.9, 0.9), (0.7, -0.4)):
                for luma in (0.0, 0.001, 0.1, 1.0):
                    for pulse in (0.0, 0.42, 3.0):
                        with self.subTest(centre=centre, pixel=pixel, luma=luma, pulse=pulse):
                            distance = math.hypot((pixel[0] - centre[0]) * 1.777,
                                                  pixel[1] - centre[1])
                            previous = pulse * eval(response, {"__builtins__": {}},
                                                    dict(names, distanceToStrike=distance,
                                                         sceneLuma=luma))
                            self.assertEqual(combined(((centre, pulse),), pixel, luma), previous)

    def test_flash_list_is_carried_by_every_producer_and_uniform_upload(self):
        header = read(CLIENT / "rendering/LiveStrikeFlash.h")
        cap = re.search(r"kMaxLiveStrikeFlashes = (\d+);", header)[1]
        shader = read(ROOT / "res/shaders/lightning_overlay.frag")
        self.assertIn("#define MAX_STRIKE_FLASHES " + cap, shader)
        for uniform in ("u_flashNdc", "u_flashPulse", "u_flashOnscreen"):
            self.assertIn(uniform + "[MAX_STRIKE_FLASHES]", shader)
        frame = read(CLIENT / "rendering/RenderFrameTypes.h")
        self.assertIn("LiveStrikeFlashList flashes;", frame)
        lighting = read(CLIENT / "rendering/passes/LightingPass.cpp")
        self.assertIn("std::min(lit.flashes.count, kMaxLiveStrikeFlashes)", lighting)
        self.assertIn('setInt("u_flashCount", static_cast<int>(flash_count))', lighting)
        self.assertIn("for (std::size_t i = 0; i < flash_count; ++i)", lighting)
        self.assertIn("const auto& flash = lit.flashes.entries[i];", lighting)
        # Indexed names are built once per shader generation, then uploaded by slot.
        for slot, (uniform, value) in enumerate((("u_flashNdc", "flash.strike_ndc"),
                                                 ("u_flashPulse", "flash.pulse"),
                                                 ("u_flashOnscreen", "flash.onscreen ? 1 : 0"))):
            self.assertIn('"' + uniform + '" + suffix', lighting)
            self.assertIn("(names[" + str(slot) + "], " + value + ")", lighting)
        for path in ("core/ScenarioRunnerDrive.cpp", "core/scenarios/WorldVisualSweep.cpp"):
            source = read(CLIENT / path)
            self.assertEqual(source.count("lstate.flashes = composed.flashes;"),
                             source.count("ComposeLiveStrikes("))
        live = read(CLIENT / "main_client.cpp")
        self.assertIn("g_app.weather.liveBolt.flashes = composed.flashes;", live)
        self.assertIn("Luminumbra::Rendering::SelectLiveStrikeFlashCandidates( pulses)", compact(live))
        self.assertIn("for (const std::size_t index : flash_selected)", live)
        self.assertIn("std::sort(frame_indices.begin(), frame_indices.end());", live)
        self.assertIn("for (const std::size_t index : frame_indices)", live)
        self.assertNotIn("pulses[strongest]", live)

    def test_storm_does_not_add_a_night_visibility_floor(self):
        weather = read(ROOT / "res/shaders/weather_system.frag")
        sky = read(ROOT / "res/shaders/enhanced_skybox.frag")
        self.assertNotIn("floorLift", weather)
        self.assertNotIn("stormMurk", sky)
        self.assertNotIn("nightStormFloor", sky)
        self.assertIn("finalColor *= dim;", weather)
        self.assertIn("u_stormSkyFloor", sky)  # still gates stars under overcast

class TerrainMaterialWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shaders = ROOT / "res/shaders"
        cls.fragment = read(cls.shaders / "g_buffer.frag")
        cls.terrain = read(cls.shaders / "g_buffer.vert")

    def test_material_interface_matches_all_shipping_vertex_paths(self):
        fragment_interface = compact(block(self.fragment, "in VS_OUT"))
        for name in ("g_buffer.vert", "instanced_mesh.vert", "skinned_mesh.vert"):
            source = read(self.shaders / name)
            with self.subTest(vertex=name):
                # Ignore comments when comparing the complete linked interface.
                interface = lambda value: compact(re.sub(r"//[^\n]*", "", value))
                self.assertEqual(interface(block(source, "out VS_OUT")),
                                 interface(block(self.fragment, "in VS_OUT")))
                self.assertIn("vec4 GroundWeights;", fragment_interface)
                self.assertIn("float DeepslateWeight;", fragment_interface)
                if name != "g_buffer.vert":
                    self.assertIn("vs_out.GroundWeights = vec4(0.0);", source)
                    self.assertIn("vs_out.DeepslateWeight = 0.0;", source)
        self.assertIn("vec4(equal(uvec4(aMaterialID), uvec4(1u, 2u, 3u, 4u)))",
                      self.terrain)
        self.assertIn("vs_out.DeepslateWeight = aMaterialID == 5u ? 1.0 : 0.0;",
                      self.terrain)
        self.assertNotIn("flat vec4 GroundWeights", self.fragment)
        self.assertNotIn("flat float DeepslateWeight", self.fragment)

    def test_natural_materials_blend_all_surface_channels_before_lighting(self):
        main = block(self.fragment, "void main()")
        blend = block(main, "if (naturalCoverage)")
        self.assertIn("fs_in.MaterialID >= 1u && fs_in.MaterialID <= 5u", main)
        self.assertIn("dot(fs_in.GroundWeights, vec4(1.0)) + fs_in.DeepslateWeight", main)
        self.assertIn("for (int lane = 0; lane < 5; ++lane)", blend)
        lane = block(blend, "if (weight > 0.0)")
        self.assertIn("weight / coverage", lane)
        self.assertIn("terrainSurface(uint(lane + 1)", lane)
        self.assertIn("vec3 laneN = geomN;", lane)
        for output, sampled in (("albedo", "laneAlbedo"), ("worldN", "laneN"),
                                ("roughness", "laneRoughness"),
                                ("metallic", "laneMetallic"), ("ao", "laneAO")):
            with self.subTest(channel=output):
                self.assertRegex(blend, rf"{output} = (vec3\()?0\.0")
                self.assertIn(f"{output} += {sampled} * normalizedWeight;", lane)
        self.assertIn("worldN = normalize(worldN);", blend)
        surface = block(self.fragment, "void terrainSurface(")
        self.assertIn("float matIndex = float(materialId) / 255.0;", surface)
        self.assertIn("baseAlbedo *= albedoScale * albedoTint;", surface)
        self.assertLess(surface.index("baseAlbedo *= albedoScale * albedoTint;"),
                        surface.index("baseAlbedo = mix(baseAlbedo, rockAlbedo, rockW);"))
        self.assertIn("albedo = baseAlbedo;", surface)
        # No new coverage path can change the material-ID attachment or water ownership.
        self.assertIn("float material_id_normalized = float(fs_in.MaterialID) / 255.0;", main)
        self.assertIn("farLiveOwns(local, fs_in.MaterialID)", main)
        self.assertIn("discard;", block(main, "if (fs_in.MaterialID == 7u)"))
        fallback = compact(block(main, "} else if ((texInfo.a > 0.5"))
        self.assertIn("terrainSurface(fs_in.MaterialID, fs_in.WorldPos, worldDx, worldDy,", fallback)
        self.assertIn("(texInfo.a > 0.5 || naturalCoverage) && u_forceFlat == 0", main)
        self.assertLess(main.index("if (proceduralLeaf)"), main.index("if (naturalCoverage)"))
        self.assertLess(main.index("u_skinnedAlbedoLayer >= 0"), main.index("if (naturalCoverage)"))
        self.assertIn("albedo = materialAlbedo(materialId);", surface)
        self.assertLess(surface.index("albedo = materialAlbedo(materialId);"),
                        surface.index("if (texInfo.a <= 0.5) return;"))
        self.assertIn("if (texInfo.a <= 0.5) return;", surface)
        self.assertIn("(u_terrainRoughnessValid == 1)", surface)

    def test_coverage_guard_keeps_partial_and_non_ground_surfaces_on_fallback(self):
        main = block(self.fragment, "void main()")
        match = re.search(r"bool naturalCoverage = ([^;]+);", main)
        self.assertIsNotNone(match)
        predicate = re.sub(r"(\d+)u\b", r"\1", match.group(1))
        for material_id in (0, 1, 2, 3, 4, 5, 6, 7, 200, 255, 2**32 - 1):
            for coverage in (0.0, 0.25, 0.5, 0.998, 1.0, 1.0000001):
                with self.subTest(material=material_id, coverage=coverage):
                    accepts = cpp_expression(predicate, coverage=coverage,
                                             **{"fs_in.MaterialID": material_id})
                    self.assertEqual(accepts, 1 <= material_id <= 5 and coverage >= 1.0)

    def test_material_blend_gradients_precede_divergence_and_keep_mip_bias(self):
        main = block(self.fragment, "void main()")
        self.assertLess(main.index("dFdx(fs_in.WorldPos)"), main.index("if ("))
        self.assertLess(main.index("dFdy(fs_in.WorldPos)"), main.index("if ("))
        sample = block(self.fragment, "vec4 sampleTerrain(")
        self.assertIn("exp2(u_lodBias)", sample)
        self.assertIn("textureGrad(image, uvw, dx * biasScale, dy * biasScale)", sample)
        for name in ("albedo", "normal", "roughness"):
            surface = block(self.fragment, "triplanar_" + name + "(")
            self.assertNotIn("sampleBias(", surface)
            self.assertNotIn("textureQueryLod(", surface)
            for axis in ("zy", "xz", "xy"):
                self.assertIn(f"dx.{axis} * scale, dy.{axis} * scale", surface)
        self.assertIn("baseLod + kDetailBlurLod + u_lodBias", self.fragment)

    def test_sand_detail_is_world_anchored_and_uses_all_three_arrays(self):
        sand = block(self.fragment, "if (materialId == 4u)")
        self.assertIn("worldPos * 4.17 + vec3(37.0, 17.0, 53.0)", sand)
        self.assertIn("dx * 4.17, detailDy = dy * 4.17", sand)
        self.assertIn("vnoise(worldPos * 0.13)", sand)
        for name in ("albedo", "normal", "roughness"):
            self.assertIn("triplanar_" + name + "(detailPos,", sand)
        self.assertEqual(sand.count("detailDx, detailDy)"), 3)
        self.assertIn("if (u_terrainRoughnessValid == 1)", sand)
        self.assertNotIn("gl_FragCoord", sand)
        self.assertNotIn("u_time", sand)

    def test_all_terrain_arrays_enable_bounded_anisotropy_only_when_supported(self):
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        terrain = block(pipeline, "void RenderPipeline::init_terrain_textures()")
        self.assertIn("GLAD_GL_VERSION_4_6", terrain)
        self.assertIn('glfwExtensionSupported("GL_ARB_texture_filter_anisotropic")', terrain)
        self.assertIn('glfwExtensionSupported("GL_EXT_texture_filter_anisotropic")', terrain)
        enabled = block(terrain, "if (terrain_anisotropy_supported)")
        self.assertIn("glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY", enabled)
        self.assertIn("std::clamp(maximum_anisotropy, 1.0f, 8.0f)", enabled)
        self.assertEqual(terrain.count("if (terrain_anisotropy_supported)"), 4)
        for binding in ("m_terrainTextureArray", "m_terrainNormalArray", "m_terrainRoughnessArray"):
            # Each setting belongs to its array, before the next array binding.
            after_bind = terrain.split("glBindTexture(GL_TEXTURE_2D_ARRAY, " + binding + ");", 1)[1]
            settings = after_bind.split("glBindTexture(GL_TEXTURE_2D_ARRAY,", 1)[0]
            guarded = block(settings, "if (terrain_anisotropy_supported)")
            self.assertIn("GL_TEXTURE_MAX_ANISOTROPY, terrain_anisotropy", guarded)

class PhotoEnvironmentWiring(unittest.TestCase):
    def test_exposure_release_is_independent_of_time_capture_pins_and_pause(self):
        overlays = read(CLIENT / "app/DebugOverlays.cpp")
        capture = compact(block(overlays, "void UpdatePhotoCapture("))
        self.assertIn("app.hud.photoExposure.Apply( renderPipeline, "
                      "Luminumbra::Rendering::ManualExposureMultiplier(app.hud.photoMode.lens));",
                      capture)
        self.assertNotIn("set_exposure_override(", capture)
        context = block(read(CLIENT / "app/ClientAppContext.h"), "struct HudState")
        self.assertIn("PhotoExposureOverride photoExposure;", context)
        update = block(overlays, "void UpdatePhotoEnvironment(")
        release = "app.hud.photoExposure.ReleaseIfInactive(renderPipeline, photo_active);"
        self.assertIn(release, update)
        self.assertNotIn("if (", update.split(release, 1)[0])
        self.assertLess(update.index(release), update.index("if (!photo_active || capture_pinned)"))
        self.assertNotIn("set_exposure_override(", update)
        self.assertNotIn("photoExposure =", update)
        self.assertIn("const bool photo_active = playerController && "
                      "playerController->photo_mode_active();", compact(update))
        hud = compact(block(overlays, "void UpdatePhotoModeAndHud("))
        self.assertIn("app.hud.photoMode.active = playerController->photo_mode_active();", hud)
        self.assertNotIn("capture_pinned", hud)
        exposure = block(read(CLIENT / "app/PhotoEnvironment.h"), "class PhotoExposureOverride")
        self.assertIn("bool applied_ = false;", exposure)
        self.assertNotIn("time_active", exposure)
        apply = block(exposure, "void Apply(")
        self.assertIn("pipeline.set_exposure_override(multiplier);", apply)
        self.assertIn("applied_ = true;", apply)
        release_body = block(exposure, "void ReleaseIfInactive(")
        guard = re.search(r"if\s*\(([^()]*)\)", release_body).group(1)
        self.assertIn("return;", block(release_body, "if ("))
        self.assertIn("pipeline.set_exposure_override(-1.0f);", release_body)
        self.assertIn("applied_ = false;", release_body)
        for active in (False, True):
            for applied in (False, True):
                with self.subTest(active=active, applied=applied):
                    self.assertEqual(cpp_expression(guard, photo_mode_active=active, applied_=applied),
                                     active or not applied)

    def test_photo_selection_follows_live_defaults_and_precedes_pins_and_render(self):
        main = read(CLIENT / "main_client.cpp")
        release = main.index("renderPipeline.set_time_of_day_hold(false);")
        weather_release = main.index("g_app.hud.photoWeather.Restore(renderPipeline);")
        loop = block(main, "while (!glfwWindowShouldClose(window))")
        before_restore = loop.split("g_app.hud.photoWeather.Restore(renderPipeline);", 1)[0]
        self.assertNotIn("if (", before_restore)
        self.assertLess(weather_release, main.index("GameState currentState ="))
        self.assertLess(weather_release, main.index("RenderMenuScreens("))
        weather_writer_guard = "if (g_systemConfig.enabled(luminumbra::core::SysKey::RenderLiveWeather))"
        self.assertLess(weather_release, main.index(weather_writer_guard))
        self.assertNotIn("photoWeather.Restore", block(main, weather_writer_guard))
        photo = main.index("UpdatePhotoEnvironment(")
        for live in ("renderPipeline.set_world_clock(gameSession->GetWorldClock());",
                     "renderPipeline.set_time_of_day_tick(sim_tick);",
                     "renderPipeline.set_weather_state(weather_state);"):
            self.assertLess(release, main.index(live))
            self.assertLess(weather_release, main.index(live))
            self.assertLess(main.index(live), photo)
        for pin in ("scenario_runner->onPreRenderCapturePins();",
                    "renderPipeline.set_time_of_day(Luminumbra::BenchmarkPose::kTimeOfDay);",
                    "renderPipeline.set_weather(wt, g_app.capture.scene_weather_intensity);",
                    "scenario_runner->onPreRenderFoliage()"):
            self.assertLess(photo, main.index(pin))
        self.assertLess(photo, main.index("renderPipeline.render_frame(", photo))

    def test_photo_capture_has_no_late_environment_writers(self):
        overlays = read(CLIENT / "app/DebugOverlays.cpp")
        capture = block(overlays, "void UpdatePhotoCapture(")
        for setter in ("set_weather(", "set_time_of_day(", "set_time_of_day_hold(",
                       "consume_tod_nudge()", "consume_weather_cycle()"):
            self.assertNotIn(setter, capture)
        update = block(overlays, "void UpdatePhotoEnvironment(")
        self.assertIn("SelectPhotoEnvironment(live, photo, photo_active, capture_pinned)", update)
        self.assertIn("renderPipeline.set_time_of_day(selected.time_of_day);",
                      block(update, "if (selected.time_active)"))
        self.assertIn("app.hud.photoWeather.Apply(renderPipeline, selected.weather, selected.intensity);",
                      block(update, "if (selected.weather_active)"))

    def test_weather_snapshot_survives_photo_selection_reset(self):
        context = block(read(CLIENT / "app/ClientAppContext.h"), "struct HudState")
        self.assertIn("PhotoWeatherOverride photoWeather;", context)
        update = block(read(CLIENT / "app/DebugOverlays.cpp"), "void UpdatePhotoEnvironment(")
        self.assertIn("photo = {};", update)
        self.assertNotIn("photoWeather =", update)

    def test_all_capture_modes_disable_photo_overrides(self):
        main = compact(read(CLIENT / "main_client.cpp"))
        guard = re.search(r"const bool photo_capture_pinned = ([^;]+);", main).group(1)
        flags = ("scenario_config.active()", "g_rb_active", "g_app.capture.timelapse_frames",
                 "g_app.capture.scene_active", "g_app.capture.frame_scan_active",
                 "g_app.capture.render_parity_active", "g_app.capture.survey_active",
                 "g_app.capture.bake_impostor_path.empty()",
                 "g_app.capture.ui_screenshot_screen.empty()", "g_app.capture.ui_thumbs")
        defaults = {flag: flag.endswith(".empty()") for flag in flags}
        self.assertFalse(cpp_expression(guard, **defaults))
        for flag in flags:
            with self.subTest(flag=flag):
                inputs = dict(defaults)
                inputs[flag] = not inputs[flag]
                self.assertTrue(cpp_expression(guard, **inputs))


class WorldVisualSweepWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sweep = read(CLIENT / "core/scenarios/WorldVisualSweep.cpp")
        cls.capture = block(read(CLIENT / "core/ScenarioRunnerCapture.cpp"),
                            "void ScenarioRunnerImpl::onPreRenderWorldSweep()")

    def test_sweep_streams_at_camera_until_residency_with_water_initialization(self):
        main = read(CLIENT / "main_client.cpp")
        anchored = main.split("const bool cam_anchored =", 1)[1].split(";", 1)[0]
        self.assertIn("scenario_config.world_visual_sweep()", anchored)
        callback = block(self.capture, "deps.render_and_read =")
        update = compact(block(callback, "if (stream_world)"))
        self.assertIn("Luminumbra::Vec3(g_camera->Position)", update)
        # The existing live update seeds newly streamed water; paused updates do not.
        self.assertIn("gameSession->GetPhysicsSystem());", update)
        self.assertNotIn("TickSimulation", callback)
        self.assertNotIn("sweep_stream_frames", callback)
        self.assertIn("deps.readiness_timeout_seconds = scenario_config.readiness_timeout_seconds;",
                      self.capture)
        self.assertIn("settle_uploads(", callback)
        self.assertIn("UploadPresentation::Settled", callback)
        self.assertIn("deps.render_and_read(pixels, fb_w, fb_h, false, "
                      "i + 1 == settle_frames ? attribution_dir : std::filesystem::path{})",
                      compact(self.sweep))

    def test_warmup_requires_complete_terrain_lod0_and_water(self):
        terrain = re.search(r"const bool terrain_ready =\s*([^;]+);", self.sweep)
        self.assertIsNotNone(terrain, "warmup must test measured terrain residency")
        expression = terrain.group(1)
        ready = {"warmup_frames": 28, "coverage.near_field_renderable": True,
                 "coverage.ready_surface_chunks": 625, "coverage.expected_surface_chunks": 625,
                 "coverage.pending_lod_chunks": 0, "near_coverage.expected_surface_chunks": 49,
                 "near_coverage.lod_counts[0]": 49}
        cases = [(None, None, True), ("warmup_frames", 27, False),
                 ("coverage.near_field_renderable", False, False),
                 ("coverage.ready_surface_chunks", 624, False),
                 ("coverage.pending_lod_chunks", 1, False),
                 ("near_coverage.lod_counts[0]", 0, False),
                 ("near_coverage.lod_counts[0]", 48, False),
                 ("near_coverage.expected_surface_chunks", 0, False)]
        for field, value, expected in cases:
            with self.subTest(field=field):
                inputs = dict(ready)
                if field is not None:
                    inputs[field] = value
                self.assertEqual(cpp_expression(expression, **inputs), expected)
        loop = block(self.sweep, "while (!streaming_ready")
        self.assertIn("get_camera_local_coverage_stats(anchor, 12)", loop)
        self.assertIn("get_camera_local_coverage_stats(anchor, 3)", loop)
        self.assertIn("deps.render_and_read(pixels, fb_w, fb_h, true, std::filesystem::path())", loop)
        self.assertIn("return false;", block(loop, "if (!deps.render_and_read(pixels, fb_w, fb_h, true, std::filesystem::path())"))
        water = block(loop, "if (terrain_ready)")
        self.assertIn("get_last_render_pass_stats()", water)
        self.assertIn("water_indices_drawn = stats.water_indices_drawn;", water)
        predicate = re.search(r"streaming_ready = ([^;]+);", water).group(1)
        for draws in (0, 1):
            for count in (0, 3, 47172):
                self.assertEqual(cpp_expression(predicate, water_indices_drawn=count,
                                                **{"stats.water_draws": draws}),
                                 draws > 0 and count > 0)
        self.assertIn("AimCameraAt(camera, water_target.focus);",
                      self.sweep.split("const auto readiness_deadline", 1)[0])

    def test_expired_or_invalid_warmup_cannot_capture_cells(self):
        run = block(self.sweep, "bool RunWorldVisualSweep(")
        guard = run.split("if (", 1)[1].split(") {", 1)[0]
        self.assertIn("deps.readiness_timeout_seconds <= 0", guard)
        self.assertIn("return false;", block(self.sweep, "if (deps.game_session"))
        condition = re.search(r"while \(([^\n]+)\) \{", self.sweep).group(1)
        condition = condition.replace("std::chrono::steady_clock::now()", "now")
        for ready, now, expected in ((False, 119, True), (False, 120, False),
                                     (False, 121, False), (True, 119, False)):
            self.assertEqual(cpp_expression(condition, streaming_ready=ready, now=now,
                                            readiness_deadline=120), expected)
        refused = block(self.sweep, "if (!streaming_ready)")
        self.assertIn("return false;", refused)
        self.assertLess(self.sweep.index("if (!streaming_ready)"),
                        self.sweep.index("for (const SeasonPin& season"))
        self.assertIn("std::chrono::seconds(deps.readiness_timeout_seconds)", self.sweep)

    def test_water_cells_require_drawn_geometry_even_with_blue_sky_pixels(self):
        gate = re.search(r"if \((c.water_aimed && \(c.water_draws[^\n]+)\) \{", self.sweep)
        self.assertIsNotNone(gate, "water presence must reject cells without submitted water")
        predicate = gate.group(1)
        for aimed in (False, True):
            for draws, indices in ((0, 0), (1, 0), (0, 3), (1, 3)):
                self.assertEqual(cpp_expression(predicate, **{
                    "c.water_aimed": aimed, "c.water_draws": draws,
                    "c.water_indices_drawn": indices}), aimed and (draws == 0 or indices == 0))
        for field in ("water_draws", "water_indices_drawn"):
            self.assertIn(f"cell.{field} = stats.{field};", self.sweep)
            self.assertIn(f'{{"{field}", c.{field}}}', self.sweep)
        # Presence is depth-tested and colour independent; submission remains required.
        self.assertIn("c.water_aimed && c.visible_water_pixels == 0", self.sweep)
        self.assertIn("c.water_aimed && !c.water_mask_valid", self.sweep)
        self.assertIn('manifest["warmup_water_indices_drawn"] = water_indices_drawn;', self.sweep)

    def test_water_target_is_local_and_absence_refuses_the_sweep(self):
        self.assertNotIn("FindWaterVisualCameraTarget(game_session)", self.sweep)
        self.assertIn("anchor(168.0f, 10.0505952835083f, -40.0f)", self.sweep)
        helper = read(CLIENT / "rendering/WaterVisibility.h")
        search = block(helper, "FindWaterVisibilityTarget(")
        self.assertIn("for (int dz = -96; dz <= 96; dz += 16)", search)
        self.assertIn("for (int dx = -96; dx <= 96; dx += 16)", search)
        self.assertIn("for (int step = 0; step <= 32 && clear; ++step)", search)
        self.assertIn("h < sea_level - 0.5f", search)
        self.assertIn("h < point.y - 0.5f", search)
        self.assertIn("std::isfinite(h)", search)
        self.assertIn("result = {.found = true, .eye = eye, .focus = focus};", search)
        self.assertIn("FindWaterVisibilityTarget(", self.sweep)
        self.assertIn("camera->Position = water_target.eye;", self.sweep)
        verify = block(self.sweep, "if (!target_rendered || !target_mask_written")
        self.assertIn("return false;", verify)
        self.assertIn("pipeline->water_visibility_mask());", self.sweep)
        self.assertIn("const bool target_rendered = render_settled(3, {});", self.sweep)
        self.assertIn("pipeline->water_visibility_mask().visible_pixels_in_target() == 0", self.sweep)
        self.assertIn("return false;", block(self.sweep, "if (!water_target.found)"))
        self.assertLess(self.sweep.index("if (!water_target.found)"),
                        self.sweep.index("// --- run the matrix"))

    def test_water_presence_refuses_empty_target_even_with_geometry_or_sky_hue(self):
        condition = re.search(r"if \((c.water_aimed && c.visible_water_pixels[^)]+)\)",
                              self.sweep).group(1)
        for aimed, visible, expected in ((True, 0, True), (True, 1, False),
                                         (False, 0, False), (False, 1, False)):
            self.assertEqual(cpp_expression(condition, **{
                "c.water_aimed": aimed, "c.visible_water_pixels": visible}), expected)
        invalid = block(self.sweep, "if (c.water_aimed && !c.water_mask_valid)")
        self.assertIn('failures.push_back("missing_water_mask:" + c.file);', invalid)
        self.assertIn("WriteSweepWaterMask(deps.artifact_dir / cell.water_mask_file, mask)",
                      compact(self.sweep))
        self.assertIn("return passed && manifest_written;", self.sweep)

    def test_water_target_rejects_every_invalid_patch_and_sight_line(self):
        search = block(read(CLIENT / "rendering/WaterVisibility.h"), "FindWaterVisibilityTarget(")
        assignments = re.findall(r"clear = (std::isfinite[^;]+);", search)
        self.assertEqual(len(assignments), 2)
        for expression, limit in zip(assignments, (0.0, 10.0)):
            expression = expression.replace("std::isfinite(h)", "finite").replace("0.5f", "0.5")
            for height, finite in ((limit - 1, True), (limit - 0.5, True),
                                   (limit, True), (limit - 1, False)):
                self.assertEqual(cpp_expression(expression, finite=finite, h=height,
                                                sea_level=limit, **{"point.y": limit}),
                                 finite and height < limit - 0.5)
        selected = re.search(r"if \(([^()]+)\) \{\s*nearest = distance;", search).group(1)
        for clear in (False, True):
            self.assertEqual(cpp_expression(selected, clear=clear), clear)
        self.assertIn("WaterVisibilityTarget result;", search)
        self.assertIn("return result;", search)
        self.assertIn("bool found = false;", read(CLIENT / "rendering/WaterVisibility.h"))

    def test_water_visibility_uses_surviving_alpha_not_blue_or_warm_rgb(self):
        helper = read(CLIENT / "rendering/WaterVisibility.h")
        conversion = block(helper, "static WaterVisibilityMask FromRgba(")
        predicate = re.search(r"result.pixels\[i\] = (.*?) \? 255 : 0;", conversion).group(1)
        for label, rgb, alpha, expected in (
                ("blue water", (20, 100, 200), 150, True),
                ("warm water", (220, 90, 20), 150, True),
                ("blue sky", (20, 100, 200), 0, False),
                ("occluded water", (0, 0, 0), 0, False)):
            with self.subTest(label=label):
                self.assertEqual(cpp_expression(predicate, alpha=alpha,
                                                r=rgb[0], g=rgb[1], b=rgb[2]), expected)
        self.assertIn("rgba[i * 4u + 3u]", conversion)
        self.assertIn("rgba.size() != count * 4u", conversion)
        roi = block(helper, "std::uint64_t visible_pixels_in_target() const")
        self.assertIn("if (!valid())", roi)
        self.assertIn("y = height / 4; y < height - height / 4", roi)
        self.assertIn("x = width / 4; x < width - width / 4", roi)
        self.assertIn("WaterVisibilityMask::FromRgba(width, height, rgba)",
                      read(CLIENT / "rendering/passes/WaterPass.cpp"))

    def test_water_mask_sizes_refuse_both_invalid_axes_before_arithmetic(self):
        helper = read(CLIENT / "rendering/WaterVisibility.h")
        expression = re.search(r"return ([^;]+);", block(helper, "static bool valid_size(")).group(1)
        for axis in ("w", "h"):
            expression = expression.replace(f"static_cast<std::size_t>({axis})", axis)
        for w, h, expected in ((0, 8, False), (8, 0, False), (-1, 8, False),
                                (8, -1, False), (3, 8, False), (8, 3, False),
                                (4, 4, True), (4096, 4096, True),
                                (4097, 8, False), (8, 4097, False)):
            with self.subTest(w=w, h=h):
                self.assertEqual(cpp_expression(expression, w=w, h=h, kMaxPixels=4096 * 4096),
                                 expected)

    def test_water_draw_and_visibility_replay_refuse_unrepresentable_counts(self):
        source = read(CLIENT / "rendering/passes/WaterPass.cpp")
        guard = block(source, "GLsizei WaterElementCount(")
        condition = re.search(r"if \((.*?)\) \{", guard).group(1)
        condition = condition.replace("static_cast<u32>(std::numeric_limits<GLsizei>::max())", "limit")
        for count in (0, 1, 2147483647, 2147483648, 4294967295):
            self.assertEqual(cpp_expression(condition, count=count, limit=2147483647),
                             count > 2147483647)
        self.assertIn("return 0;", guard)
        # Capture admission, the original draw and visibility replay share the conversion.
        self.assertEqual(source.count("WaterElementCount(item.element_count)"), 3)
        self.assertIn("continue;", block(block(source, "WaterPass::execute("), "if (count == 0)"))
        self.assertIn("continue;", block(block(source, "void WaterPass::capture_visibility("),
                                         "if (elements == 0)"))

    def test_water_visibility_replays_shipping_shader_and_exact_depth_only_when_requested(self):
        source = read(CLIENT / "rendering/passes/WaterPass.cpp")
        execute = block(source, "WaterPass::execute(")
        self.assertIn("m_visibility_mask = {};", execute)
        self.assertIn("glEnable(GL_DEPTH_TEST)", execute)
        self.assertIn("glDepthFunc(GL_GREATER)", execute)
        self.assertIn("glDepthMask(GL_FALSE)", execute)
        self.assertIn("capture_visibility(ctx, input);",
                      block(execute, "if (capture_visibility_requested)"))
        capture = block(source, "void WaterPass::capture_visibility(")
        self.assertIn("ctx.frozen_replay", capture)
        self.assertIn("ctx.lit_scene_depth.id == 0", capture)
        self.assertIn("glFramebufferRenderbuffer( GL_FRAMEBUFFER", compact(capture))
        self.assertIn("ctx.lit_scene_depth.id", capture)
        self.assertNotIn("glDisable(GL_DEPTH_TEST)", capture)
        self.assertNotIn("glDepthMask(GL_TRUE)", capture)
        self.assertNotIn("glUseProgram", capture)
        self.assertIn("for (const auto& item : input.draw_items)", capture)
        self.assertIn("glDrawElements", capture)
        self.assertIn("glGetError() == GL_NO_ERROR", capture)
        self.assertIn("glClearTexImage(color, 0, GL_RGBA, GL_FLOAT, zero)", capture)
        self.assertIn("glReadPixels", capture)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        water = block(pipeline, "void RenderPipeline::execute_stage_water(")
        self.assertIn("water_ctx, water_input, camera, m_water_visibility_capture_enabled", compact(water))
        self.assertIn("ctx.lit_scene_depth = RenderbufferHandle", block(
            pipeline, "RenderContext RenderPipeline::make_water_context("))
        gate = read(ROOT / "tools/gates/validate-engine-frontier.ps1")
        water_gate = gate.split("$waterCells =", 1)[1].split("# No rain", 1)[0]
        self.assertIn("$cell.visible_water_pixels -lt 1", water_gate)
        self.assertIn("$cell.water_mask_valid", water_gate)
        self.assertIn("Assert-PpmArtifact -Path $cell.water_mask_file", water_gate)
        self.assertNotIn("$cell.water_like_pixels", water_gate)

    def test_rain_pool_is_reset_for_every_cell_not_just_clear_weather(self):
        weather = block(self.sweep, "const auto apply_weather =")
        prefix = weather.split("if (storm)", 1)[0]
        self.assertIn("if (particles != nullptr)", prefix)
        self.assertIn("particles->clear_emitters();", block(prefix, "if (particles != nullptr)"))
        self.assertEqual(weather.count("clear_emitters();"), 1)
        self.assertIn("particles->add_emitter(", block(weather, "if (storm)"))
        self.assertNotIn("set_emitter_origin", weather)
        self.assertNotIn("rain_emitter", weather)
        self.assertIn("cell.particles_drawn = stats.particles_drawn;", self.sweep)
        # This is a frame occupancy snapshot, not a monotonic counter to subtract.
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        self.assertIn("m_last_render_pass_stats = {};", block(pipeline, "void RenderPipeline::prepare_frame("))
        particle = block(read(CLIENT / "rendering/passes/ParticlePass.cpp"),
                         "std::size_t ParticlePass::execute(")
        self.assertIn("return m_frame_instance_count;", particle)

    def test_capture_and_analysis_refuse_partial_or_unaddressable_rgb_buffers(self):
        callback = block(self.capture, "deps.render_and_read =")
        measure = block(self.sweep, "void MeasureSweepFrame(")
        for source, width, height, size in ((callback, "w", "h", "out_pixels.max_size()"),
                                           (measure, "width", "height", "pixels.size()")):
            checked = re.search(r"if \(([^{}]+(?:max_size|pixels.size)[^{}]+)\) \{", source)
            self.assertIsNotNone(checked, "RGB storage must be checked before multiplication")
            guard = checked.group(1)
            guard = guard.replace(f"static_cast<std::size_t>({width})", width)
            guard = guard.replace(f"static_cast<std::size_t>({height})", height)
            guard = guard.replace("3u", "3")
            for w, h, n, refused in ((0, 2, 12, True), (2, 0, 12, True),
                                      (-1, 2, 12, True), (2, -1, 12, True),
                                      (2, 2, 0, True), (2, 2, 11, True),
                                      (2, 2, 12, False), (2, 2, 13, False),
                                      (2**31-1, 2**31-1, 2**64-1, False),
                                      (2**31-1, 2**31-1, 2**63-1, True)):
                self.assertEqual(cpp_expression(guard, **{width: w, height: h, size: n}), refused)


class ParticleContributorTests(unittest.TestCase):
    def test_original_particle_batch_uses_the_shared_forward_transaction(self):
        source = read(CLIENT / "rendering/passes/ParticlePass.cpp")
        draw = block(source, "std::size_t ParticlePass::execute(")
        setup = block(draw, "if (auto* capture = GBufferDiagnosticCapture::Current())")
        for required in ("GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE", "if (attachment_type == GL_TEXTURE)",
                         "GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME", "diagnostic_attachment.emplace(",
                         "DiagnosticExtent{ctx.internal_w(), ctx.internal_h()}",
                         "draw_shader = capture->variant(draw_shader, false)"):
            self.assertIn(required, setup)
        for required in ('.shader_variant = "magical_particles"', '.chunk = {}', '.lod = 0',
                         '.ownership = DiagnosticOwnership::Live', '.pass = DiagnosticPass::Forward',
                         '.provenance = DiagnosticProvenance::Mesh',
                         '.generated_mesh = "particles/instanced-billboards"'):
            self.assertIn(required, draw)
        self.assertLess(draw.index("if (!ctx.lit_scene.id)"), draw.index("diagnostic_attachment.emplace("))
        self.assertLess(draw.index("m_frame_instance_count == 0"), draw.index("diagnostic_attachment.emplace("))
        self.assertLess(draw.index("diagnostic_attachment.emplace("), draw.index("draw_shader->use()"))
        self.assertLess(draw.index("capture->write_current("), draw.index("AttributedGl::Submit("))
        self.assertEqual(draw.count("capture->write_current("), 1)
        self.assertEqual(draw.count("AttributedGl::Submit("), 1)
        self.assertIn("glDrawArraysInstanced,GLenum{GL_TRIANGLE_STRIP},0,4,static_cast<GLsizei>(m_frame_instance_count)",
                      re.sub(r"\s+", "", draw))
        # Both refusal paths keep the original identity but block publication.
        shared = read(CLIENT / "rendering/GBufferTargets.cpp")
        current = block(shared, "void GBufferDiagnosticCapture::write_current(")
        self.assertIn("record_contributor();", block(current, "if (!state_->framebuffer)"))
        self.assertIn('constexpr std::string_view suffix = ":contributor_only";', current)
        self.assertIn("abort();", block(current, "const auto record_contributor ="))
        self.assertIn("record_contributor();", current[current.index("for (const auto& entry"):])
        self.assertIn("abort();", block(shared, "if (!state_->forward_targets)"))
        self.assertIn('unsupported("diagnostic_shader_variant_limit"', shared)
        self.assertIn('unsupported("diagnostic_shader_variant_unavailable"', shared)
        # All live particle kinds populate the same packed instance ring.
        for producer in ("set_precipitation(", "spawn_splash_burst(", "add_waterfall_spray(",
                         "update_ambient_motes(", "spawn_from_emitter("):
            self.assertIn(producer, source)
        self.assertIn("m_instance_vbo[m_ring_cursor]", draw)

    def test_capture_off_bytes_and_fragment_rejection_are_preserved(self):
        shader = read(ROOT / "res/shaders/magical_particles.frag")
        self.assertIn("layout (location = 6) out uvec4 gDiagnosticIds;", shader)
        self.assertEqual(shader.count("gDiagnosticIds = u_diagnosticIds;"), 1)
        self.assertEqual(shader.count("discard;"), 2)
        self.assertLess(shader.rindex("discard;"), shader.index("gDiagnosticIds ="))
        self.assertLess(shader.index("FragColor = finalColor;"), shader.index("gDiagnosticIds ="))
        for predicate, cases in (("shape <= 0.0", ((-1, True), (0, True), (0.5, False))),
                                  ("finalColor.a < 0.01", ((0, True), (0.009, True),
                                                          (0.01, False), (1, False)))):
            self.assertIn("discard;", block(shader, f"if ({predicate})"))
            name = predicate.split()[0]
            for value, expected in cases:
                self.assertEqual(cpp_expression(predicate, **{name: value}), expected)
        # Strip only diagnostic blocks, retaining the original color declaration.
        directive = "#ifdef LUMIN_DIAG_IDS\n"
        self.assertEqual(shader.count(directive), 3)
        while directive in shader:
            start = shader.index(directive)
            end = shader.index("#endif\n", start) + len("#endif\n")
            if "layout (location = 0)\n" not in shader[start:end] and shader[start-2:start] == "\n\n":
                start -= 1
            shader = shader[:start] + shader[end:]
        self.assertEqual(hashlib.sha256(shader.encode()).hexdigest(),
                         "dcb928e1b40f30b5cfcf32796a675a12b38bee756deb9e988ddf2862c0ae3c8c")
        draw = block(read(CLIENT / "rendering/passes/ParticlePass.cpp"),
                     "std::size_t ParticlePass::execute(")
        branch = "if (auto* capture = GBufferDiagnosticCapture::Current())"
        setup = draw.index("Shader* draw_shader = m_shader.get();")
        contents = block(draw, branch)
        end = draw.index(contents) + len(contents) + 1
        draw = draw[:setup] + draw[end:]
        start = draw.index(branch)
        contents = block(draw, branch)
        end = draw.index(contents, start) + len(contents) + 1
        draw = draw[:start] + draw[end:]
        draw = draw.replace("draw_shader->", "m_shader->")
        original = "{" + re.sub(r"\s+", "", draw) + "}"
        self.assertEqual(hashlib.sha256(original.encode()).hexdigest(),
                         "2b294f4af9fe4ae19afd27a6be23488b665e84dbbe07a4505ce820d75ae19286")

    def test_weather_overlay_refuses_instead_of_guessing_a_particle_identity(self):
        draw = block(read(CLIENT / "rendering/passes/SkyboxPass.cpp"),
                     "const glm::mat4& projection)")
        branch = "if (auto* capture = GBufferDiagnosticCapture::Current())"
        refusal = block(draw, branch)
        self.assertIn('capture->unsupported("weather_overlay_contributor_uninstrumented", '
                      '0, DiagnosticOwnership::Live, DiagnosticPass::Forward);', compact(refusal))
        self.assertLess(draw.index("if (!m_weather_shader"), draw.index(branch))
        self.assertLess(draw.index("if (!ctx.lit_scene.id"), draw.index(branch))
        self.assertLess(draw.index(branch), draw.index("AttributedGl::Submit("))
        self.assertNotIn("capture->write", draw)
        self.assertEqual(draw.count("AttributedGl::Submit("), 1)
        start = draw.index(branch)
        end = draw.index(refusal, start) + len(refusal) + 1
        original = "{" + re.sub(r"\s+", "", draw[:start] + draw[end:]) + "}"
        self.assertEqual(hashlib.sha256(original.encode()).hexdigest(),
                         "0c395afb6d2eb7d521b10a2f6600e57b6ba2b3c0a7003a35d5b4ecc8de84033e")


class TreeImpostorCoverageWiringTest(unittest.TestCase):
    def test_both_bake_consumers_validate_before_publishing(self):
        source = read(CLIENT / "rendering/ImpostorBake.cpp")
        render = block(source, "RenderAtlas(const std::string&")
        self.assertEqual(len(re.findall(r"glGetTextureImage\(\s*out\.albedo", render)), 1)
        self.assertEqual(len(re.findall(r"glGetTextureImage\(\s*out\.albedo", source)), 1)
        self.assertIn("MeasureImpostorCoverage(out.rgba, atlas, atlas, n)", compact(render))
        self.assertIn("out.ok = out.alpha_coverage.usable();", render)
        refusal = block(render, "if (!out.ok)")
        for required in ("LUMINUMBRA_CORE_ERROR", "kTreeImpostorAtlasName",
                         "out.alpha_coverage.coverage", "glDeleteTextures(1, &out.albedo)",
                         "glDeleteTextures(1, &out.normal)", "out.albedo = out.normal = 0;"):
            self.assertIn(required, refusal)
        for marker in ("ImpostorBakeResult BakeTreeImpostorAtlas(",
                       "ImpostorAtlasTextures BakeTreeImpostorAtlasToTextures("):
            consumer = block(source, marker)
            self.assertIn("RenderAtlas(rootDir, rp, grid)", consumer)
            self.assertIn("if (!ra.ok)", consumer)
            self.assertNotIn("glGetTexImage", consumer)
        runtime = block(source, "ImpostorAtlasTextures BakeTreeImpostorAtlasToTextures(")
        self.assertLess(runtime.index("out.alpha_coverage = ra.alpha_coverage;"),
                        runtime.index("if (!ra.ok)"))
        export = block(source, "ImpostorBakeResult BakeTreeImpostorAtlas(")
        self.assertLess(export.index("if (!ra.ok)"), export.index("WritePpm("))
        self.assertIn("ra.alpha_coverage.cells[k]", export)

    def test_refusal_reaches_draw_and_mesh_fallback_without_blocking_entry(self):
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        header = read(CLIENT / "rendering/RenderPipeline.h")
        self.assertIn("m_treeImpostorCoverage = ia.alpha_coverage;", pipeline)
        enable = block(pipeline, "if (ia.drawable())")
        self.assertIn("m_treeImpostorsEnabled = true;", enable)
        enabled = block(header, "bool tree_impostor_enabled() const")
        self.assertIn("m_treeImpostorsEnabled && m_treeImpostorCoverage.usable()", enabled)
        self.assertIn("gbuffer_input.tree_impostor_enabled = tree_impostor_enabled();", pipeline)
        gate = block(read(CLIENT / "main_client.cpp"), "auto game_content_error =")
        self.assertIn("!renderPipeline.tree_impostor_coverage().refused()", gate)
        draw = compact(read(CLIENT / "rendering/passes/GBufferPass.cpp"))
        self.assertIn("const bool impostorsOn = input.tree_impostor_enabled;", draw)
        self.assertIn("if (impostorsOn && authoredTree && lod == 3 && cp.impostorTree)", draw)
        self.assertIn("if (cp.impostorTree && !authoredTree) { lod = std::min(lod, 2); }", draw)
        self.assertIn("AuthoredTreeLodConfig(impostorsOn)", draw)
        self.assertIn("if (impostorsOn && !impostorInstances.empty())", draw)
        lod = read(CLIENT / "rendering/TreeLod.h")
        self.assertIn("impostors ? 65.0f : std::numeric_limits<float>::infinity()", lod)
        self.assertIn('TreeImpostorsRequested(Core::ReadEnvironment("LUMIN_TREE_IMPOSTORS"))',
                      compact(pipeline))

    def test_live_capture_serializes_measured_coverage_and_refusal(self):
        live = compact(read(CLIENT / "rendering/CaptureManifestLive.cpp"))
        self.assertIn("const auto& atlas = pipeline.tree_impostor_coverage();", live)
        for key, value in (("atlas", "std::string(kTreeImpostorAtlasName)"),
                           ("alpha_coverage", "atlas.coverage"),
                           ("alpha_refused", "atlas.refused()"),
                           ("alpha_status", "std::string(atlas.status())"),
                           ("alpha_min_cell", "atlas.min_cell"),
                           ("alpha_max_cell", "atlas.max_cell"),
                           ("alpha_cell_count", "std::uint64_t(atlas.cell_count)")):
            self.assertIn('input.render_settings.emplace("tree_impostor_' + key + '", ' +
                          value + ');', live)
        # The manifest has a fixed 128-setting budget; full cell values stay in bake JSON.
        settings = re.findall(r'\{"[a-z0-9_]+",|render_settings\.emplace\("', live)
        self.assertLessEqual(len(settings), 128)
        manifest = read(CLIENT / "rendering/CaptureManifest.cpp")
        self.assertIn("input.render_settings", manifest)
        cmake = read(ROOT / "test/CMakeLists.txt")
        self.assertRegex(cmake, r"add_executable\(octa_impostor_test\s+rendering/octa_impostor_test.cpp")

    def test_readback_restores_transfer_state_and_uses_bounded_dsa(self):
        source = read(CLIENT / "rendering/ImpostorBake.cpp")
        guard = block(source, "struct BakeGlState")
        for name in ("GL_PACK_ALIGNMENT", "GL_PACK_ROW_LENGTH", "GL_PACK_SKIP_ROWS",
                     "GL_PACK_SKIP_PIXELS", "GL_PACK_IMAGE_HEIGHT", "GL_PACK_SKIP_IMAGES",
                     "GL_PACK_SWAP_BYTES", "GL_PACK_LSB_FIRST"):
            self.assertIn(name, guard)
        self.assertIn("glPixelStorei(pack_names[i], pack_values[i]);", guard)
        self.assertIn("glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);", guard)
        self.assertIn("glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));", guard)
        render = compact(block(source, "RenderAtlas(const std::string&"))
        self.assertLess(render.index("kImpostorMaxAtlasSide / n"), render.index("const int atlas = n * tile"))
        self.assertIn("const BakeGlState gl_state;", render)
        self.assertIn("static_cast<GLsizei>(bytes), out.rgba.data()", render)


class ComparisonAtmosphereWiring(unittest.TestCase):
    def test_comparison_pins_follow_live_defaults_for_both_scenarios(self):
        main = read(CLIENT / "main_client.cpp")
        hook = main.index("scenario_runner->onPreRenderCapturePins();")
        self.assertLess(main.index("renderPipeline.set_cloud_state("), hook)
        self.assertLess(main.index("renderPipeline.set_weather_state(weather_state);"), hook)
        drive = read(CLIENT / "core/ScenarioRunnerDrive.cpp")
        weather = re.sub(r"\(\s+", "(", compact(block(drive, "bool ScenarioRunnerImpl::driveWeatherVisual()")))
        self.assertIn('PinComparisonAtmosphere(renderPipeline, root_dir / "data/common/particles", '
                      'g_camera->Position, weather_phase);', weather)
        self.assertIn("renderPipeline.set_season_tick(0);", weather)
        self.assertNotIn("SampleAt(", weather)
        capture = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        pins = block(capture, "void ScenarioRunnerImpl::onPreRenderCapturePins()")
        self.assertIn("(void)driveWeatherVisual();", pins)
        sweep = re.sub(r"\(\s+", "(", compact(block(pins, "scenario_config.timeofday_sweep_smoke()")))
        self.assertIn('PinComparisonAtmosphere(renderPipeline, root_dir / "data/common/particles", '
                      'g_camera->Position, false);', sweep)
        self.assertLess(sweep.index("PinComparisonAtmosphere("), sweep.index("set_season_tick("))

    def test_complete_atmosphere_and_presentation_replace_live_inputs(self):
        source = read(CLIENT / "core/scenarios/WeatherVisual.cpp")
        pin = compact(block(source, "void PinComparisonAtmosphere("))
        for call in ("pipeline.set_weather_state(state.weather);",
                     "pipeline.set_cloud_state(state.clouds);",
                     "pipeline.set_lightning_state({});",
                     "pipeline.set_time_of_day_hold(true);",
                     "presentation_clock.set_capture_tick(kComparisonPresentationTick, "
                     "kComparisonPresentationRate);",
                     "particles->set_precipitation(particle_data_root, camera_position, "
                     "frame_weather.rain_intensity, frame_weather.snow_intensity);",
                     "particles->set_wind(frame_weather.wind_direction * "
                     "frame_weather.wind_strength * 8.0f);"):
            self.assertIn(call, pin)
        builder = compact(block(read(CLIENT / "core/scenarios/ScenarioCommon.h"),
                                "BuildComparisonAtmosphere(bool rain)"))
        for assignment in ("state.weather.driven = true;",
                           "state.weather.rain_intensity = rain ? 1.0f : 0.0f;",
                           "state.weather.fog_density = rain ? 0.1f : 0.0f;",
                           "state.weather.storm_intensity = rain ? 0.4f : 0.0f;",
                           "state.weather.wetness = rain ? 1.0f : 0.0f;",
                           "state.weather.wind_strength = 0.0f;",
                           "state.clouds.enabled = true;",
                           "state.clouds.shadow_enabled = true;",
                           "state.clouds.coverage_amount = rain ? 0.8f : 0.0f;",
                           "state.clouds.shadow_strength = rain ? 0.45f : 0.0f;"):
            self.assertIn(assignment, builder)
        # set_cloud_state preserves old scroll; only the enabled per-frame
        # advance actually resolves it from the fixed presentation clock.
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        advance = block(pipeline, "void RenderPipeline::advance_cloud_phase(")
        self.assertIn("presentation_clock.capture_seconds()", advance)
        self.assertIn("m_cloud_phase = static_cast<float>(*seconds);", advance)
        self.assertIn("m_cloud_state.scroll_offset = wind_xz * (m_cloud_phase * speed);", advance)

    def test_actual_inputs_are_snapshotted_with_every_comparison_capture(self):
        capture = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        for function in ("captureWeatherVisual(", "captureTimeOfDaySweep("):
            body = block(capture, "void ScenarioRunnerImpl::" + function)
            assignment = "stats.atmosphere = ResolvedAtmosphereToJson(renderPipeline);"
            self.assertIn(assignment, body)
            self.assertLess(body.index("glReadPixels("), body.index(assignment))
            self.assertLess(body.index(assignment), body.index("writeScenarioPixels("))
        for file, function in (("WeatherVisual.cpp", "WeatherPixelStatsToJson("),
                               ("TimeOfDaySeasonSweep.cpp", "TimeOfDayPixelStatsToJson(")):
            writer = block(read(CLIENT / "core/scenarios" / file), function)
            self.assertIn('{"atmosphere", stats.atmosphere}', writer)
        snapshot = block(read(CLIENT / "core/scenarios/WeatherVisual.cpp"),
                         "ResolvedAtmosphereToJson(")
        for field in ("driven", "rain_intensity", "snow_intensity", "fog_density",
                      "storm_intensity", "wetness", "wind_direction", "wind_strength"):
            self.assertIn("weather." + field, snapshot)
        for field in ("enabled", "shadow_enabled", "coverage_amount", "biome_variation",
                      "plane_height", "shadow_strength", "scroll_offset", "sun_travel_dir"):
            self.assertIn("clouds." + field, snapshot)
        self.assertIn("pipeline.get_time_of_day()", snapshot)
        self.assertIn("pipeline.get_season_tick()", snapshot)
        self.assertIn("presentation_clock.capture_seconds()", snapshot)
        self.assertIn("particles->wind_velocity()", snapshot)


class DeferredDiagnosticLeaseWiring(unittest.TestCase):
    """Evaluate capture admission and guard every skipped deferred producer."""

    def setUp(self):
        self.targets = read(CLIENT / "rendering/GBufferTargets.cpp")
        self.passes = read(CLIENT / "rendering/passes/GBufferPass.cpp")

    def test_completion_requires_all_six_conditions(self):
        import itertools

        body = block(self.targets, "bool GBufferDiagnosticCapture::complete()")
        expression = re.fullmatch(r"\s*return\s+(.*?);\s*", body, re.S).group(1)
        for captured, framebuffer, lease, failed, refused, coverage in itertools.product(
                (False, True), repeat=6):
            with self.subTest(captured=captured, framebuffer=framebuffer, lease=lease,
                              failed=failed, refused=refused, coverage=coverage):
                actual = cpp_expression(expression, **{
                    "state_->captured": captured, "state_->framebuffer": framebuffer,
                    "state_->attachment_lease": lease, "state_->failed": failed,
                    "state_->refused": refused, "state_->table.complete_coverage()": coverage,
                    "true": True, "false": False})
                self.assertEqual(actual, captured and not framebuffer and not lease
                                 and not failed and not refused and coverage)

    def test_partial_snapshot_readiness_requires_valid_detached_storage(self):
        import itertools

        body = block(self.targets, "bool GBufferDiagnosticCapture::snapshot_ready()")
        expression = re.fullmatch(r"\s*return\s+(.*?);\s*", body, re.S).group(1)
        for captured, framebuffer, lease, failed, refused in itertools.product(
                (False, True), repeat=5):
            with self.subTest(captured=captured, framebuffer=framebuffer, lease=lease,
                              failed=failed, refused=refused):
                actual = cpp_expression(expression, **{
                    "state_->captured": captured, "state_->framebuffer": framebuffer,
                    "state_->attachment_lease": lease, "state_->snapshot_failed": failed,
                    "state_->refused": refused})
                self.assertEqual(actual, captured and not framebuffer and not lease
                                 and not failed and not refused)
        # Unsupported coverage must leave this independent storage diagnostic usable.
        self.assertNotIn("snapshot_failed", block(
            self.targets, "void GBufferDiagnosticCapture::unsupported("))
        for marker in ("void GBufferDiagnosticCapture::abort()",
                       "void GBufferDiagnosticCapture::before_destroy(",
                       "void GBufferDiagnosticCapture::detach_target()"):
            self.assertIn("state_->snapshot_failed = true;", block(self.targets, marker))
        for marker in ("bool GBufferDiagnosticCapture::attach(",
                       "bool GBufferDiagnosticCapture::attach_forward(",
                       "void GBufferDiagnosticCapture::detach()"):
            self.assertIn("abort();", block(self.targets, marker))

    def test_partial_snapshot_display_preserves_strict_publication_identity(self):
        source = read(CLIENT / "rendering/passes/DebugViewPass.cpp")
        finish = block(block(source, "class DebugViewPass::Frame"), "void finish(")
        self.assertIn("const bool ready = capture_->snapshot_ready();", finish)
        self.assertIn("identity_complete = capture_->complete();", finish)
        self.assertLess(finish.index("snapshot_ready()"), finish.index("capture_.reset()"))
        self.assertIn("return;", block(finish, "if (!ready)"))
        self.assertNotIn("if (!complete)", finish)
        gather = block(source, "void DebugViewPass::finish_frame(")
        self.assertIn('"Identity snapshot (incomplete coverage)"', gather)
        inspect = block(source, "void DebugViewPass::inspect_snapshot(")
        self.assertIn('{"identity_complete", m_snapshot->identity_complete}', inspect)
        self.assertIn("m_snapshot->identity_complete && kind != ProducerKind::Unsupported", gather)
        readback = block(read(CLIENT / "rendering/DiagnosticReadback.h"),
                         "class DiagnosticFrameCapture")
        self.assertIn("return scope_ ? scope_->complete() : identity_complete_;", readback)
        self.assertIn("identity_complete_ = ending->complete();", readback)
        self.assertIn("frame.identity_complete = identity_complete_;", readback)
        self.assertNotIn("snapshot_ready()", readback)
        bundle = read(CLIENT / "rendering/DiagnosticCapture.cpp")
        self.assertIn("!frame.identity_complete", bundle)
        self.assertIn("!draws.complete_deferred_coverage()", bundle)
        shader = block(read(ROOT / "res/shaders/debug_view.frag"), "vec3 identityColor()")
        self.assertIn("metadata.y == 0u) return unsupportedColor();", shader)
        self.assertIn("if (u_mode == 6) return diagnosticColor(id.x);", shader)

    def test_attachment_wraps_deferred_submission(self):
        execute = block(self.passes, "GBufferDrawStats GBufferPass::execute(")
        attachment = execute.index("GBufferDiagnosticAttachment diagnostic_attachment(")
        self.assertLess(execute.index("ReplayRollback restore_gl("), attachment)
        self.assertLess(execute.index("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)"),
                        attachment)
        for producer in ("geometry_pass_chunks(", "geometry_pass_static_meshes(",
                         "geometry_pass_skinned_meshes("):
            self.assertLess(attachment, execute.index(producer))
        # No extra block can release the attachment before the last producer.
        prefix = execute[:attachment]
        masked = re.sub(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"",
                        "", prefix, flags=re.S)
        self.assertEqual(masked.count("{"), masked.count("}"))

    def test_unavailable_base_program_invalidates_before_return(self):
        variant = block(self.targets, "Shader* GBufferDiagnosticCapture::variant(")
        unavailable = block(variant, "if (!shader || !shader->IsValid())")
        self.assertLess(unavailable.index('unsupported("diagnostic_base_shader_unavailable",'),
                        unavailable.index("return shader;"))
        for producer in ("geometry_pass_chunks(", "geometry_pass_static_meshes(",
                         "geometry_pass_skinned_meshes("):
            body = block(self.passes, "void GBufferPass::" + producer)
            self.assertLess(body.index("capture->variant("), body.index("UseDrawProgram("))
        skinned = block(self.passes, "void GBufferPass::geometry_pass_skinned_meshes(")
        self.assertLess(skinned.index("capture->variant("),
                        skinned.index("if (view.begin() == view.end())"))

    def test_queued_impostor_is_invalidated_before_program_refusal(self):
        static = block(self.passes, "void GBufferPass::geometry_pass_static_meshes(")
        queued = block(static, "if (impostorsOn && !impostorInstances.empty())")
        select = queued.index("RefusedImpostorProgram(m_tree_impostor_shader.get(), capture)")
        self.assertLess(select, queued.index("if (!UseDrawProgram("))
        self.assertIn("return;", block(queued, "if (!UseDrawProgram("))
        helper = block(self.passes, "Shader* RefusedImpostorProgram(")
        record = helper.index('capture->unsupported("tree_impostor_fragment_output_unavailable", 3)')
        self.assertLess(helper.index("if (!capture)"), record)
        self.assertIn("if (base && base->IsValid())", helper)
        self.assertLess(record, helper.index("return capture->variant(base, false, {}, true)"))
        self.assertIn("UseDrawProgram(impostor_program, stats)", queued)
        uniforms = queued[queued.index("UseDrawProgram("):]
        self.assertIn('impostor_program->setMat4("u_proj", static_proj)', uniforms)
        self.assertNotIn("m_tree_impostor_shader->set", uniforms)

    def test_uninstrumented_capture_wraps_original_fragment_and_separates_cache_modes(self):
        shader = block(read(CLIENT / "rendering/Shader.cpp"),
                       "AttributedGl::Program Shader::buildProgram(")
        self.assertIn('"LUMIN_DIAG_ZERO_IDS 1"', shader)
        rename = block(shader, "if (zero_ids && source == &fragmentCode)")
        self.assertIn('defines.emplace_back("main lumin_diagnostic_original_main")', rename)
        wrapper = block(shader, "if (zero_ids)")
        self.assertIn("#undef main", wrapper)
        self.assertIn("layout(location = 6) out uvec4", wrapper)
        self.assertLess(wrapper.index("lumin_diagnostic_original_main();"),
                        wrapper.index("lumin_diagnostic_zero = uvec4(0);"))
        variant = block(self.targets, "Shader* GBufferDiagnosticCapture::variant(")
        self.assertIn("entry.zero_ids == zero_ids", variant)
        self.assertIn('zero_ids ? "LUMIN_DIAG_ZERO_IDS 1" : "LUMIN_DIAG_IDS 1"', variant)
        fixture = read(ROOT / "test/rendering/diagnostic_inspector_test.cpp")
        draw = block(fixture, "void DrawImpostor(")
        for token in ('"tree_impostor.vert"', '"tree_impostor.frag"',
                      "capture.variant(&original, false, {}, true)",
                      "glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1)",
                      "glReadPixels(0, 0, 2, 2, GL_RGBA_INTEGER", "DiagnosticId{}"):
            self.assertIn(token, draw)
        self.assertNotIn("glTextureSubImage2D", draw)
        self.assertIn("ASSERT_NO_FATAL_FAILURE(DrawImpostor(*capture, coverage))", fixture)

    def test_unsafe_program_fallbacks_refuse_snapshot_before_return(self):
        variant = block(self.targets, "Shader* GBufferDiagnosticCapture::variant(")
        for marker in ("if (!shader || !shader->IsValid())",
                       "})) {",
                       "if (state_->variants.size() == 6)",
                       "if (!diagnostic->IsValid() || (!zero_ids && identity < 0))"):
            failure = block(variant, marker)
            self.assertLess(failure.index("abort();"), failure.index("return shader;"))
        writer = block(self.targets, "void GBufferDiagnosticCapture::write(")
        self.assertIn("abort();", block(writer, "if (entry.zero_ids)"))
        for signature in ("void GBufferDiagnosticCapture::write(",
                          "void GBufferDiagnosticCapture::write_current("):
            body = block(self.targets, signature)
            self.assertRegex(body, r'"diagnostic_writer_without_variant"[^;]*;\s*abort\(\);')

    def test_detached_writers_and_variants_do_no_work(self):
        for signature, work, returned in (
                ("Shader* GBufferDiagnosticCapture::variant(", "if (!shader", "return shader;"),
                ("void GBufferDiagnosticCapture::write(", "for (const auto& entry", "return;")):
            body = block(self.targets, signature)
            guard = "if (!state_->framebuffer)"
            self.assertLess(body.index(guard), body.index(work))
            self.assertIn(returned, block(body, guard))

    def test_unsupported_submission_failure_is_sticky(self):
        writer = block(self.targets, "void GBufferDiagnosticCapture::write(")
        unsupported = block(writer, "if (submission.provenance == DiagnosticProvenance::Unsupported)")
        self.assertIn("state_->failed = true;", unsupported)
        self.assertLess(writer.index("submission.provenance"),
                        writer.index("state_->table.submit(submission, id)"))


class DeferredRasterDiagnosticWiring(unittest.TestCase):
    """Read shipping shader/state paths; GL pixel execution stays in pass_context_test."""

    def setUp(self):
        self.lighting = read(ROOT / "res/shaders/lighting_pass.frag")
        self.fragment = read(ROOT / "res/shaders/g_buffer.frag")
        self.overlay = read(ROOT / "res/shaders/debug_view.frag")
        self.debug = read(CLIENT / "rendering/passes/DebugViewPass.cpp")
        self.gbuffer = read(CLIENT / "rendering/passes/GBufferPass.cpp")

    def test_cascade_observation_matches_actual_selector_at_every_split(self):
        selector = block(self.lighting, "float CalculateShadow(")
        tint = block(self.lighting, "vec3 SampleShadowTint(")
        increments = re.findall(r"cascadeIndex \+= \((viewDepth > u_cascadeSplits\.[xyz])\) \? 1: 0;", selector)
        self.assertEqual(increments, re.findall(
            r"cascadeIndex \+= \((viewDepth > u_cascadeSplits\.[xyz])\) \? 1: 0;", tint))
        self.assertEqual(len(increments), 3)
        self.assertIn("u_lightSpaceMatrices[cascadeIndex] * vec4(fragPos, 1.0)", selector)
        observation = re.search(r"diagnosticCascade = vec3\(float\((.*?)\), projCoords.xy\);", selector)
        self.assertIsNotNone(observation)
        for depth, expected in ((0, 0), (1, 0), (1.0001, 1), (2, 1),
                                (2.0001, 2), (3, 2), (3.0001, 3), (8, 3)):
            with self.subTest(depth=depth):
                selected = sum(cpp_expression(expression, **{
                    "viewDepth": depth, "u_cascadeSplits.x": 1,
                    "u_cascadeSplits.y": 2, "u_cascadeSplits.z": 3})
                               for expression in increments)
                self.assertEqual(selected, expected)
                self.assertEqual(cpp_expression(observation.group(1), cascadeIndex=selected), selected)
        self.assertLess(selector.index("diagnosticCascade = vec3(float("), selector.index("// 3. PCF"))

    def test_shadow_uv_and_footprint_come_from_the_production_projection(self):
        selector = block(self.lighting, "float CalculateShadow(")
        for statement in ("diagnosticCascade = vec3(float(cascadeIndex), projCoords.xy);",
                          "vec2 shadowSize = vec2(textureSize(u_shadowCascades, 0).xy);",
                          "u_lightSpaceMatrices[cascadeIndex] * vec4(fragPos + diagnosticDpdx, 1.0)",
                          "u_lightSpaceMatrices[cascadeIndex] * vec4(fragPos + diagnosticDpdy, 1.0)",
                          "vec2 uvDx = (lightDx.xy / lightDx.w) * 0.5 + 0.5 - projCoords.xy;",
                          "vec2 uvDy = (lightDy.xy / lightDy.w) * 0.5 + 0.5 - projCoords.xy;",
                          "max(length(uvDx * shadowSize), length(uvDy * shadowSize))",
                          "diagnosticShadowUv = vec3(projCoords.xy, log2(max(footprint, 1e-8)));",
                          "diagnosticCascade = vec3(insideAny ? -1.0 : -3.0);"):
            self.assertIn(statement, selector)
        self.assertIn("candidate < 4", selector)
        self.assertIn("light.w != 0.0 && projected.z >= 0.0 && projected.z <= 1.0", selector)
        self.assertIn("all(greaterThanEqual(projCoords, vec3(0.0)))", selector)
        self.assertIn("vec3 diagnosticCascade = vec3(-2.0);", self.lighting)
        self.assertIn("u_contributionMode == 34) diagnosticValue = diagnosticCascade", self.lighting)
        self.assertIn("u_contributionMode == 35) diagnosticValue = diagnosticShadowUv", self.lighting)
        for sentinel in (-1, -2, -3):
            self.assertIn(f"all(equal(contribution.rgb, vec3({sentinel}.0)))", self.overlay)

    def test_shadow_footprint_derivatives_precede_all_main_branches_and_keep_sun_observation(self):
        main = block(self.lighting, "void main()")
        prefix = main[:main.index("// Alpha zero")]
        self.assertNotRegex(re.sub(r"//[^\n]*", "", prefix),
                            r"\b(?:if|for|while|return|discard)\b|\?")
        for axis in ("x", "y"):
            derivative = f"diagnosticDpd{axis} = dFd{axis}(FragPos);"
            self.assertEqual(self.lighting.count(f"dFd{axis}(FragPos)"), 1)
            self.assertIn(derivative, prefix)
            self.assertNotIn(f"dFd{axis}(", block(self.lighting, "float CalculateShadow("))
        self.assertIn("bool diagnosticValid = diagnosticPositionAt(TexCoords, diagnosticViewPos);", prefix)
        self.assertIn("vec3 FragPos = vec3(u_inverseView * vec4(diagnosticViewPos, 1.0));", prefix)
        globals = self.lighting[:self.lighting.index("float CalculateShadow(")]
        for declaration in ("vec3 diagnosticDpdx;", "vec3 diagnosticDpdy;",
                            "bool diagnosticShadowObserved = false;"):
            self.assertIn(declaration, globals)
        selector = block(self.lighting, "float CalculateShadow(")
        observation = block(selector, "if (!diagnosticShadowObserved)")
        self.assertIn("diagnosticShadowObserved = true;", observation)
        self.assertIn("diagnosticCascade = vec3(float(cascadeIndex), projCoords.xy);", observation)
        self.assertIn("diagnosticShadowUv = vec3(projCoords.xy, log2(max(footprint, 1e-8)));", observation)
        self.assertLess(main.index("CalculateShadow(FragPos, Normal, L_sun"),
                        main.index("CalculateShadow(FragPos, Normal, L_moon"))

    def test_shadow_footprint_rejects_every_incomplete_quad_with_the_existing_sentinel(self):
        validity = block(self.lighting, "bool diagnosticPositionAt(")
        for statement in ("farWorldPosition(gPosition, u_sceneDepth, uv, position)",
                          "reconstructed && !any(isnan(position)) && !any(isinf(position))",
                          "dot(position, position) >= 1e-8"):
            self.assertIn(statement, validity)
        main = block(self.lighting, "void main()")
        prefix = main[:main.index("// Alpha zero")]
        for statement in (
                "float diagnosticNeighbourOffset = 1.0 - 2.0 * mod(floor(gl_FragCoord.y), 2.0);",
                "vec2 diagnosticNeighbourUv = TexCoords + dFdyFine(TexCoords) * diagnosticNeighbourOffset;",
                "bool diagnosticNeighbourValid = diagnosticPositionAt(diagnosticNeighbourUv, diagnosticNeighbourPos);",
                "float diagnosticColumnValid = float(diagnosticValid && diagnosticNeighbourValid);",
                "float diagnosticValidityDx = dFdxFine(diagnosticColumnValid);"):
            self.assertIn(statement, prefix)
        predicate = re.search(r"diagnosticQuadValid = ([^;]+);", prefix).group(1)
        # Fine horizontal differences of paired columns include the diagonal,
        # without undefined higher-order derivatives or divergent derivative calls.
        for mask in range(16):
            columns = [float(bool(mask & (1 << x)) and bool(mask & (1 << (x + 2))))
                       for x in range(2)]
            for lane in range(4):
                with self.subTest(mask=mask, lane=lane):
                    self.assertEqual(cpp_expression(
                        predicate, diagnosticColumnValid=columns[lane % 2],
                        diagnosticValidityDx=columns[1] - columns[0]), mask == 15)
        selector = block(self.lighting, "float CalculateShadow(")
        supported = block(selector, "if (diagnosticQuadValid)")
        self.assertIn("float footprint =", supported)
        self.assertIn("diagnosticShadowUv = vec3(projCoords.xy, log2(max(footprint, 1e-8)));",
                      supported)
        self.assertIn("vec3 diagnosticShadowUv = vec3(-2.0);", self.lighting)
        self.assertIn("if (diagnosticQuadValid) diagnosticShadowUv = diagnosticCascade;", selector)
        self.assertIn("all(equal(contribution.rgb, vec3(-2.0)))) value = unsupportedColor();",
                      self.overlay)

    def test_wireframe_depth_policy_and_full_restoration_are_wired(self):
        attach = block(self.debug, "void attach(")
        detach = block(self.debug, "void detach()")
        self.assertIn("glPolygonMode(GL_FRONT_AND_BACK, view == Overdraw ? GL_FILL : GL_LINE);", attach)
        self.assertIn("DebugViewPass::replay_depth_policy();", attach)
        self.assertIn("glDepthMask(GL_FALSE);", block(self.debug, "void DebugViewPass::replay_depth_policy()"))
        self.assertLess(attach.index("std::make_unique<ReplayGlGuard>"),
                        attach.index("glPolygonMode("))
        self.assertIn('entry.shader->setInt("u_surfaceMode", view == Wireframe ? RawFinite : view);',
                      self.debug)
        self.assertIn('gDiagnosticSurface = vec4(diagnosticValue, diagnosticRasterDepth);',
                      self.fragment)
        self.assertIn("replay_guard.reset();", detach)
        self.assertLess(detach.index("glFramebufferTexture2D("), detach.index("replay_guard.reset();"))
        policy = block(self.debug, "void DebugViewPass::replay_depth_policy()")
        self.assertIn("glDepthFunc(GL_GEQUAL);", policy)
        self.assertIn("glDepthMask(GL_FALSE);", policy)
        chunks = block(self.gbuffer, "void GBufferPass::geometry_pass_chunks(")
        self.assertLess(chunks.index("uniforms.Prepare(command)"),
                        chunks.index("DebugViewPass::replay_depth_policy();"))
        self.assertIn("replay.a >= texelFetch(gDepth, pixel, 0).r", self.overlay)
        execute = block(self.debug, "void DebugViewPass::execute(")
        self.assertIn("overlay_guard = std::make_unique<ReplayGlGuard>", execute)
        self.assertIn("GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE", execute)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        dispatch = block(pipeline, "void RenderPipeline::dispatch_stages(")
        self.assertIn("DebugViewPass::controls().wireframe = m_frame_prepared.wireframe;", dispatch)
        self.assertNotIn("GL_LINE", dispatch)
        # Every live geometry frame invalidates its previous plane, even when an
        # identity owner or a replay refusal prevents the current frame from drawing.
        self.assertIn("DebugViewPass::geometry_requested() ||", dispatch)
        self.assertLess(dispatch.index("DebugViewPass::geometry_requested() ||"),
                        dispatch.index("m_debug_view_pass->begin_frame("))
        mdi = block(pipeline, "void RenderPipeline::draw_chunks_mdi(")
        self.assertIn("ReplayRollback restore_cursor", mdi)
        self.assertIn("m_mdi_stream.restore_cursor(saved_cursor);", block(mdi, "if (diagnostic_replay)"))

    def test_overdraw_shader_pins_match_shipping_sources(self):
        import hashlib

        manifest = json.loads(read(ROOT / "test/rendering/shader_scenes/scenes.json"))
        for name, inventory in (("g_buffer.frag", "inputs"),
                                ("debug_view.frag", "diagnostic_shader_sha256")):
            path = "res/shaders/" + name
            payload = (ROOT / path).read_bytes().replace(b"\r\n", b"\n")
            self.assertEqual(manifest[inventory][path], hashlib.sha256(payload).hexdigest())

    def test_overdraw_adds_one_per_surviving_fragment_with_depth_off(self):
        attach = block(self.debug, "void attach(")
        additive = block(attach, "if (view == Overdraw)")
        for statement in ("glEnablei(GL_BLEND, 6);",
                          "glBlendFuncSeparatei(6, GL_ONE, GL_ONE, GL_ONE, GL_ONE);",
                          "glBlendEquationSeparatei(6, GL_FUNC_ADD, GL_FUNC_ADD);"):
            self.assertIn(statement, additive)
        policy = block(self.debug, "void DebugViewPass::replay_depth_policy()")
        self.assertIn("glDisable(GL_DEPTH_TEST);", block(policy,
                      "if (controls().surface_owner->m_surface->view == Overdraw)"))
        self.assertIn("glDepthMask(GL_FALSE);", policy)
        self.assertIn("outputs.fill(GL_NONE)", attach)
        self.assertIn("if (u_surfaceMode == 37) gDiagnosticSurface = vec4(1.0, 0.0, 0.0, 0.0);",
                      self.fragment)
        self.assertGreater(self.fragment.index("if (u_surfaceMode == 37)"),
                           self.fragment.rindex("discard;"))
        header = read(CLIENT / "rendering/passes/DebugViewPass.h")
        self.assertIn("Overdraw = 37", header)
        self.assertIn('"overdraw"', header)
        self.assertIn("return view == Wireframe || view == Overdraw;", header)
        self.assertIn("control.view == Overdraw ? None", header)

    def test_overdraw_ramp_reserves_saturation_and_does_not_read_final_colour(self):
        count = block(self.overlay, "if (u_mode == 37)")
        self.assertIn("count >= 16.0 ? vec3(0.0, 1.0, 1.0)", count)
        self.assertIn("count < 1.0 ? vec3(0.0)", count)
        self.assertIn("(count - 1.0) / 14.0", count)
        self.assertIn("(16.0 - count) / 15.0", count)
        self.assertIn("texelFetch(gContribution, pixel, 0).r", count)
        for source in ("gAlbedo", "gDepth", "gPosition", "gNormalMaterial"):
            self.assertNotIn(source, count)
        legend = block(self.debug, "std::string_view DebugViewPass::contribution_legend(")
        for label in ("depth test OFF", ">=16=cyan saturation", "authored/impostor/forward/sky unavailable"):
            self.assertIn(label, legend)

    def test_overdraw_inspection_reads_one_live_top_left_pixel_and_refuses_stale_values(self):
        count = block(self.overlay, "if (u_mode == 37)")
        execute = block(self.debug, "void DebugViewPass::execute(")
        begin = block(self.debug, "DebugViewPass::begin_frame(")
        geometry = block(begin, "if (geometry_requested())")
        self.assertIn("return {};", geometry)
        self.assertNotIn("inspect_requested", geometry)
        self.assertLess(begin.index("if (geometry_requested())"),
                        begin.index("std::exchange(control.inspect_requested, false)"))
        self.assertIn("mode() == Overdraw && std::exchange(controls().inspect_requested, false)", execute)
        self.assertNotIn("inspector_open", execute)
        self.assertIn("all(equal(ivec2(gl_FragCoord.xy), ivec2(0)))", count)
        self.assertIn("texelFetch(gContribution, ivec2(u_pixelX, u_pixelY), 0).r", count)
        self.assertIn("inspectorNormalDepth = vec4(selected, 0.0, 0.0, float(u_contributionReady));", count)
        self.assertIn("static_cast<int>(ctx.internal_h()) - 1 - controls().y", execute)
        self.assertIn("contribution_extent(ctx.internal_w(), ctx.internal_h())", execute)
        self.assertIn("glNamedBufferSubData(m_snapshot->sample_buffer, 0, sizeof(empty), &empty);", execute)
        self.assertIn("read_inspector_pixel(m_snapshot->sample_buffer, pixel);", execute)
        self.assertIn("if (pixel.normal_depth[3] == 1.0f)", execute)
        self.assertIn("Overdraw count >=16 (saturated); depth test OFF", execute)
        self.assertIn("Overdraw count unavailable (producer, target or selected pixel)", execute)
        self.assertIn('if (mode() != Overdraw || !controls().result.starts_with("Overdraw count "))', execute)
        self.assertIn("controls().result.clear();", block(execute, "if (mode() != Overdraw"))
        self.assertLess(execute.index("glNamedBufferSubData("), execute.index("AttributedGl::Submit("))
        self.assertLess(execute.index("glMemoryBarrier("), execute.index("read_inspector_pixel("))

    def test_replay_refusal_matrix_precedes_every_allocation_and_gl_call(self):
        import itertools

        scope = block(self.debug, "DebugViewPass::SurfaceFrame::SurfaceFrame(")
        guard = re.search(r"if \((.*?)\) \{\s*return;", scope, re.S)
        self.assertIsNotNone(guard)
        for requested, selected_replay, replay, frozen, capture, owner in itertools.product(
                (False, True), repeat=6):
            actual = cpp_expression(guard.group(1), **{
                "geometry_requested()": requested,
                "replay_mode(geometry_view())": selected_replay, "replay": replay,
                "ctx.frozen_replay": frozen, "GBufferDiagnosticCapture::Current()": capture,
                "control.surface_owner": owner})
            self.assertEqual(actual, not requested or selected_replay != replay
                             or frozen or capture or not owner)
        self.assertLess(guard.end(), scope.index("std::make_unique<SurfaceState>"))
        self.assertLess(guard.end(), scope.index("->attach(ctx, gbuffer)"))
        gbuffer = block(self.gbuffer, "GBufferDrawStats GBufferPass::execute(")
        replay = block(gbuffer, "if (DebugViewPass::geometry_requested()")
        self.assertNotIn("glClear(", replay)
        self.assertIn("if (replay.ready())", replay)
        self.assertIn("ReplayRollback restore_timing", replay)
        self.assertIn("m_last_static_prop_cpu_ms = production_static_cpu_ms;", replay)
        for producer in ("geometry_pass_chunks(", "geometry_pass_static_meshes(", "geometry_pass_skinned_meshes("):
            self.assertEqual(replay.count(producer), 1)

    def test_snapshot_lifecycle_keeps_current_context_and_single_lease_check(self):
        begin = block(self.debug, "DebugViewPass::begin_frame(")
        finish = block(self.debug, "void DebugViewPass::end_frame(")
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        dispatch = block(pipeline, "void RenderPipeline::dispatch_stages(")
        self.assertIn("auto diagnostic_frame =", dispatch)
        self.assertNotIn("const auto diagnostic_frame", dispatch)
        self.assertIn("return std::make_shared<Frame>(*this);", begin)
        self.assertNotIn("std::make_shared<Frame>(*this, ctx)", begin)
        self.assertEqual(begin.count("m_snapshot->targets.reset()"), 1)
        self.assertLess(begin.index("if (!m_snapshot->targets.reset())"),
                        begin.index("m_snapshot->draws.clear();"))
        self.assertIn("Identity attachment still leased", begin)
        self.assertLess(finish.index("frame->finish(ctx);"), finish.index("frame.reset();"))
        self.assertLess(dispatch.index("(this->*fn)(camera);"),
                        dispatch.index("DebugViewPass::end_frame("))
        chunks = block(self.gbuffer, "void GBufferPass::geometry_pass_chunks(")
        self.assertEqual(chunks.count("uniforms.Prepare(command)"), 1)
        self.assertLess(chunks.index("if (!uniforms.Prepare(command))"),
                        chunks.index("diagnostic_draw.prepare(command);"))
        self.assertNotIn('capture->unsupported("authored_static_surface")', self.gbuffer)

    def test_fresh_live_overlays_prepare_and_restore_bounded_inspector_storage(self):
        execute = block(self.debug, "void DebugViewPass::execute(")
        prepare = block(self.debug, "bool DebugViewPass::prepare_inspector_storage()")
        self.assertIn("if (contribution_view && !prepare_inspector_storage())", execute)
        refusal = block(execute, "if (contribution_view && !prepare_inspector_storage())")
        self.assertEqual(compact(refusal), "return;")
        self.assertLess(execute.index("if (contribution_view && !prepare_inspector_storage())"),
                        execute.index("identity_view && (!m_snapshot"))
        self.assertLess(execute.index("if (!shader || !shader->IsValid() || m_vao == 0)"),
                        execute.index("if (contribution_view && !prepare_inspector_storage())"))
        predicate = re.search(r"const bool identity_view =\s*(.*?);", execute, re.S).group(1)
        for enabled in (False, True):
            for wireframe in (False, True):
                for mode in (0, 5, 6, 12, 30, 34, 36, 37):
                    for contribution in (False, True):
                        self.assertEqual(cpp_expression(predicate, **{
                            "controls().enabled": enabled, "controls().wireframe": wireframe,
                            "mode()": mode, "DrawId": 6, "contribution_view": contribution}),
                            (enabled or wireframe) and mode >= 6 and not contribution)
        self.assertIn("InspectorStorageBinding bindings;", prepare)
        self.assertIn("if (m_snapshot->sample_buffer == 0)", prepare)
        self.assertIn("sizeof(InspectorPixel), &empty, GL_STREAM_READ", compact(prepare))
        guard = re.search(r"if \((sample_bytes != sizeof\(InspectorPixel\))\)", prepare)
        self.assertIsNotNone(guard)
        for size in (-1, 0, 47, 48, 49, 96):
            self.assertEqual(cpp_expression(guard.group(1), **{
                "sample_bytes": size, "sizeof(InspectorPixel)": 48}), size != 48)
        invalid = block(prepare, "if (sample_bytes != sizeof(InspectorPixel))")
        self.assertIn("m_snapshot->reset_sample();", invalid)
        self.assertIn("return false;", invalid)
        self.assertLess(execute.index("storage_binding.emplace();"),
                        execute.index("glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7,"))
        self.assertLess(execute.index("glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7,"),
                        execute.index("AttributedGl::Submit("))

    def test_per_frame_mode_update_retains_storage_for_the_wireframe_toggle(self):
        setter = block(self.debug, "void DebugViewPass::set_mode(")
        guard = re.search(r"if \((.*?)\) \{", setter).group(1)
        for snapshot in (False, True):
            for enabled in (False, True):
                for wireframe in (False, True):
                    self.assertEqual(cpp_expression(guard, **{
                        "m_snapshot": snapshot, "controls().enabled": enabled,
                        "controls().wireframe": wireframe}),
                        snapshot and not enabled and not wireframe)
        self.assertIn("m_snapshot.reset();", setter)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        self.assertIn("m_debug_view_pass->set_mode(mode);",
                      block(pipeline, "void RenderPipeline::set_debug_view("))
        client = read(CLIENT / "main_client.cpp")
        self.assertIn("renderPipeline.set_debug_view(", client)

    def test_off_shader_bytes_keep_the_fixed_production_oracles(self):
        def without_guard(source, guard):
            pattern = re.compile(r"^#ifdef " + guard + r"\n(.*?)^#endif\n", re.M | re.S)
            for match in pattern.finditer(source):
                self.assertNotRegex(match.group(1), r"^#(?:if|else)")
            return pattern.sub("", source)

        def fingerprint(source):
            value = 14695981039346656037
            for byte in source.encode("utf-8"):
                value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
            return value

        self.assertEqual(fingerprint(without_guard(self.lighting, "LUMIN_DIAG_CONTRIBUTIONS")),
                         0xc6e9bc9c6fc7110b)
        self.assertEqual(fingerprint(without_guard(self.fragment, "LUMIN_DIAG_SURFACE")),
                         0x7289f9525f762361)


class LaterDiagnosticSubmissionWiring(unittest.TestCase):
    """Guard original submissions, disabled rendering and continuation refusals."""

    def setUp(self):
        self.source = read(CLIENT / "rendering/GBufferTargets.cpp")
        self.attach = block(self.source, "bool GBufferDiagnosticCapture::attach_target(")

    @staticmethod
    def without_diagnostics(source):
        while "#ifdef LUMIN_DIAG_IDS\n" in source:
            start = source.index("#ifdef LUMIN_DIAG_IDS\n")
            end = source.index("#endif\n", start)
            alternate = source.find("#else\n", start, end)
            if alternate >= 0:
                source = source[:start] + source[alternate + 6:end] + source[end + 7:]
            else:
                # Identity declarations introduce one separator absent from the base.
                if source[start - 2:start] == "\n\n":
                    start -= 1
                source = source[:start] + source[end + 7:]
        return source

    def test_empty_forward_passes_do_not_invalidate_scenario_capture(self):
        owner = block(read(CLIENT / "rendering/DiagnosticReadback.h"),
                      "explicit DiagnosticFrameCapture(")
        self.assertIn("GBufferDiagnosticCapture>(targets_, draws_)", owner)
        for name, sequence in (("WaterPass", "input.draw_items"),
                               ("WaterfallPass", "m_live_factors")):
            with self.subTest(producer=name):
                source = read(CLIENT / f"rendering/passes/{name}.cpp")
                draw = block(source, f"{name}::execute(")
                setup = block(draw, "if (auto* capture = GBufferDiagnosticCapture::Current())")
                self.assertIn("if (std::any_of(", setup)
                predicate_body = block(setup, "if (std::any_of(")
                gate = block(setup, "}))")
                self.assertIn(f"{sequence}.begin(), {sequence}.end()", compact(setup))
                self.assertIn("diagnostic_attachment.emplace(", gate)
                self.assertIn("draw_shader = capture->variant(draw_shader, false)", gate)
                self.assertEqual(setup.count("diagnostic_attachment.emplace("), 1)
                self.assertIn("capture->write_current(", draw)
                predicate = re.search(r"return (.*?);", predicate_body).group(1)
                if name == "WaterPass":
                    conversion = block(source, "GLsizei WaterElementCount(")
                    refusal = re.search(r"if \((.*?)\) \{", conversion).group(1)
                    self.assertIn("return 0;", block(conversion, "if ("))
                    self.assertIn("return static_cast<GLsizei>(count);", conversion)
                    self.assertIn("const GLsizei count = WaterElementCount(item.element_count);", draw)
                    self.assertIn("continue;", block(draw, "if (count == 0)"))
                    cases = ((), (0,), (2**32 - 1,), (0, 2**32 - 1),
                             (0, 2**32 - 1, 3), (3, 0, 2**32 - 1), (2**31 - 1,))
                    for counts in cases:
                        selected = []
                        for count in counts:
                            invalid = cpp_expression(refusal, **{
                                "count": count,
                                "static_cast<u32>(std::numeric_limits<GLsizei>::max())": 2**31 - 1})
                            converted = 0 if invalid else count
                            selected.append(cpp_expression(predicate, **{
                                "WaterElementCount(item.element_count)": converted}))
                        self.assertEqual(any(selected), any(0 < count <= 2**31 - 1 for count in counts))
                else:
                    skip = re.search(r"if \((live .*?)\) \{", draw).group(1)
                    self.assertIn("continue;", block(draw, "if (live < 0.02f)"))
                    for factors in ((), (0,), (0.019,), (0.02,), (0, 1), (1, 0),
                                    (math.nan,), (-math.inf,), (math.inf,)):
                        selected = [cpp_expression(re.sub(r"(?<=\d)f\b", "", predicate), live=value)
                                    for value in factors]
                        submitted = [not cpp_expression(re.sub(r"(?<=\d)f\b", "", skip), live=value)
                                     for value in factors]
                        self.assertEqual(selected, submitted)
        glass = block(read(CLIENT / "rendering/passes/GlassOitPass.cpp"),
                      "void GlassOitPass::execute_accum(")
        self.assertIn("return;", block(glass, "if (!input.glass_items || input.glass_items->empty()"))
        self.assertLess(glass.index("if (!input.glass_items || input.glass_items->empty()"),
                        glass.index("diagnostic_attachment.emplace("))

    def test_plant_and_decal_keep_shipping_shaders_when_disabled(self):
        import hashlib
        for name, expected in (
                ("plant_procgen", "5de3e367b62a29914f16b3014f84979b104765db67b97766cacf3c5fd5d4a3ed"),
                ("scent_decal", "00536af5f2a9d77d493e4ebbe4accfd3bf0301d9882e9c36a5cba1a75d4d755e")):
            path = f"res/shaders/{name}.frag"
            source = read(ROOT / path)
            self.assertEqual(hashlib.sha256(self.without_diagnostics(source).encode()).hexdigest(),
                             expected)
            self.assertIn("layout (location = 6) out uvec4 gDiagnosticIds;", source)
            self.assertIn("gDiagnosticIds = u_diagnosticIds;", source)

    def test_disabled_submissions_preserve_original_uniforms_and_draw_arguments(self):
        branch = "if (auto* capture = GBufferDiagnosticCapture::Current())"
        for name, expected in (("PlantProcgenPass", 0x17bde0693dc08760),
                               ("GroundDecalPass", 0x46646c0093fc3f8a)):
            source = read(CLIENT / f"rendering/passes/{name}.cpp")
            draw = "{" + block(source, f"void {name}::execute(") + "}"
            for stage in range(2):
                inner = block(draw, branch)
                begin = (draw.index("    Shader* draw_shader = m_shader.get();")
                         if stage == 0 else draw.index("    " + branch))
                end = draw.index(inner, draw.index(branch)) + len(inner) + (3 if stage == 0 else 2)
                draw = draw[:begin] + draw[end:]
            draw = re.sub(r'    if \(draw_shader != m_shader.get\(\)\) \{\n.*?\n    \}\n',
                          '    m_shader->setMat4("projection", projection);\n', draw, flags=re.S)
            draw = draw.replace("draw_shader->", "m_shader->")
            digest = 14695981039346656037
            for byte in draw.encode():
                digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
            self.assertEqual(digest, expected)

    def test_plant_attributes_one_original_uploaded_batch(self):
        draw = block(read(CLIENT / "rendering/passes/PlantProcgenPass.cpp"),
                     "void PlantProcgenPass::execute(")
        for early in ("if (!m_enabled || !m_has_geometry || m_index_count == 0)",
                      "if (!m_shader || !m_shader->IsValid() || m_vao == 0)"):
            self.assertLess(draw.index(early), draw.index("GBufferDiagnosticAttachment"))
            self.assertIn("return;", block(draw, early))
        setup = block(draw, "if (auto* capture = GBufferDiagnosticCapture::Current())")
        for text in ("GL_DRAW_FRAMEBUFFER_BINDING", "GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME",
                     "target.position_texture = static_cast<GLuint>(position_texture)",
                     "diagnostic_attachment.emplace(target,", "capture->variant(draw_shader, false)"):
            self.assertIn(text, setup)
        identity = draw[draw.index("const std::string mesh_key"):]
        for text in ("std::to_string(m_signature)", "std::to_string(m_index_count)",
                     '.shader_variant = "plant_procgen"', ".pass = DiagnosticPass::GBuffer",
                     ".generated_mesh = mesh_key"):
            self.assertIn(text, identity)
        self.assertLess(identity.index("capture->write("), identity.index("Submit(glDrawElements,"))
        self.assertEqual(draw.count("Submit(glDrawElements,"), 1)

    def test_decal_uses_forward_storage_and_discards_noncontributors(self):
        draw = block(read(CLIENT / "rendering/passes/GroundDecalPass.cpp"),
                     "void GroundDecalPass::execute(")
        self.assertIn("diagnostic_attachment.emplace(static_cast<GLuint>(framebuffer),", draw)
        self.assertIn("ctx.gbuffer_position.id", draw)
        self.assertIn('capture->variant(draw_shader, false)', draw)
        self.assertIn(".pass = DiagnosticPass::Forward", draw)
        self.assertIn('.generated_mesh = "scent-decal/fullscreen-triangle"', draw)
        self.assertLess(draw.index("capture->write_current("), draw.index("Submit(glDrawArrays,"))
        self.assertEqual(draw.count("Submit(glDrawArrays,"), 1)
        shader = read(ROOT / "res/shaders/scent_decal.frag")
        guarded = re.findall(r"#ifdef LUMIN_DIAG_IDS\n(.*?)#else\n", shader, re.S)
        self.assertEqual(len(guarded), 4)
        for fragment in guarded:
            self.assertIn("discard;", fragment)
        for condition in ("!farWorldPosition(gPosition, gDepth, TexCoords, viewPos)",
                          "dot(viewPos, viewPos) < 1e-8", "intensity <= 0.0025"):
            self.assertIn(f"if ({condition}) {{ discard; }}", shader)
        self.assertLess(shader.rindex("discard;"), shader.index("gDiagnosticIds ="))

    def test_continuations_preserve_ids_and_original_color_routing(self):
        self.assertIn("const bool already_captured = forward ? state_->forward_captured : state_->captured",
                      self.attach)
        clear = block(self.attach, "if (!already_captured)")
        self.assertIn("glClearTexImage(", clear)
        self.assertIn("state_->attachment_lease.texture()", clear)
        self.assertEqual(self.attach.count("glClearTexImage("), 1)
        self.assertLess(self.attach.index("if (!complete)"),
                        self.attach.index("if (!already_captured)"))
        routing = block(self.attach, "if (forward || already_captured)")
        for text in ("auto forward_buffers = state_->buffers", "forward_buffers[6] = GL_COLOR_ATTACHMENT6",
                     "glDrawBuffers(state_->buffer_count, forward_buffers.data())"):
            self.assertIn(text, routing)

    def test_extent_refusal_precedes_storage_mutation_for_both_planes(self):
        condition = re.search(r"if \((already_captured &&.*?)\) \{", self.attach, re.S).group(1)
        for captured in (False, True):
            for width, height in ((0, 0), (16, 32), (32, 16), (32, 32), (33, 32)):
                self.assertEqual(cpp_expression(condition, **{
                    "already_captured": captured, "targets.extent().width": 32,
                    "targets.extent().height": 32, "extent.width": width, "extent.height": height}),
                    captured and (width != 32 or height != 32))
        refusal = block(self.attach, "if (already_captured &&")
        self.assertIn('state_->failure = "diagnostic_continuation_extent_mismatch";', refusal)
        self.assertIn("return false;", refusal)
        self.assertLess(self.attach.index("diagnostic_continuation_extent_mismatch"),
                        self.attach.index("targets.ensure("))
        self.assertLess(self.attach.index("targets.ensure("),
                        self.attach.index("targets.acquire_attachment_lease()"))

    def test_forward_fallback_keeps_metadata_and_stops_after_refusal(self):
        forward = block(self.source, "bool GBufferDiagnosticCapture::attach_forward(")
        missing = block(forward, "if (!state_->forward_targets)")
        self.assertIn("abort();", missing)
        self.assertIn("return false;", missing)
        self.assertNotIn("unsupported(", missing)
        writer = block(self.source, "void GBufferDiagnosticCapture::write_current(")
        contributor = block(writer, "const auto record_contributor = [&]")
        self.assertIn("abort();", contributor)
        self.assertIn("return;", block(contributor, "if (state_->refused)"))
        self.assertLess(contributor.index("if (state_->refused)"),
                        contributor.index("const std::string label"))
        condition = re.search(r"if \((submission.shader_variant.empty\(\).*?)\) \{",
                              contributor, re.S).group(1)
        for size in (0, 1, 1007, 1008, 1024, 1025):
            self.assertEqual(cpp_expression(condition, **{
                "submission.shader_variant.empty()": size == 0,
                "submission.shader_variant.size()": size, "suffix.size()": 17}),
                size == 0 or size > 1007)
        self.assertIn('suffix = ":contributor_only"', contributor)
        self.assertIn('"diagnostic_contributor_label_limit"', contributor)
        self.assertLess(contributor.index("1024 - suffix.size()"),
                        contributor.index("const std::string label"))
        self.assertIn("auto contributor = submission;", contributor)
        self.assertIn("contributor.shader_variant = label;", contributor)
        refused = block(contributor, "if (!state_->table.submit(contributor, id))")
        self.assertIn('"diagnostic_contributor_identity_refused"', refused)
        self.assertIn("state_->refused = true;", refused)
        self.assertNotIn("throw", contributor)
        self.assertEqual(writer.count("record_contributor();"), 2)
        detached = block(writer, "if (!state_->framebuffer)")
        self.assertIn("record_contributor();", block(detached,
                      "if (submission.pass == DiagnosticPass::Forward)"))
        self.assertLess(writer.index("write(*entry.shader, submission)"),
                        writer.rindex("record_contributor();"))


class TerrainCaptureResourceWiring(unittest.TestCase):
    """Evaluate capture resource wiring and scalar guards; no native GL execution."""

    def setUp(self):
        self.source = read(CLIENT / "rendering/TerrainCoverageCapture.cpp")
        self.capture = block(self.source, "bool RenderPipeline::capture_terrain_coverage(")
        self.replay = block(self.source, "bool CaptureFog(")

    def test_manifest_retains_lighting_sources_and_uploaded_uniform_snapshots(self):
        capture = compact(self.capture)
        records = (
            'receipt["lighting_sources"] = std::move(sources);',
            'receipt["lighting_uniforms"] = CaptureUniforms('
            'm_lighting_pass ? m_lighting_pass->shader().get() : nullptr);',
            'receipt["fog_uniforms"] = CaptureUniforms('
            'm_aerial_pass ? m_aerial_pass->shader().get() : nullptr);',
        )
        for record in records:
            with self.subTest(record=record):
                self.assertIn(record, capture)
                self.assertLess(capture.index(record), capture.index('receipt["complete"] = true'))
        uniforms = block(self.source, "Json CaptureUniforms(")
        for readback in ("glGetActiveUniform(", "glGetUniformfv(", "glGetUniformiv("):
            self.assertIn(readback, uniforms)
        refusal = block(self.capture, 'if (receipt["lighting_uniforms"].contains("status")')
        self.assertIn('receipt["fog_uniforms"].contains("status")', capture)
        self.assertEqual(compact(refusal), "return false;")

    def test_allocated_frame_owners_supply_nonzero_recorded_source_handles(self):
        # Lighting builds only its own inputs. Capture must fill these from the
        # same owners used by the production scene and aerial consumers.
        owners = {
            "lit_scene_color": ("m_lighting_pass ? m_lighting_pass->lighting_fbo().color_texture : 0", 11),
            "opaque_scene": ("m_lighting_pass ? m_lighting_pass->lighting_fbo().opaque_color_texture : 0", 12),
            "sky_view_lut": ("m_sky_lut.sky_view_texture()", 13),
            "transmittance_lut": ("m_sky_lut.transmittance_texture()", 14),
        }
        assignments = dict(re.findall(r"context\.(\w+) = adopt_texture\(\s*(.*?)\);",
                                      compact(self.capture)))
        for field, (expression, handle) in owners.items():
            with self.subTest(field=field):
                self.assertEqual(assignments.get(field), expression)
                self.assertLess(self.capture.index(f"context.{field} ="),
                                self.capture.index("CaptureTexture("))
                self.assertGreater(cpp_expression(assignments[field], **{expression: handle}), 0)
        for name, field in (("lit-scene", "lit_scene_color"), ("opaque-scene", "opaque_scene"),
                            ("sky-view", "sky_view_lut"), ("transmittance", "transmittance_lut")):
            self.assertIn(f'"{name}", context.{field}.id', compact(self.capture))
        texture = block(self.source, "bool CaptureTexture(")
        self.assertIn('{"texture", texture}', texture)
        self.assertIn('{"status", "captured"}', texture)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        lighting = block(pipeline, "RenderContext RenderPipeline::make_lighting_context(")
        for field in owners:
            self.assertNotIn(f"ctx.{field} =", lighting)
        self.assertIn("m_frame_lighting_ctx = make_lighting_context(camera);", pipeline)

    def has_storage(self, texture, allocated=True, width=8, height=8, depth=1, error=0):
        helper = block(self.source, "bool HasTextureStorage(")
        guard = re.search(r"if \((.*?)\) \{", compact(helper)).group(1)
        self.assertIn("return false;", block(helper, "if ("))
        if cpp_expression(guard, texture=texture, **{
                "glIsTexture(texture)": int(allocated), "GL_TRUE": 1}):
            return False
        for dimension in ("WIDTH", "HEIGHT", "DEPTH"):
            self.assertIn(f"glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_{dimension},", helper)
        result = re.search(r"return (.*?);", helper[helper.index("GLint width"):], re.S).group(1)
        return cpp_expression(result, width=width, height=height, depth=depth,
                              **{"glGetError()": error, "GL_NO_ERROR": 0})

    def test_replay_refuses_each_missing_or_unallocated_input_before_draw_and_completion(self):
        inputs = re.search(r"const std::array<GLuint, 3> inputs\{(.*?)\};",
                           self.replay, re.S).group(1)
        self.assertEqual(compact(inputs), "context.gbuffer_depth.id, context.sky_view_lut.id, "
                         "context.transmittance_lut.id")
        loop = block(self.replay, "for (const auto input : inputs)")
        self.assertEqual(compact(loop), "if (!HasTextureStorage(input)) { return false; }")
        self.assertLess(self.replay.index("for (const auto input : inputs)"),
                        self.replay.index("FogReadback restore;"))
        self.assertLess(self.replay.index("FogReadback restore;"),
                        self.replay.index("AttributedGl::Submit("))
        self.assertIn("if (!m_sky_lut.ready() || !CaptureFog(", compact(self.capture))
        refusal = block(self.capture, "if (!m_sky_lut.ready()")
        self.assertEqual(compact(refusal), "return false;")
        self.assertLess(self.capture.index("if (!m_sky_lut.ready()"),
                        self.capture.index('receipt["complete"] = true'))
        available = re.search(r"const bool aerial_available =\s*(.*?);", self.capture, re.S).group(1)
        self.assertNotIn("m_sky_lut.ready()", available)
        # Each required slot: zero name, generated/deleted name, or empty storage.
        for slot in range(3):
            for invalid in ({"texture": 0}, {"texture": 9, "allocated": False},
                            {"texture": 9, "width": 0}, {"texture": 9, "height": 0},
                            {"texture": 9, "depth": 0}, {"texture": 9, "error": 1282}):
                with self.subTest(slot=slot, invalid=invalid):
                    handles = [self.has_storage(11), self.has_storage(13), self.has_storage(14)]
                    handles[slot] = self.has_storage(**invalid)
                    self.assertFalse(all(handles))
        self.assertTrue(all(self.has_storage(handle) for handle in (11, 13, 14)))

    def test_volume_is_required_only_when_uploaded_shader_mode_samples_it(self):
        self.assertIn('glGetUniformLocation(shader.Id(), "u_volumetricMode")', self.replay)
        self.assertRegex(compact(self.replay), r"glGetUniformiv\(\s*shader.Id\(\), .*?&volumetric_mode\);")
        guard = re.search(r"if \((glGetError\(\).*?)\) \{", compact(self.replay)).group(1)
        self.assertEqual(compact(block(self.replay, "if (glGetError()")), "return false;")
        self.assertLess(self.replay.index(guard), self.replay.index("FogReadback restore;"))
        for mode in (0, 1, -1):
            for stored in (False, True):
                for error in (0, 1282):
                    with self.subTest(mode=mode, stored=stored, error=error):
                        self.assertEqual(cpp_expression(guard, volumetric_mode=mode, **{
                            "glGetError()": error, "GL_NO_ERROR": 0,
                            "HasTextureStorage(froxel)": stored}),
                            error != 0 or (mode != 0 and not stored))

    def test_missing_or_unallocated_quad_refuses_before_replay(self):
        guard = re.search(r"if \((context.screen_quad_vao.*?)\) \{", compact(self.replay)).group(1)
        self.assertEqual(compact(block(self.replay, "if (context.screen_quad_vao")), "return false;")
        self.assertLess(self.replay.index(guard), self.replay.index("FogReadback restore;"))
        for handle, allocated in ((0, False), (0, True), (7, False), (7, True)):
            with self.subTest(handle=handle, allocated=allocated):
                self.assertEqual(cpp_expression(guard, **{
                    "context.screen_quad_vao": handle,
                    "glIsVertexArray(context.screen_quad_vao)": int(allocated), "GL_TRUE": 1}),
                    handle == 0 or not allocated)
        available = re.search(r"const bool aerial_available =\s*(.*?);", self.capture, re.S).group(1)
        self.assertNotIn("screen_quad_vao", available)

    def test_optional_absence_stays_unavailable(self):
        texture = block(self.source, "bool CaptureTexture(")
        absent = compact(block(texture, "if (texture == 0)"))
        self.assertEqual(absent, 'sources[name] = {{"status", "unavailable"}}; return true;')
        self.assertLess(texture.index("if (texture == 0)"), texture.index("glGetTextureLevelParameteriv("))
        unavailable = block(self.capture, "if (aerial_available)")
        self.assertIn("CaptureFog(", unavailable)
        self.assertIn('sources["aerial"] = {{"status", "unavailable"}};', self.capture)
        available = re.search(r"const bool aerial_available =\s*(.*?);", self.capture, re.S).group(1)
        for mask in range(16):
            flags = [bool(mask & (1 << bit)) for bit in range(4)]
            with self.subTest(flags=flags):
                self.assertEqual(cpp_expression(available, **{
                    "m_aerial_pass": flags[0], "m_aerial_pass->shader()": flags[1],
                    "m_aerial_pass->shader()->IsValid()": flags[2],
                    "m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Aerial)": flags[3]}),
                    all(flags))

    def test_volume_readbacks_follow_texture_update_barrier_after_image_store(self):
        froxel = read(CLIENT / "rendering/passes/FroxelPass.cpp")
        integrate = block(froxel, "void FroxelPass::execute_integrate(")
        self.assertIn("GL_WRITE_ONLY, GL_RGBA16F", integrate)
        self.assertIn("glDispatchCompute", integrate)
        self.assertIn("glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT)", integrate)
        barrier = self.capture.index("glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT)")
        for name in ("fog-inscatter", "fog-transmittance"):
            self.assertLess(barrier, self.capture.index(f'"{name}"'))
        self.assertLess(barrier, self.capture.index("CaptureFog("))
        # The capture hook is after the finished render, before presentation.
        hook = block(read(CLIENT / "core/ScenarioRunnerCapture.cpp"),
                     "void ScenarioRunnerImpl::onPreRenderWorldSweep()")
        self.assertLess(hook.index("renderPipeline.render_frame("), hook.index("capture_terrain_coverage("))


class DiagnosticFixtureLifetimeWiring(unittest.TestCase):
    """Keep test-body borrows out of the longer-lived GL fixture input."""

    def assert_local_borrow(self, suite, name, owners, bindings):
        source = read(ROOT / "test/rendering/diagnostic_attribution_test.cpp")
        body = block(source, f"TEST_F({suite}, {name})")
        local_input = "auto input = input_;"
        self.assertIn(local_input, body)
        # The fixture input may only be read to initialize the local copy.
        self.assertEqual(len(re.findall(r"\binput_\b", body)), 1)
        for owner in owners:
            self.assertLess(body.index(owner), body.index(local_input))
        for binding in bindings:
            self.assertLess(body.index(local_input), body.index(binding))
            self.assertLess(body.index(binding), body.index("pass_.execute("))
        self.assertIn("pass_.execute(ctx_, entities_, input)", body)

    def test_authored_view_borrows_end_before_the_local_view(self):
        for name in ("AuthoredIdentityBoundsPreserveDrawsAndRefuseOnlyCapture",
                     "AuthoredUpgradeRestoresLiveCallerFramebuffers"):
            with self.subTest(name=name):
                self.assert_local_borrow(
                    "DiagnosticAttributionGl", name,
                    ("const auto view = RenderView::Validate(description);",),
                    ("input.authored_view = &view;",))

    def test_resident_terrain_borrow_ends_before_local_support(self):
        self.assert_local_borrow(
            "UnavailableImpostorGl", "UnavailableProducerProgramFailsAnAttachedCapture",
            ("SubmittedLiveTerrain support;",), ("input.resident_live_terrain = &support;",))

    def test_ladder_callbacks_end_before_local_observations(self):
        self.assert_local_borrow(
            "DiagnosticAttributionGl", "LadderFarDrawsDoNotInheritTheLastLiveTerrainIdentity",
            ("const auto current_identity =", "DiagnosticId live{};",
             "std::vector<DiagnosticId> ladder_ids;"),
            ("input.submit_terrain_chunks = [&]", "input.submit_ladder = [&]"))


class DiagnosticCaptureRefusalWiring(unittest.TestCase):
    """Guard capture-only refusals without executing native code or GL."""

    def test_authored_hex_key_budget_includes_the_entire_prefix(self):
        render = block(read(CLIENT / "rendering/passes/StaticDrawPass.cpp"),
                       "StaticDrawStats StaticDrawPass::Render(")
        prefix = re.search(
            r'(?:constexpr std::string_view mesh_prefix|std::string mesh_key) = "([^"]+)";',
            render).group(1)
        condition = re.search(r"if \((identity.empty\(\).*?)\) \{", render).group(1)
        for length in (0, 1, 506, 507, 508, 509, 1023, 1024, 1025, 507, 508, 1):
            with self.subTest(length=length):
                refused = bool(cpp_expression(condition, **{
                    "identity.empty()": length == 0, "identity.size()": length,
                    "mesh_prefix.size()": len(prefix)}))
                encoded = prefix + (b"x" * length).hex()
                self.assertEqual(refused, length == 0 or len(encoded) > 1024)
                if not refused:
                    self.assertLessEqual(len(encoded), 1024)

    def test_authored_identity_bounds_refuse_coverage_without_throwing(self):
        source = read(CLIENT / "rendering/passes/StaticDrawPass.cpp")
        render = block(source, "StaticDrawStats StaticDrawPass::Render(")
        capture = block(render[render.index("glBindVertexArray(mesh->vao)"):], "if (capture)")
        condition = re.search(r"if \((identity.empty\(\).*?)\) \{", capture).group(1)
        refused = block(capture, "if (identity.empty()")
        self.assertIn('capture->unsupported("authored_static_surface")', refused)
        self.assertIn("capture->clear_current_identity()", refused)
        self.assertNotRegex(refused, r"\b(?:throw|return|continue)\b")
        supported = block(capture, "else")
        self.assertIn('constexpr std::string_view mesh_prefix = "authored/"', capture)
        self.assertIn("std::string mesh_key(mesh_prefix)", supported)
        self.assertIn("mesh_key.reserve(mesh_prefix.size() + 2 * identity.size())", supported)
        self.assertIn("mesh_key += digits[byte >> 4]", supported)
        self.assertIn("mesh_key += digits[byte & 15]", supported)
        self.assertIn('.shader_variant = "static_asset"', supported)
        self.assertIn(".provenance = DiagnosticProvenance::Mesh", supported)
        self.assertIn(".generated_mesh = mesh_key", supported)
        self.assertNotIn("reserve(", refused)
        for length in (0, 1, 507, 508, 509, 1023, 1024, 1025):
            with self.subTest(length=length):
                actual = cpp_expression(condition, **{
                    "identity.empty()": length == 0, "identity.size()": length,
                    "mesh_prefix.size()": len("authored/")})
                self.assertEqual(actual, length == 0 or length > 507)
        self.assertLess(render.index(capture), render.index("glDrawElements,"))
        targets = read(CLIENT / "rendering/GBufferTargets.cpp")
        unsupported = block(targets, "void GBufferDiagnosticCapture::unsupported(")
        self.assertIn("state_->failed = true", unsupported)
        self.assertIn(".provenance = DiagnosticProvenance::Unsupported", unsupported)
        clear = block(targets, "void GBufferDiagnosticCapture::clear_current_identity()")
        self.assertIn("const DiagnosticId id{}", clear)
        self.assertIn("glProgramUniform4uiv(entry.shader->Id(), entry.identity, 1, id.data())", clear)

    def test_ladder_submission_refusals_invalidate_after_the_whole_submission(self):
        source = read(CLIENT / "rendering/passes/GBufferPass.cpp")
        chunks = block(source, "void GBufferPass::geometry_pass_chunks(")
        guard = re.search(r"if \(([^\n]*far_stats.refused[^\n]*)\) \{", chunks)
        self.assertIsNotNone(guard, "The complete Ladder submission must check refusals")
        refusal = block(chunks, guard.group())
        self.assertIn("capture->abort()", refusal)
        self.assertLess(chunks.index("diagnostic_draw.prepare(command)"), guard.start())
        self.assertLess(chunks.index("        });"), guard.start())
        self.assertLess(guard.start(), chunks.index("stats.far_region_draws +="))
        for attached in (False, True):
            for refused in (0, 1, 2, 65536):
                with self.subTest(attached=attached, refused=refused):
                    self.assertEqual(bool(cpp_expression(guard.group(1), **{
                        "capture": attached, "far_stats.refused": refused})),
                        attached and refused != 0)
        targets = read(CLIENT / "rendering/GBufferTargets.cpp")
        self.assertIn("state_->failed = true", block(targets, "void GBufferDiagnosticCapture::abort()"))
        self.assertIn("!state_->failed", block(targets, "bool GBufferDiagnosticCapture::complete()"))


class DiagnosticContributorWiring(unittest.TestCase):
    """Evaluate bounded admission and guard the shared deferred/contributor path."""

    def setUp(self):
        self.source = read(CLIENT / "rendering/GBufferTargets.cpp")
        self.attach = block(self.source, "bool GBufferDiagnosticCapture::attach_target(")

    def test_selected_storage_is_ensured_and_leased_before_attachment(self):
        self.assertIn("auto& targets = forward ? *state_->forward_targets : *state_->targets",
                      self.attach)
        ensure = self.attach.index("targets.ensure(true, extent, 7)")
        lease = self.attach.index("state_->attachment_lease = targets.acquire_attachment_lease()")
        self.assertLess(ensure, lease)
        self.assertLess(lease, self.attach.index("glFramebufferTexture2D("))
        forward = block(self.source, "bool GBufferDiagnosticCapture::attach_forward(")
        self.assertLess(forward.index("if (!state_->forward_targets)"),
                        forward.index("attach_target(target, extent, true)"))
        constructor = block(self.source,
                            "GBufferDiagnosticCapture::GBufferDiagnosticCapture(DiagnosticTargets&")
        self.assertLess(constructor.index("forward_targets == &targets"),
                        constructor.index("SetCurrent(this)"))
        self.assertIn("throw std::invalid_argument", block(constructor,
                      "if (forward_targets == &targets)"))
        forward_buffers = block(self.attach, "if (forward || already_captured)")
        self.assertIn("auto forward_buffers = state_->buffers", forward_buffers)
        self.assertIn("forward_buffers[6] = GL_COLOR_ATTACHMENT6", forward_buffers)
        self.assertIn("glDrawBuffers(state_->buffer_count, forward_buffers.data())", forward_buffers)
        self.assertEqual(self.attach.count("state_->attachment_lease.texture()"), 2)

    def test_attachment_order_allows_continuations_and_refuses_nested_or_early_forward_calls(self):
        condition = re.search(r"if \((.*?)\) \{", self.attach, re.S).group(1)
        match = re.fullmatch(r"(.*?)\(forward \? (.*?) : (.*?)\)", condition, re.S)
        self.assertIsNotNone(match)
        for attached in (False, True):
            for captured in (False, True):
                for forward_captured in (False, True):
                    for forward in (False, True):
                        selected = match.group(2 if forward else 3)
                        actual = cpp_expression(match.group(1) + "(" + selected + ")", **{
                            "state_->framebuffer": int(attached), "state_->captured": captured,
                            "state_->forward_captured": forward_captured})
                        expected = attached or (not captured if forward else forward_captured)
                        self.assertEqual(actual, expected)
        self.assertIn("state_->forward_captured = true", self.attach)

    def test_forward_and_deferred_writers_gate_submission_and_clear_rejected_ids(self):
        writer = block(self.source, "void GBufferDiagnosticCapture::write(")
        self.assertIn("submission.pass == (state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer)",
                      compact(writer))
        mismatch = block(writer, "if (!matching_pass)")
        self.assertIn('unsupported("diagnostic_contributor_pass_mismatch",', mismatch)
        self.assertNotIn("return;", mismatch)
        self.assertIn("matching_pass && !state_->refused && !state_->table.submit(submission, id)",
                      writer)
        self.assertLess(writer.index("DiagnosticId id{}"), writer.index("glProgramUniform4uiv("))
        self.assertIn("id.data()", writer)
        current = block(self.source, "void GBufferDiagnosticCapture::write_current(")
        self.assertIn("return;", block(current, "if (!state_->framebuffer)"))
        self.assertIn("glGetIntegerv(GL_CURRENT_PROGRAM, &program)", current)
        self.assertIn("entry.shader->Id() == static_cast<GLuint>(program)", current)
        self.assertIn("write(*entry.shader, submission)", current)
        self.assertIn('"diagnostic_writer_without_variant"', current)

    def test_feedback_bounds_precede_comparison_and_compilation(self):
        variant = block(self.source, "Shader* GBufferDiagnosticCapture::variant(")
        count = re.search(r"if \((feedback_varyings.size\(\) > \d+) \|\|", variant).group(1)
        name = re.search(r"return (name.size\(\) > \d+);", variant).group(1)
        for size in (0, 1, 3, 4, 65536):
            self.assertEqual(cpp_expression(count, **{"feedback_varyings.size()": size}), size > 3)
        for size in (0, 1, 63, 64, 65, 1024):
            self.assertEqual(cpp_expression(name, **{"name.size()": size}), size > 64)
        cache = variant.index("for (const auto& entry")
        self.assertLess(variant.index(count), cache)
        self.assertLess(cache, variant.index("if (state_->variants.size() == 6)"))
        self.assertLess(variant.index(count), variant.index("std::make_unique<Shader>"))
        self.assertIn("entry.feedback_varyings == feedback_varyings", variant)
        self.assertIn("shader->FragmentPath().c_str(), feedback_varyings, defines", variant)

    def test_variant_and_detachment_failures_record_the_active_pass(self):
        variant = block(self.source, "Shader* GBufferDiagnosticCapture::variant(")
        for marker in ("if (!shader || !shader->IsValid())",
                       "if (state_->variants.size() == 6)",
                       "if (!diagnostic->IsValid() || (!zero_ids && identity < 0))"):
            self.assertIn("state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer",
                          block(variant, marker))
        detach = block(self.source, "void GBufferDiagnosticCapture::detach()")
        self.assertIn("state_->forward ? DiagnosticPass::Forward : DiagnosticPass::GBuffer",
                      block(detach, "if (!state_->failure.empty())"))

    def test_generated_key_bounds_and_namespaces_precede_registry_mutation(self):
        table = read(CLIENT / "rendering/DiagnosticDrawTable.h")
        submit = block(table, "bool submit(")
        length = re.search(r"submission.generated_mesh.size\(\) > \d+", submit).group()
        for size in (0, 1, 1023, 1024, 1025, 65536):
            self.assertEqual(cpp_expression(length, **{"submission.generated_mesh.size()": size}),
                             size > 1024)
        for text in ("!submission.mesh_path.empty() || !valid_path(submission.generated_mesh)",
                     "submission.mesh_path.empty() && submission.generated_mesh.empty()",
                     'submission.generated_mesh.empty() ? ":asset:" : ":generated:"',
                     'mesh["generated"] = submission.generated_mesh'):
            self.assertIn(text, submit)
        for check in (length, "mesh_json = mesh.dump()", "added > max_json_bytes - json_bytes_"):
            self.assertLess(submit.index(check), submit.index("mesh_ids_.emplace("))


class DiagnosticScenarioCaptureWiring(unittest.TestCase):
    def test_first_refusal_stops_sibling_writers_and_preserves_the_reason(self):
        capture = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        refuse = block(capture, "bool ScenarioRunnerImpl::refuseScenarioCapture(")
        self.assertIn("if (!scenario_failed)", refuse)
        latch = block(refuse, "if (!scenario_failed)")
        self.assertIn('scenario_failure_reason = "capture_diagnostics_" + std::string(reason);', latch)
        self.assertEqual(refuse.count("scenario_failure_reason ="), 1)
        ready = block(capture, "bool ScenarioRunnerImpl::scenarioCaptureReady(")
        self.assertIn("if (scenario_failed) { return false; }", compact(ready))
        self.assertLess(ready.index("if (scenario_failed)"),
                        ready.index("renderPipeline.diagnostic_capture_ready()"))

    def test_mutating_readiness_precedes_frame_and_recorder_updates(self):
        capture = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        ready = block(capture, "bool ScenarioRunnerImpl::scenarioCaptureReady(")
        self.assertIn("if (!renderPipeline.diagnostic_capture_ready())", ready)
        refusal = block(ready, "if (!renderPipeline.diagnostic_capture_ready())")
        self.assertIn("return refuseScenarioCapture(renderPipeline.diagnostic_capture_refusal());", refusal)
        post = block(capture, "void ScenarioRunnerImpl::onPostRenderCapture(")
        preflight = block(post, "if (renderPipeline.capture_diagnostics_enabled())")
        self.assertIn("if (!scenarioCaptureReady(width, height)) { return; }", compact(preflight))
        for update in ("++scenario_frame_count", "lod_ground_frame_recorder.record_frame(",
                       "lod_boundary_transition_recorder.record_frame(", "runtime_state_recorder.capture("):
            self.assertLess(post.index(preflight), post.index(update))

    def test_backbuffer_completion_uses_the_checked_pixel_writer(self):
        harness = read(CLIENT / "core/RuntimeScenarioHarness.cpp")
        writer = block(harness, "bool WriteBackbufferPpm(")
        self.assertIn("return WritePixelBufferPpm(path, width, height, pixels);", writer)
        self.assertNotIn("std::ofstream", writer)
        self.assertLess(writer.index("CapturePixelBytes(width, height, 3, bytes)"),
                        writer.index("std::vector<unsigned char> pixels(bytes)"))
        pixels = read(CLIENT / "rendering/PixelIo.cpp")
        checked = block(pixels, "bool WritePixelBufferPpm(")
        self.assertIn("const bool written = WritePixelBufferPpmStream(output, width, height, pixels);", checked)
        self.assertIn("const bool closed = CloseCaptureFile(output);", checked)
        self.assertIn("if (!written || !closed) {", checked)

    def test_scan_and_benchmark_conflict_without_a_scenario_before_paths(self):
        config = block(read(CLIENT / "core/RuntimeScenarioConfig.cpp"),
                       "RuntimePathOptions ParseRuntimePathOptions(")
        admission = block(config, "if (diagnostics.enabled)")
        self.assertIn('const bool frame_scan = HasCommandLineOption(argc, argv, "--frame-scan");', admission)
        self.assertIn('const bool render_benchmark = HasCommandLineOption(argc, argv, "--render-benchmark");', admission)
        self.assertIn('if ((!scenario.empty() && (frame_scan || render_benchmark)) || '
                      '(frame_scan && render_benchmark)) { throw std::invalid_argument('
                      '"capture diagnostics refuse multiple capture drivers"); }', compact(admission))
        self.assertLess(config.index(admission), config.index("std::filesystem::create_directories("))

    def test_every_scenario_writer_uses_the_completed_frame_adapter(self):
        capture = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        families = (("captureWindowModeStress", 1), ("captureLodSeamArrival", 1),
                    ("captureLodGround", 1), ("captureWaterVisual", 2),
                    ("captureMaterialVisual", 2), ("captureSkyboxVisual", 1),
                    ("captureWeatherVisual", 2), ("captureLightningStrike", 2),
                    ("captureCloudShadow", 2), ("captureParticleDeterminism", 1),
                    ("captureAtmosMotion", 1), ("captureFoliageVisual", 1),
                    ("capturePrecipitation", 2), ("captureTimeOfDaySweep", 2),
                    ("capturePlayerView", 1), ("captureFarLodHorizon", 1),
                    ("captureSkinnedMeshVisual", 1), ("captureShowcaseVideo", 1),
                    ("captureCreatureSlice", 1))
        for family, count in families:
            with self.subTest(family=family):
                body = block(capture, "void ScenarioRunnerImpl::" + family + "(")
                self.assertNotIn("WritePixelBufferPpm(", body)
                self.assertNotIn("WriteBackbufferPpm(", body)
                self.assertEqual(len(re.findall(r"writeScenario(?:Pixels|Backbuffer)\(", body)), count)
        post = block(capture, "void ScenarioRunnerImpl::onPostRenderCapture(")
        self.assertIn("RenderPipeline::CaptureCleanup diagnostic_cleanup(renderPipeline);", post)
        self.assertLess(post.index("scenarioCaptureReady(width, height)"),
                        post.index("++scenario_frame_count"))
        self.assertLess(post.index("renderPipeline.capture_diagnostics_enabled() && scenario_failed"),
                        post.index("runtime_state_recorder.capture("))

    def test_scenario_identity_is_observed_from_the_completed_frame(self):
        capture = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        observe = block(capture, "ScenarioRunnerImpl::scenarioCaptureManifest() const")
        for observation in ("renderPipeline.diagnostic_capture_manifest()",
                            "input.simulation_tick = gameSession->GetSimulationTickCount()",
                            "input.world_seed = gameSession->GetMetadata().seed",
                            "gameSession->PresetContentHash()",
                            'input.render_settings["scenario"] = scenario_config.scenario'):
            self.assertIn(observation, observe)
        source = read(CLIENT / "rendering/RenderPipeline.cpp")
        publish = block(source, "bool RenderPipeline::capture_diagnostics(")
        self.assertNotIn("std::move(m_diagnostic_capture)", publish)
        self.assertIn("m_diagnostic_capture->finish(", publish)
        readback = read(CLIENT / "rendering/DiagnosticReadback.h")
        self.assertIn("return scope_ ? scope_->complete() : identity_complete_;", readback)

    def test_refusal_and_disabled_bypass_precede_output(self):
        pipeline = read(CLIENT / "rendering/RenderPipeline.h")
        publish = block(pipeline, "bool with_diagnostic_capture(")
        self.assertLess(publish.index("diagnostic_capture_refusal().empty() &&"),
                        publish.index("WithCaptureManifest(path, observe(), write_capture)"))
        self.assertIn("if (!m_capture_diagnostics.enabled) { return write_capture(); }", compact(publish))
        runner = read(CLIENT / "core/ScenarioRunnerImpl.h")
        write = block(runner, "bool writeScenarioCapture(")
        self.assertLess(write.index("scenarioCaptureReady(width, height)"),
                        write.index("renderPipeline.with_diagnostic_capture("))
        capture = read(CLIENT / "core/ScenarioRunnerCapture.cpp")
        ready = block(capture, "bool ScenarioRunnerImpl::scenarioCaptureReady(")
        self.assertIn("capture_extent_mismatch", ready)
        self.assertIn("width <= 0 || height <= 0", ready)
        self.assertLess(ready.index("diagnostic_capture_refusal()"),
                        ready.index("return true;", ready.index("capture_extent_mismatch")))
        pixels = block(capture, "bool ScenarioRunnerImpl::writeScenarioPixels(")
        self.assertLess(pixels.index("CapturePixelBytes(width, height, 3, bytes)"),
                        pixels.index("return writeScenarioCapture("))

    def test_admission_covers_all_supported_and_unsupported_producers_before_paths(self):
        config = block(read(CLIENT / "core/RuntimeScenarioConfig.cpp"),
                       "RuntimePathOptions ParseRuntimePathOptions(")
        admission = block(config, "if (diagnostics.enabled)")
        supported = set(re.findall(r'scenario == "([^"]+)"', admission))
        self.assertEqual(supported, {"world_visual_sweep",
                                     "lod_seam_arrival_smoke", "lod_ground_smoke", "skybox_visual_smoke",
                                     "cloud_shadow_smoke", "timeofday_sweep_smoke", "player_view_smoke",
                                     "skinned_mesh_visual_smoke", "creature_slice_smoke"})
        unsupported = admission[admission.index("for (const auto* unsupported :"):]
        unsupported = unsupported[:unsupported.index("})")]
        self.assertEqual(set(re.findall(r'"([^"]+)"', unsupported)),
                         {"window_mode_stress_smoke", "material_visual_smoke", "farlod_horizon_smoke", "water_visual_smoke",
                          "weather_visual_smoke", "particle_emitter_determinism_smoke",
                          "foliage_visual_smoke", "precipitation_smoke"})
        for producer in ("--render-parity-ssao", "--bake-tree-impostor", "--survey",
                         "--ui-screenshot", "--ui-thumbs", "--scene-config",
                         "--timelapse", "--timelapse-frames"):
            self.assertIn('"' + producer + '"', admission)
        self.assertIn("HasCommandLineOption(argc, argv, producer)", admission)
        self.assertIn("capture diagnostics refuse multiple capture drivers", admission)
        self.assertLess(config.index("if (diagnostics.enabled)"),
                        config.index("options.diagnostics = diagnostics;"))
        self.assertLess(config.index("options.diagnostics = diagnostics;"),
                        config.index("for (const char* flag :"))


class PassCallerStateContract(unittest.TestCase):
    """Shipping preservation uses known values; external callers query explicitly."""

    def test_shipping_shadow_has_zero_state_queries(self):
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        shadow = read(CLIENT / "rendering/passes/ShadowPass.cpp")
        depth = read(CLIENT / "rendering/passes/PassDepthState.h")
        # Check the execution body first: the original four queries fail here.
        execute = block(shadow, "ShadowPass::execute(")
        self.assertEqual(re.findall(r"\bgl(?:Get\w*|IsEnabled\w*)\s*\(", execute), [])
        stage = block(pipeline, "void RenderPipeline::execute_stage_shadow(")
        self.assertRegex(stage, r"m_shadow_pass->execute\(shadow_ctx,\s*shadow_input,\s*caller\)")
        paths = (stage, execute,
                 block(shadow, "void ShadowPass::CallerState::Restore()"),
                 block(depth, "void Restore()"),
                 block(pipeline, "SubmitTerrainChunksFn RenderPipeline::make_terrain_submitter("),
                 block(pipeline, "void RenderPipeline::draw_chunks_mdi("))
        for path in paths:
            self.assertNotRegex(path, r"\bgl(?:Get(?:Integer\w*|Boolean\w*|Float\w*|Double\w*)|IsEnabled\w*)\s*\(")
            self.assertNotIn("QueryExternal(", path)
            self.assertNotIn("execute_external(", path)
        self.assertIn("caller.Restore()", execute)
        self.assertLess(execute.index("ReplayRollback restore_raster"),
                        execute.index("glClipControl("))
        self.assertNotIn("glPolygonOffset", execute)

    def test_shadow_external_boundary_and_scene_values_are_explicit(self):
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        shadow = read(CLIENT / "rendering/passes/ShadowPass.cpp")
        external = block(shadow, "ShadowPass::execute_external(")
        self.assertIn("return execute(ctx, input, CallerState::QueryExternal())", external)
        self.assertNotIn("execute_external(", pipeline)
        startup = block(pipeline, "bool RenderPipeline::startup(")
        dispatch = block(pipeline, "void RenderPipeline::dispatch_stages(")
        stage = compact(block(pipeline, "void RenderPipeline::execute_stage_shadow("))
        for call in ("glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)",
                     "glClearDepth(0.0)", "glDepthFunc(GL_GREATER)"):
            self.assertIn(call, startup)
        for call in ("glEnable(GL_DEPTH_TEST)", "glDepthMask(GL_TRUE)",
                     "glEnable(GL_CULL_FACE)", "glCullFace(GL_BACK)"):
            self.assertIn(call, dispatch)
        for field in (".function = GL_GREATER", ".write_mask = GL_TRUE",
                      ".test_enabled = GL_TRUE", ".clip_origin = GL_LOWER_LEFT",
                      ".clip_depth = GL_ZERO_TO_ONE", ".clear_depth = 0.0",
                      ".cull_face = GL_BACK", ".cull_enabled = GL_TRUE"):
            self.assertIn(field, stage)

    def test_static_draw_queries_only_unknown_bindings_on_gbuffer_path(self):
        source = read(CLIENT / "rendering/passes/StaticDrawPass.cpp")
        state = block(source, "struct GlState")
        initialization = block(source, "explicit Impl(")
        self.assertIn("GlState state(nullptr)", initialization)
        known = block(state, "if (known_caller)")
        self.assertIn("caller = *known_caller", known)
        self.assertNotRegex(known, r"gl(?:Get\w*|IsEnabled)\s*\(")
        external = block(state, "else")
        self.assertIn("PassDepthState::QueryExternal()", external)
        self.assertIn("glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING", external)
        # No second depth/target query outside that explicit unknown boundary.
        remainder = state.replace(external, "")
        self.assertNotRegex(remainder, r"glGet\w*\(GL_(?:DEPTH_|DRAW_FRAMEBUFFER)")
        self.assertNotIn("glIsEnabled(GL_DEPTH_TEST)", remainder)
        self.assertIn("caller.depth.Restore()", remainder)
        gbuffer = block(read(CLIENT / "rendering/passes/GBufferPass.cpp"),
                        "GBufferDrawStats GBufferPass::execute(")
        self.assertIn(".draw_framebuffer = m_gbuffer.fbo_id", gbuffer)
        self.assertIn("m_gbuffer, false, &caller)", compact(gbuffer))
        for call in ("glEnable(GL_DEPTH_TEST)", "glDepthFunc(GL_GREATER)",
                     "glDepthMask(GL_TRUE)", "glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id)"):
            self.assertLess(gbuffer.index(call), gbuffer.index("StaticDrawPass::CallerState caller"))


if __name__ == "__main__":
    unittest.main()
