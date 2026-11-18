# Audio playback defaults

Ordinary client launches enable the miniaudio backend. `--no-audio` selects the
null backend and wins over `--enable-audio`, which remains accepted for compatibility.
A device initialization failure leaves playback unavailable. Settings report the
backend's actual availability and disable playback controls when unavailable.
Saved master, music and SFX levels remain authoritative; changing the playback
default does not overwrite them.

Render qualification, preview and capture launch commands must pass `--no-audio`
explicitly, including GPU and headless captures. Null playback
retains no request history or per-frame telemetry writes unless
`--audio-telemetry-path <file>` is requested. Historical null telemetry fixtures
retain their release-default activation label for compatibility.

Parser, mixer, volume, environmental-model, null-backend and injected device-failure
tests cover activation and suppression. Audible quality, output routing and complete
event coverage still require device qualification.

## Amended startup acceptance

The historical default-off startup requirements in issue #68, issue #70 and
issue #71 are amended to the ordinary-startup audio-on policy above:

- Issue #68: qualify ordinary client startup with audio on and the explicit
  `--no-audio` path alongside loading, networking and performance. The explicit
  null path must suppress playback with nonzero saved volumes, win over
  `--enable-audio` in either order, report unavailable playback controls truthfully
  and retain no request history or telemetry unless explicitly requested.
- Issue #70: apply that amended startup and explicit-null acceptance to the final
  release candidate. Source publication does not establish native startup,
  settings or package qualification.
- Issue #71: ordinary packaged client startup has audio on; the newly named
  preview launcher must pass `--no-audio` explicitly. Preserve saved preferences
  and the previously delivered preview. Verify the new package identity,
  saved-world acceptance and clean exits on that exact package.

Native startup/settings/package qualification and preview delivery remain pending.
Audible quality and complete event coverage remain separate device qualification.
The historical `audio_off` section name remains compatible in
`tools/gates/run-v03-acceptance.ps1`, but its old default-off-only checklist cannot
qualify acceptance. The driver collects three sessions: ordinary startup without
audio flags, `--enable-audio --no-audio`, and `--no-audio --enable-audio`. In each
session, open Settings, leave all three audio controls visible without changing
levels, then quit within `-TimeoutSeconds`. Use nonzero saved master, music and
SFX levels. `-AudioChecklist` remains accepted as supplementary evidence only.

The receipt embeds `runtime_snapshots` in that section, keyed by `ordinary`,
`no_audio` and `no_audio_reversed`. Each entry contains `exit_code`, `timed_out`
and the runtime snapshot as `state`; the same entries are retained in
`audio-acceptance.json`, alongside raw runtime artifacts and logs. The checker
requires successful exits without timeouts and each state's
`luminumbra.audio_acceptance.v1` object. Its `resolution`, `backend`,
`saved_volumes`, `controls` and `telemetry` observations must establish:

- ordinary default resolution to `device`, enabled playback, an initialized engine,
  a started device and SFX, ambient, event and UI routing;
- explicit resolution to `null` in both flag orders, with engine, device, playback
  and routing false and `no_audio_precedence` true;
- equal nonzero `loaded` and `in_effect` saved volumes within and across sessions,
  `unchanged` true, device `playback_gains` equal to saved levels, and suppressed
  `playback_gains` explicitly null;
- `controls.observed` true, exactly three controls, disabled counts of zero for
  playback and three for suppression, `match_playback` true, and a matching
  `controls_disabled` alias;
- boolean telemetry opt-in and enabled observations, `opt_in_only` true, and no
  enabled telemetry or retained requests without opt-in. Ordinary startup must
  not request telemetry.

Ordinary `no_audio_precedence` is explicitly null because no disable flag was
requested. Unknown, missing, wrongly typed, false qualifying observations and
unsupported schema versions refuse qualification; unobserved controls and
unavailable saved volumes never pass. Required nulls must be present as keys.
Every audio-schema object must carry exactly its v1 keys: an extra key refuses,
even when every required key is correct. The acceptance driver reports audio as
unevaluated, never pass, on non-Windows hosts, because only Windows sessions own
the client process tree.
These receipt checks do not establish audible quality or complete event coverage.

Source evidence: `src/luminumbra_client/core/RuntimeScenarioConfig.cpp:255–256`
parses the explicit disable flag and enables ordinary startup;
`src/luminumbra_client/core/RuntimeScenarioConfig.h:69–73` gives disable precedence;
`src/luminumbra_client/main_client.cpp:2266–2284` selects the backend and applies
saved volumes. Both client executables share that entry point
(`src/CMakeLists.txt:103–128`). The headless server accepts `--no-audio` as a no-op
(`src/luminumbra_server/main_server.cpp:228–229`).

Regression evidence (registered, not native execution receipts):
`AudioDisabledTest.RealLaunchParserDefaultsOnAndDisableWinsInEitherOrder`
(`test/audio/audio_disabled_test.cpp:48–58`) pins audio-on startup and both flag
orders; `AudioDisabledTest.SavedNonzeroVolumesCannotEnablePlaybackOrRetainRequestHistory`
(`test/audio/audio_disabled_test.cpp:104–132`) pins explicit-null suppression;
`AudioDisabledTest.ExplicitTelemetryPreservesSuppressionAndReportsItsRealActivation`
(`test/audio/audio_disabled_test.cpp:135–164`) covers opt-in telemetry. The target
is registered in `test/CMakeLists.txt:1922–1935,2149,2231–2232`.
`UiSmokeTest.DisabledAudioShowsStatusAndPreservesSavedVolumes`
(`test/ui/ui_smoke_test.cpp:1864–1915`) covers unavailable controls and preserved
levels; `AudioPhysicsBinding.FailedDeviceInitializationReportsUnavailableAndSuppressesPlayback`
(`test/audio/audio_physics_binding_test.cpp:107–122`) covers device failure.
