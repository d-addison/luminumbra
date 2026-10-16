# Audio playback defaults

Ordinary client launches enable the miniaudio backend. `--no-audio` selects the
null backend and wins over `--enable-audio`, which remains accepted for compatibility.
A device initialization failure leaves playback unavailable. Settings report the
backend's actual availability and disable playback controls when unavailable.
Saved master, music and SFX levels remain authoritative; changing the playback
default does not overwrite them.

GPU and headless captures continue to pass `--no-audio` explicitly. Null playback
retains no request history or per-frame telemetry writes unless
`--audio-telemetry-path <file>` is requested. Historical null telemetry fixtures
retain their release-default activation label for compatibility.

Parser, mixer, volume, environmental-model, null-backend and injected device-failure
tests cover activation and suppression. Audible quality, output routing and complete
event coverage still require device qualification.
