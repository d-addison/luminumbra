# Audio in the v0.3 release

Audio is disabled by default for this release. The client uses its null audio
backend, so it does not initialize an audio device, open audio banks or play
sounds. Existing saved volume levels cannot enable it. The settings screen
states that audio is disabled and disables its volume controls while preserving
saved values.

`--enable-audio` is an explicit developer diagnostic opt-in to the existing
experimental backend. It is not a supported release feature or an audibility
claim. `--no-audio` takes precedence when both flags are present. The UI labels
an initialized opt-in backend as experimental.

Null-backend telemetry is written only when `--audio-telemetry-path <file>` is
requested. Ordinary disabled playback retains no event history and performs no
per-frame telemetry file writes. Explicit telemetry identifies whether the null
backend was selected by the release default or `--no-audio`.

The existing offline bank, mixer, volume, environmental-model and null-backend
tests remain useful and remain enabled. They do not establish heard playback,
output routing or event coverage. Audio playback and sound-quality qualification
are deferred from this release to the separate audio implementation session;
they are not additional mandatory release gates.
