# Developer console

The client provides one developer console and a widget registry in both ordinary
and QA builds, including Release. F1 opens the hub by default. Ordinary Settings
also provides **Controls → Developer bindings and hotkey help**, so an unbound
console remains accessible.
Opening it leaves simulation running. Closing the hub preserves widget visibility
and active rendering effects.

## Widgets and shortcuts

The hub lists every module with a short description, resolved hotkeys and any
unavailable-service reason. Search matches IDs, titles, categories and descriptions.
Panel rows provide Toggle, Show and Hide. Each binding provides Rebind and Unbind;
effect bindings also provide Run. Action-only modules explicitly have no panel.
A window close button uses the same visibility operation as its row and commands. Interactive panels can remain open after the hub closes; passive
widgets draw independently and never take focus.

| ID | Default visibility | Default shortcut | Presentation |
|---|---|---|---|
| `perf.hud` | On | Unbound | Passive frame performance |
| `perf.gpu` | Off | F3 | GPU pass diagnostics |
| `render.shaders` | Off | F10 | Shader roster, reload and uniform editing |
| `shader.reload` | No panel | F5 | Reload all shaders |
| `render.debug_views` | Off | F6 (cycle views) | G-buffer view controls |
| `render.wireframe` | Off | F9 (toggle effect) | Wireframe effect controls |
| `world.generation` | Off | F7 | Independent generation preview |
| `client.settings` | Off | F8 | Client settings |
| `runtime.time` | Off | `[` slower, `]` faster, `\` reset | Time controls |
| `runtime.time_indicator` | On, when scale differs from 1 | Unbound | Passive time indicator |
| `player.state` | Off | Unbound | Current player diagnostics |
| `farm.debug` | Off | Unbound | Farming diagnostics |
| `world.creature_labels` | Off | Unbound | Foreground creature labels |
| `ui.data_stream` | Off | Unbound | UI stream diagnostics |
| `ui.debugger` | Off | Unbound | RmlUi debugger adapter |
| `console.help` | Off | Unbound | Generated help |

Effect bindings belong to their module rows: `shader.reload`, `render.debug-cycle`,
`render.wireframe-toggle`, `time.slower`, `time.faster` and `time.reset`. Showing or
hiding their panels does not perform these actions. Photo mode owns `[` and `]`
for aperture; developer time shortcuts are suppressed there.

These are defaults. Module rows, **Actions and resolved bindings**, generated Help,
`bindings` and the ordinary Settings shortcut list report the actual bindings, including
changes and conflicts. Help also includes gameplay bindings and loading, photo,
objectives and farming presentation controls. Availability snapshots refresh on
opening the hub and world/menu transitions; **Refresh services** updates them
explicitly without polling hidden services every frame.

The generation preview is allocated lazily when its contents first draw. Changing
preview parameters, showing or hiding it only affects the preview. **Apply &
Regenerate** remains an explicit button, enabled only with an ordinary local world.
Menu backdrops and automated scenarios are not application targets. There is no
live-world regeneration console command.

The RmlUi debugger retains its own rendering. Close the hub and other developer
panels to interact with that inspector; its adapter still participates in
visibility, persistence and Escape ordering. Loading UI, photo presentation, the
creature journal, objectives and ordinary farming UI follow their existing
lifecycle or gameplay rules rather than developer preferences.

## Commands

Commands validate arity and typed arguments before invoking an operation. Floats
must be finite; time scale is limited to 0–16. Unknown IDs, modes, conflicting
bindings and trailing arguments are rejected. Quoted arguments support names
containing spaces. There is no scripting language or arbitrary simulation setter.

| Command | Operation |
|---|---|
| `help [command]`, `widgets`, `bindings` | Generated syntax, inventory and resolved bindings |
| `widget show/hide/toggle <id>` | Change one visibility preference |
| `bind <action> <chord>`, `unbind <action>` | Change a widget or action binding |
| `shader reload [all\|name]`, `shader watch <bool>` | Queue shader work or enable the independent watcher |
| `render debug <mode>`, `render debug-cycle` | Select or cycle off/albedo/normal/depth/material/position |
| `render wireframe <bool>`, `render wireframe-toggle` | Set or toggle the rendering effect |
| `time scale <value>`, `time slower/faster/reset` | Explicit runtime time control |
| `settings save`, `settings defaults developer` | Retry saving or reset developer preferences only |
| `log clear`, `log level <level>` | Clear or filter the console display |

For example, `widget hide perf.hud`, `bind perf.gpu Ctrl+F3` and `shader reload all`
use the same operations as UI buttons and shortcuts. Shader reload executes on
the render thread with existing rollback behavior. Hiding the shader panel does
not stop an explicitly enabled watcher. Uniform edits remain runtime-only and
may be overwritten by the rendering pass that owns a uniform.

Input is limited to 4 KiB. Session history retains 128 submissions, ignores blank
lines, collapses consecutive duplicates, and restores the edited draft when
navigating back from history. Logs retain newest records with severity, timestamp
and sequence: at most 2,048 records, 512 KiB of payload and 8 KiB per record.
Oversized messages are copied into bounded storage; truncations and evictions are
counted. Producers never call ImGui. Display filtering leaves the global logger
level and existing stdout/file sinks unchanged. History and logs are session-local.

## Focus and persistence

Application-owned callbacks feed each UI backend once. Binding capture comes
first: Escape cancels and modifier-only presses keep waiting. The console toggle
remains usable during developer interaction; Escape closes the most recently
focused developer interaction before gameplay pause. Developer interaction blocks
movement, farming, terraforming, photo actions, mouse-look and gameplay scroll.
Focused RmlUi controls retain ownership when developer interaction closes, and
printable console bindings do not steal text-input characters. Resolved developer
actions and panel bindings, including bindings on non-function keys, remain usable
while a panel owns input. Focused console/ImGui text entry and RmlUi text fields
block developer actions, including function keys. Passive widgets never acquire
input ownership.

Ownership updates within the event batch, clears pending gameplay edges and
quarantines held keys until release. Cursor mode follows remaining interactions;
returning to mouse-look resets its initial mouse sample. Binding validation rejects
conflicts in overlapping scopes, window/navigation reservations and resolved
gameplay controls. On startup and developer reset, gameplay wins and conflicting
developer defaults become unbound with a reported reason.

Visibility, bindings and interactive panel layouts live in schema 1 of the existing
`user.developer` overlay. `layouts` records store `x`, `y`, `width`, `height` and
`open` by module ID; the hub uses `console.toggle`. Absent layout records use the
panel defaults. In otherwise valid JSON, malformed layout records, including numeric
overflow at any depth, are logged and ignored without discarding valid sibling settings.
A wrong-type `layouts` container (including an array or scalar with overflowing numbers)
is also logged and ignored as a whole. Both initial loading and user overlays apply
this tolerance only within `user.developer.layouts`; errors in other settings retain
their existing loading behavior.
The Controls settings section links to the hub for developer rebinding.
Absent `perf.hud` means on. The hub, commands and ordinary Settings performance
switch edit the same registry value. Changes save immediately; failed saves are
reported, retain dirty state and can be retried with `settings save` or the hub
button. Bounded unknown IDs survive for forward compatibility. Video, audio and
integer gameplay controls retain their existing formats. The client disables ImGui
disk ini persistence; completed moves/resizes save to the user overlay.
Saved layout open state and registry visibility stay synchronized.

Debug modes, wireframe, time scale, shader watch/uniform state and preview
parameters remain runtime-only. Developer preferences are excluded from the
simulation configuration hash. Explicit time scaling and live regeneration are
intentional runtime operations; simply displaying their controls performs neither.

## Capture and widget registration

`--no-ui` remains a temporary override. Automated timelapse, scene, frame-scan,
render-parity, render-benchmark, survey, impostor, scenario and UI screenshot/thumbnail
runs suppress developer drawing and shortcuts without rewriting saved preferences.
The ecology timelapse retains its explicit creature-label override. Essential
loading and gameplay presentation keep their existing capture rules.

Register definitions explicitly through `app/RegisterDeveloperWidgets.cpp` after
configuration and services exist. `BuiltinDeveloperWidgets()` supplies stable
IDs, titles, short descriptions, categories, defaults, effect bindings, persistence
keys and presentation/focus policy. `Presentation::ActionOnly` explicitly represents
a module without a panel or visibility binding.
Supply availability and contents through `DeveloperWidgetServices`; missing
services remain listed with an unavailable reason. Definitions seal at startup;
duplicate IDs are rejected and mutations requested while drawing are deferred.

Callbacks must resolve the current service owner when invoked rather than retain
a player/world pointer across transitions. Contents do not create windows or
manage visibility. `debug/widgets/DeveloperWidgetUi.cpp` wraps interactive windows,
focus and closing; passive contents use windows without inputs or foreground
labels. An external presentation can mirror visibility mutations through
`visibility_changed`, with teardown clearing it before UI/context destruction.
Destroy preview GL resources before ImGui and the window context.

Hidden widgets receive no per-frame availability or draw callbacks, formatting,
uniform enumeration, preview sampling or UI stream snapshot work. Inventory/help
availability is refreshed by events. The log sink still ingests bounded records,
and existing GPU timer instrumentation continues independently of profiler
visibility. Pure registry/service tests require no graphics dependencies. The
separate `developer_console_imgui_test` target compiles core ImGui without its GL
backends and exercises the shell and widgets with fake services. Runtime acceptance
still needs an authorized Release run: fresh settings, disable/restart, all widget
controls, capture suppression and repeated world transitions.
