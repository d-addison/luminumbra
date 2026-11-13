# Audio playback defaults

Ordinary client launches enable the miniaudio backend. `--no-audio` selects the
null backend and wins over `--enable-audio`, which remains accepted for compatibility.
A device initialization failure leaves playback unavailable. Settings report the
backend's actual availability and disable playback controls when unavailable.
Saved master, music and SFX levels remain authoritative; changing the playback
default does not overwrite them.

Qualification, preview and capture launch commands must pass `--no-audio`
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
The historical `audio_off` receipt section in `tools/gates/run-v03-acceptance.ps1`
still requests `default_null_playback_with_saved_volume` (lines 381–415); it needs
alignment with this amended contract before it can qualify ordinary startup.
Its existing receipt is not evidence that the amended native acceptance passed.

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
