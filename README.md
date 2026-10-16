# Kast

C++20 game engine for building MMORPGs. `engine/` is the reusable runtime (SDL3 window + bgfx Direct3D 11, EnTT
scene, renderer, scene files); `editor/` is the Unity-style editor (`kast_editor`) built on it with Dear ImGui docking
and ImGuizmo.

## Prerequisites (Windows)

- Visual Studio 2022 with the C++ workload (MSVC v143)
- CMake 3.25+ and Ninja on `PATH`
- vcpkg checkout with `VCPKG_ROOT` set (e.g. `C:/vcpkg`)

Dependencies (`sdl3`, `bgfx`, `imgui[docking-experimental,sdl3-binding]`, `imguizmo`, `entt`, `nlohmann-json`, `luajit`, `sol2`) install automatically from `vcpkg.json`
on first configure. The first configure builds bgfx's shader tools (glslang, SPIRV-Tools, ...) and takes several minutes.

`CMakePresets.json` sets `VCPKG_VISUAL_STUDIO_PATH` to `C:/Program Files/Microsoft Visual Studio/2022/Community`
so vcpkg builds ports with VS 2022 even when newer Visual Studio versions are installed. Edit it if VS 2022 lives elsewhere.

## Quick start

```bat
build.bat              :: configure + build (debug). build.bat release for optimized
run.bat                :: run build\debug\kast_editor.exe from the repo root
run.bat release --size 1600x900   :: optional preset, then any editor flags
run.bat debug --ui-scale 2.0      :: force UI scale (default: monitor's display scale)
```

The editor UI (text, padding, tabs, menu bar) scales with the monitor's Windows display scale (e.g. 150% → 1.5x),
clamped to 0.75–3.0, and rescales live when the window moves to a monitor with a different scale. Resizing the window
does not change text size. `--ui-scale F` overrides the detected scale (out-of-range values are clamped with a warning).

Both work from any terminal (cmd, PowerShell, VS Code; in PowerShell type `.\build.bat` / `.\run.bat`); `build.bat` sets up the VS 2022 environment and
`VCPKG_ROOT=C:/vcpkg` itself. Running from the repo root means the layout file is `imgui.ini` there.

## Build (manual)

Ninja needs VS 2022's `cl` on `PATH`. Get a VS 2022 x64 build environment one of these ways:

- **Start menu:** type `x64 Native Tools` and pick **x64 Native Tools Command Prompt for VS 2022**
  (not the VS 2026 one if that is installed too).
- **Any `cmd` window:**
  `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"`
- **From PowerShell:** opens a `cmd` shell with the environment loaded:
  `cmd /k '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"'`

That environment points `VCPKG_ROOT` at Visual Studio's bundled vcpkg, so reset it, then check both:

```bat
set VCPKG_ROOT=C:/vcpkg
where cl
echo %VCPKG_ROOT%
```

The first `cl` listed should be `...\2022\Community\VC\Tools\MSVC\<version>\bin\Hostx64\x64\cl.exe`, and
`VCPKG_ROOT` should print `C:/vcpkg`. Then build and test:

```bat
cd /d C:\dev\kast

cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset release
cmake --build --preset release
ctest --preset release
```

Output goes to `build/<preset>/`: `kast_editor.exe`, `kast_player.exe` (the game client Build copies) and
`kast_self_check.exe`; runtime DLLs are copied next to them.

## Run

```bat
build\release\kast_editor.exe
```

| Flag | Effect |
|------|--------|
| `--frames N` | Run N frames, then quit cleanly (exit 0). |
| `--screenshot <path.png>` | With `--frames`: capture the backbuffer after frame N, write a PNG, then exit. Exit 1 if it can't be written. |
| `--size WxH` | Initial window size (default `1280x720`; Project Manager `1024x600`). |
| `--ui-scale F` | Force UI scale `F` (positive number, clamped to 0.75–3.0) instead of the monitor's display scale. |
| `--project <dir>` | Open `<dir>/project.kast` at startup (its `startScene`, else a new Untitled scene). Exit 1 if it is missing or invalid. With `--scene`, the scene opens afterwards and its own project (walk-up) wins. Once the editor window opens, the project is added to the top of the recent-projects list. |
| `--scene <path>` | Open this `.scene` at startup (inside a project or not). Exit 1 if it can't be loaded. |
| `--save-scene <path>` | With `--scene` or `--project`: save the startup scene to `path` and exit 0 without opening a window. Exit 1 on write failure. |
| `--projects-file <path>` | Use this recent-projects list instead of `%APPDATA%\Kast\Editor\projects.json` (Project Manager and editor; the Project Manager passes it on to the editor it starts). Tests always set it. |
| `--select <id>` | Select the entity with this persistent `id` at startup (shows it in the Inspector). Exit 1 if there is none. |
| `--play` | Enter Play mode right after the startup scene loads. Works with `--frames`, `--screenshot`, `--scene`, `--select`. |
| `--add-component-popup` | Open the Inspector's Add Component popup for the `--select`ed entity (after the first 10 frames, once the layout has settled). |
| `--new-component-prompt <Name>` | Open the Inspector's New Lua Component modal at startup with Name prefilled (validation shown). Creates nothing by itself. |
| `--new-state-machine-prompt <Name>` | Open the State Machines panel's New State Machine modal at startup with Name prefilled (validation shown). Creates nothing by itself. |
| `--new-component <Name>` | Create Lua component `<Name>` (as the modal's Create does) and add it to the `--select`ed entity at startup. On failure logs an error and keeps running. |
| `--launcher` | Run the [Project Manager](#project-manager) (what no `--project`/`--scene`/`--save-scene`/`--build` already does), e.g. with `--frames 120 --screenshot`. |
| `--new-project <name> <dir>` | Project Manager: open its New Project modal at startup with Name and Directory prefilled (quote values with spaces), e.g. for `--frames 120 --screenshot`. Creates nothing by itself. |
| `--launcher-open <dir>` | Project Manager without a window: open `<dir>` exactly as double-clicking it does (add it to the top of the list, start `kast_editor --project <dir>`), wait for that editor and exit with its exit code; exit 1 if the project is invalid or the editor can't start. |
| `-- <flags...>` | Project Manager: every argument after `--` is appended to the editor it starts, e.g. `--launcher-open tests/project -- --frames 120 --screenshot out.png` makes that editor take a screenshot and exit. |
| `--build <dir>` | With `--project`: build that project's game into `<dir>` (see [Build](#build)) and exit without opening a window or any dialog. Exit 0 on success, 1 on failure (no `--project`, invalid `startScene`, ...). Does not remember the folder. |
| `--open-menu <Name>` | Open that main menu (e.g. `Build`) after the first 10 frames, for screenshots. |
| `--open-build-settings` | With `--project`: open the Build > Build Settings modal after the first 10 frames, for screenshots. Writes nothing by itself. Without `--project` it is an invalid argument. |
| `--open-online-settings` | With `--project`: open the Build > Online Services modal after the first 10 frames, for screenshots. Writes nothing by itself. Without `--project` it is an invalid argument. |
| `--focus-panel <Window>` | Bring that editor window (e.g. `Events`, `"State Machines"`, `"GUI Preview"`) to the front of its dock after the first 10 frames, for screenshots. `Lighting`, `"Post Processing"`, `Physics`, `Mixer` and `Network` also open those hidden panels. |
| `--no-scene-post` | Start with the Scene toolbar's **Post** box off (scene without post effects). |
| `--assets-menu <path>\|/` | Open that Assets entry's context menu (`/` = the panel background) with its Create submenu open, for screenshots. |
| `--assets-create system\|fsm\|gui:<folder>` | Run Assets > Create > Lua System / State Machine / GUI in that project folder (empty = the root) after the first 10 frames, as the menu does (the new file's rename starts). |
| `--open-gui <file.rml>` | Open that `.rml` in the [GUI Builder](#gui-builder) at startup (relative paths: to the project, else the working directory) and bring its four panels to the front. |
| `--gui-select <id>` | With `--open-gui`: select the element with this `id` (logs an error if there is none). |
| `--gui-preview-size WxH` | Add `WxH` to the GUI Preview's resolutions and pick it. |
| `--gui-pick X,Y` | With `--open-gui`: click the GUI Preview at (X, Y) in preview pixels after 16 frames, as the mouse does, and log `--gui-pick X,Y selected '<id>'`. |
| `--gui-add Panel\|Text\|Button` | With `--open-gui`: add that element as the GUI Hierarchy's **+** buttons do (into the selected Panel, else `<body>`), after 17 frames. |
| `--gui-save` | With `--open-gui`: save the GUI as its **Save** button does, after 19 frames. |
| `--drop-model <path>` | Add this project model file (project-relative) as dropping it from the Assets panel on the Scene view does: an entity at the origin named after the file, with a `ModelRenderer` and, if the model has clips, an `Animator` (selected). |
| `--no-vsync` | Present frames as fast as possible instead of at the display's refresh rate (with `--log-fps`, to measure headroom). |
| `--no-audio-device` | Play sound on miniaudio's null backend (silent), as if there were no output device: one warning, everything else behaves normally. Tests use it. |
| `--log-fps` | Log once a second: frames per second (vsync caps it at the display's refresh rate), the editor's CPU time per frame and bgfx's GPU frame time, and the draw call count. |
| `--gui-click <id>` | With `--play`: click the game GUI element with this `id` (topmost visible document holding one) after 10 frames, as the mouse does; logs an error if there is none. |

Without `--project`, `--scene`, `--save-scene` or `--build`, `kast_editor` runs the Project Manager instead of the
editor. Editor-only flags (`--select`, `--play`, `--add-component-popup`, `--new-component[-prompt]`,
`--new-state-machine-prompt`, `--open-menu`, `--open-build-settings`, `--open-online-settings`, `--focus-panel`, `--assets-*`, `--open-gui`,
`--gui-*`) then are invalid arguments, as are Project Manager flags (`--launcher`, `--new-project`,
`--launcher-open`, `--`) together with the editor's.

Invalid arguments exit with code 2; init failures (SDL, window, bgfx) log the reason, show a message box, and exit 1.

## Editor controls

Scene view (only while the mouse is over the Scene panel; a drag that starts there keeps control until released):

| Input | Action |
|-------|--------|
| RMB held + mouse | Look around (fly) |
| RMB held + W/A/S/D, Q/E | Fly forward/left/back/right, down/up. Shift = 4x speed |
| Alt + LMB drag | Orbit around the pivot |
| MMB drag | Pan |
| Mouse wheel | Zoom (dolly toward the pivot) |
| LMB click | Select the nearest mesh or camera under the cursor (a camera is picked within 0.25 m of its origin); empty space clears the selection |
| F | Frame the selected entity |
| W / E / R | Gizmo: move / rotate / scale (also the toolbar) |
| X | Toggle gizmo Local / World space (also the toolbar button) |
| Delete, Ctrl+D | Delete / duplicate the selected entity and its children |
| Ctrl+P | Play / Stop (works anywhere in the editor) |

Letter shortcuts work only when the Scene panel is focused and no text field is active.

The Scene view draws each `Camera` as white frustum lines out to 2 m (a preview at 16:9, not the far plane). Select a
camera and the gizmo moves/rotates it like any entity.

**Game panel** (a tab beside Scene; Scene is the front tab on first run): renders the scene through the **active
camera**, the first entity with a `Camera` component in hierarchy (depth-first) order, at the panel's aspect ratio.
No grid, axes, gizmos or frustum lines. It renders live in edit mode too; systems only tick while playing, so it
animates while playing and holds still while paused. With no camera it shows "No Camera in scene", and Play logs a
warning but still runs. Layouts saved before the Game panel existed get it docked beside Scene automatically.

While the Game panel is focused and playing, editor shortcuts (W/E/R, X, F, Delete, Ctrl+D, Hierarchy keys) don't
apply, since they need their own panel focused; Ctrl+P still stops Play. Keyboard and mouse input then goes to
`Document::input` (`kast::Input` in `engine/input.h`: scancodes held down, mouse delta this frame) for game
systems to read; Lua systems read it through `kast.input`. It is cleared whenever the Game panel loses focus or Play stops. The
cursor stays normal (no capture).

**Systems panel** (below Hierarchy): the scene's Lua systems in run order. **Add Script...** picks an existing `.lua`
file inside the active project (a file outside it is rejected with an error), **New from Template** writes
`<project>/scripts/new_system.lua` (then `new_system_2.lua`, ...) and adds it; both store project-relative paths. The
arrows reorder and **Remove** drops an entry (the file stays). With no active project both add buttons are disabled
(tooltip "Open a scene inside a project"); New from Template also needs a saved scene (tooltip "Save the scene
first"). The list can't be edited during Play. Each edit marks the scene dirty. While playing,
each entry shows **OK** or **Disabled** with its last error. Layouts saved before the panel existed get it docked as a
tab beside Hierarchy.

**Components panel** (below Systems): the active project's [Lua components](#lua-components), one row per definition
file: **OK** or **Error**, the type name and its project-relative path, and the error message for a rejected file.
**New from Template** writes `<project>/components/new_component.lua` (type `NewComponent`; then
`new_component_2.lua` with `NewComponent2`, ...), a valid definition with one field and every field type shown
commented, and loads it at once. It is disabled with no active project (tooltip "Open a scene inside a project") and
during Play ("Stop Play to add components"). Layouts saved before the panel existed get it docked as a tab beside
Systems.

**State Machines panel** (tabbed with Console): the active project's [state machine](#state-machines) definitions, one
row per file: **OK** or **Error**, the machine name and its project-relative path, and the error message for a
rejected file. A valid one has a **Global** checkbox that adds it to (or removes it from) `project.kast`
`"stateMachines"` and saves that file at once; while playing, a Global machine shows its live `State:`. A listed name
with no valid definition is shown in red with **Remove**. **New State Machine...** opens a modal like New Lua
Component: type a name (e.g. `TrafficSignal`), Create writes a template definition (states `idle` and `active`, every
option shown) to `<project>/fsms/<snake_case>.lua` and rescans. Create is disabled, with the reason in red, for an
empty name, a non-identifier, the name of any listed definition (even one in error) or an existing file
(case-insensitive); it never overwrites. Global and New State Machine are disabled with no active project and during
Play ("Stop Play to change state machines"). The list is rescanned every 0.5 s, also during Play (Play itself reloads
its own copy). Layouts saved before the panel existed get it docked as a tab beside Console.

**New Lua Component...** (first item of the Inspector's Add Component popup) creates a Lua component and adds it to the
selected entity in one step. Type a name (e.g. `PlayerHealth`) and choose Create (or Enter; Esc cancels): it writes the
New from Template definition with that type name to `<project>/components/<snake_case>.lua` (`player_health.lua`;
`HTTPServer` -> `http_server.lua`), loads it and adds it. The name is checked as you type, with the reason shown in red
and Create disabled: empty, not an identifier (letters, digits, `_`; not starting with a digit), a built-in name, the
name of any listed definition (even one in error), or a file that already exists (case-insensitive). It never
overwrites a file. Disabled with the same tooltips as New from Template.

Hierarchy: click to select, right-click for Create / Rename (F2) / Duplicate (Ctrl+D) / Delete (Del). Creating from an
entity's menu makes the new entity its child; right-click empty space creates at the root. Drag an entity onto another
to reparent it (world transform kept; dropping onto a descendant is rejected), or onto empty space to move it to the
root. The **GameObject** menu creates at the root.

Inspector: the name, then one collapsible section per component, with widgets generated from the component's
registered fields (rotation as Euler degrees, colors as color pickers, enums as combos). The **x** on a section header
removes that component (not shown for Transform); **Add Component** lists the registered types the entity doesn't have.

**Play mode** (Scene toolbar Play / Pause / Stop, or Ctrl+P): Play snapshots the scene in memory and runs the systems
(e.g. `Rotator`) every frame; Pause holds them, Play or Pause again resumes; Stop restores the snapshot exactly (scene,
unsaved-changes flag, selection by id; the editor camera stays where it is). Play (from stopped, including `--play`)
brings the Game tab to the front; Stop brings the Scene tab back; Pause and resume leave the tabs alone. While
playing, the Scene panel has an orange border and the title says `(Playing)` or `(Paused)`. Edits during Play are allowed and are discarded on Stop;
they never mark the scene dirty. File New/Open/Save/Save As are disabled during Play; Exit or
closing the window stops Play first. A Lua `kast.loadScene` switch replaces the running scene: Hierarchy, Inspector,
Scene, Game and the Systems panel status show the new scene, the selection is cleared, the editor camera stays, and the
title shows `(Playing: <scene>)` / `(Paused: <scene>)`. The edited scene's file and path are never touched; Stop
restores the pre-Play scene, path, project, dirty flag and selection however many switches happened.

**GUI Builder** (GUI Hierarchy, GUI Inspector, GUI Style, GUI Preview): double-click a `.rml` in Assets. See
[GUI Builder](#gui-builder).

**File**: New (Ctrl+N), Open... (Ctrl+O), Save (Ctrl+S), Save As... (Ctrl+Shift+S), Exit. Projects are created and
opened from the [Project Manager](#project-manager), not the editor. The title shows `Kast Editor - <project> - <scene>`
with an active project, `Kast Editor - <scene>` without, plus `*` when there are unsaved changes; New, Open, Exit and
closing the window ask Save / Don't Save / Cancel first. Opening an invalid scene shows an error and keeps the current
scene. Moving the camera does not mark the scene dirty, but Save stores the current camera pose.

- Save / Save As never rewrite `systems` paths. If Save As moves a scene with systems into another project or out of
  any project, the Console warns that its system paths may no longer resolve.

## Project Manager

Running `kast_editor.exe` with no project or scene (or with `--launcher`) opens the **Kast Project Manager** window
(1024×600, resizable) instead of the editor. Opening or creating a project there starts a new
`kast_editor.exe --project <dir>` process (`SDL_CreateProcess`, sharing the console) and closes the Project Manager's
window. Its process then waits for that editor and exits with the editor's exit code, so a terminal (`run.bat`) stays
attached until the editor closes and its log stays in the terminal. Closing the Project Manager without choosing exits
0 and starts nothing.

- **List** (left), most recently opened first: the project name, its folder and when it was last opened. Click selects;
  double-click, Enter or **Open** opens the selected project. A project whose folder is missing or whose `project.kast`
  is missing or invalid (checked when the window opens) is greyed out with the reason, and can't be opened (Open's
  tooltip says why); it stays listed until removed.
- **New Project**: a modal with **Name**, **Directory** (type a parent directory or pick one with **Browse...**;
  cancelling the picker keeps the field) and a `Will create: <directory>\<folder>` preview. The folder name is the name
  trimmed, with ASCII letters lower-cased and each run of whitespace replaced by `-` (`"  My   Game  "` → `my-game`);
  non-ASCII characters are kept. **Create** is disabled, with a one-line reason, if that folder name is empty,
  `.`/`..`, ends with `.` or contains `< > : " / \ | ? *` or a control character, if the directory is empty, missing or
  not a directory (parent directories are never created), or if `<directory>\<folder>` already exists. Create makes
  that one folder, writes `scenes/main.scene` in it (the editor's default scene: light, ground, cube, Main Camera and
  the editor camera framing them) and `project.kast` (`"name"` = the trimmed typed name,
  `"startScene": "scenes/main.scene"`), adds it to the list and opens it, so the editor starts in `main`. A failure shows under the list and removes the folder if this attempt made it. Cancel or Esc
  writes nothing.
- **Import**: pick a `project.kast` (the dialog filters `*.kast`; other file names are rejected). An invalid file shows
  the error under the list and changes nothing. Otherwise the project is added to the top of the list (an already
  listed folder just moves there, never twice) and opened.
- **Remove**: drops the selected entry from the list. Files on disk are never touched; no confirmation.
- If the editor can't be started, the error shows under the list (and in the log) and the Project Manager stays open;
  the list keeps the update.

**Recent-projects list**: `%APPDATA%\Kast\Editor\projects.json` (`SDL_GetPrefPath("Kast", "Editor")`), or
`--projects-file <path>`. A JSON array, most recent first, 2-space indent:

```json
[
  {
    "path": "C:/dev/games/my-game",
    "name": "My Game",
    "lastOpened": 1791900000
  }
]
```

`path` is the absolute, normalized project folder (`/` separators; compared case-insensitively, so a folder is listed
once), `name` the project name when last opened, `lastOpened` Unix seconds. Every successful open adds or moves the
project to the top: Open/double-click/Enter, Import, New Project, `--launcher-open`, and the editor's own `--project`
startup once its window opens (not `--build` / `--save-scene`). An unreadable or invalid file is copied to
`projects.json.bad`, a warning is logged, and the list starts empty.

## Projects

A project is a directory marked by a `project.kast` file (exactly that name):

```json
{
  "version": 1,
  "name": "My Game",
  "startScene": "scenes/start.scene",
  "stateMachines": ["game_flow"],
  "audio": {"Music": {"volume": 0.6, "muted": false}},
  "online": {"enabled": true, "authUrl": "https://auth.example.com", "caFile": "certs/dev_ca.pem"}
}
```

- `version` must be 1, `name` a non-empty string, `startScene` a project-relative path or `null` (only its type is
  checked; opening the project opens it, and a built game starts there, see [Build](#build)). These three keys are
  required.
- `stateMachines` (optional): the [game state machines](#state-machines) every Play session runs, as definition names;
  an array of unique non-empty strings. A name without a valid definition is an error at Play start, not here. The
  State Machines panel's **Global** checkbox edits it.
- `audio` (optional): the default [bus](#audio) levels applied at Play start, `{"<bus>": {"volume": 0..1, "muted":
  bool}}`; a missing bus or key is 1.0 / unmuted. The Mixer panel edits it. Written (every bus) only when some bus is
  not at 1.0 unmuted.
- `online` (optional): the project's opt-in to **Online Services**, the built-in integration with the repo's auth and
  world servers (`services/`); later engine features check `enabled` to decide whether online features exist. A
  missing block (every older project) means disabled. Keys, all optional:
  - `enabled` (bool, default `false`).
  - `authUrl`: the auth service's `http://` or `https://` base URL. Required while enabled; checked whenever set.
    World server addresses are not configured here: they come from the auth service's realm list.
  - `caFile`: a project-relative PEM file of extra trusted CA certificates (e.g. the world stack's self-signed dev
    certificate), trusted besides the Windows ROOT store for both auth HTTPS and world TLS. It ships in `game.pak` like
    every project file, so it must not be in a dot-folder.

  Loading checks the shape, the URL and that `caFile` is a path inside the project; that the file exists and holds a
  valid certificate is checked by the **Online Services** modal's Save, by Build and when the built game starts (only
  while enabled), so a deleted CA file never stops the project from opening. Written only when some key differs from
  the defaults, so disabling keeps the URL and CA file. Older editors reject the `online` key; projects that never
  set it stay readable by them. Never put secrets (tokens, passwords, admin credentials) here: it ships in `game.pak`.
- Unknown keys are rejected. Saved with 2-space indent in this key order; `stateMachines` is written only if the file
  had it or a machine was made Global (unchecking the last one leaves `[]`), so other projects keep identical bytes.
- **Active project**: opening a scene (File > Open, `--scene`) activates the project found by walking up from the
  scene's directory (the nearest ancestor-or-self with a `project.kast`; none = no project; an invalid one is warned
  about and treated as none). New (Untitled) keeps the current project. Save / Save As re-run the walk-up from the saved
  file. `--project` (how the Project Manager opens a project) sets it directly.
- **Project-relative paths** (`systems`, `kast.loadScene`, `startScene`) use `/` separators and must stay inside the
  project: absolute paths and `..` out of the root are rejected.
- Without an active project the scene can still be edited and played (C++ systems run), but every Lua system is
  disabled with a "no project" error.

## Build

**Build** menu (enabled when a project is open and Play is stopped):

- **Build Settings...** opens a modal with one setting, **Start Scene**: a dropdown of every `*.scene` in the project
  that would ship in `game.pak` (no dotfiles or dot-directories), as sorted project-relative `/` paths, listed when the
  modal opens. It starts on the current `startScene`; one that is set but not listed shows as `<path> (missing)`. With
  no scenes the dropdown is disabled and the modal says `No scenes in project. Save a scene first.` **Save** (enabled
  once a listed scene is chosen) checks that the scene loads, then writes it as `startScene` to `project.kast`
  (nothing else changes: not the open scene, its unsaved changes or `.kast/editor.json`); a failure shows in the modal
  and writes nothing. **Cancel** / Escape closes without writing. There is no way to set `startScene` back to `null`
  here.
- **Online Services...** opens a modal editing the project's [`online`](#projects) block: **Enabled**, **Auth URL** and
  **CA File** (typed as a project-relative path, or **Browse...** for a `.pem`/`.crt`; a file outside the project shows
  as an error; **Clear** removes it). It is checked as you edit (while enabled: URL set and http(s), CA file exists
  inside the project, holds a valid PEM certificate and is not in a dot-folder); the first problem shows in red and
  disables **Save**. Opening it shows a problem already in the saved settings (e.g. a deleted CA file). **Save** writes
  only the `online` block to `project.kast`; **Cancel** / Escape writes nothing.
- **Build...** asks for an output folder, then builds the game into it.
- **Build and Run** builds into the project's last output folder (asks if there is none), then starts the game as a
  separate process.

Both first offer to save unsaved scene changes (the same prompt as File > Open), because the game is built from the
files on disk. The last output folder is remembered per project in `<project>/.kast/editor.json`
(`{"buildFolder": "..."}`), never in `project.kast`. Errors go to the Console and an Error popup.

If `startScene` is not set, outside the project or doesn't load, **Build...** and **Build and Run** log why and open
Build Settings instead (after the unsaved-changes prompt, before any folder dialog); saving there doesn't resume the
build, run it again. Headless `--build` fails instead (exit 1), its error pointing at Build > Build Settings.

Before writing anything, Build checks that `startScene` is set, inside the project and loads, that the Online Services
settings pass the modal's checks (nothing extra while disabled), that the output folder
isn't the project folder itself, and that `kast_player.exe` and its DLLs are next to the editor. Then it writes, into
the output folder (created if needed, overwriting only these files):

```
<Name>.exe   copy of kast_player.exe
game.pak     the project's files
SDL3.dll     runtime DLLs kast_player.exe imports, directly or through RmlUi (dumpbin /dependents)
lua51.dll
rmlui.dll    game GUI, with FreeType and its libraries: freetype.dll, zlib1.dll, bz2.dll, libpng16.dll,
             brotlidec.dll, brotlicommon.dll (a debug build: freetyped.dll, zlibd1.dll, bz2d.dll, libpng16d.dll)
```

`<Name>` is the project name with each of `< > : " / \ | ? *` and control characters replaced by `_`, surrounding
spaces and trailing dots removed, `_` appended to a reserved device name (`CON`, `NUL`, `COM1`, ...) and `Game` if
nothing is left: `Switch Test` -> `Switch Test.exe`, `My: Game?` -> `My_ Game_.exe`.

**game.pak** holds every file under the project folder except dotfiles, anything in a dot-directory (`.kast/`, `.git/`)
and the output folder if it is inside the project. It is uncompressed: a `KPAK` magic and format version, an index of
(project-relative `/` path, offset, size) entries, then the raw bytes. The exact byte layout is in `engine/pak.h`. The
player checks the magic, the version and that every entry lies inside the file.

**The game** (`kast_player.exe` under its new name) opens the `game.pak` next to it, loads `project.kast` and its Lua
components from it, titles its window with the project name and plays `startScene` as editor Play mode does (C++
systems, Lua systems, Lua components, `kast.loadScene`), rendering through the scene's Main Camera. It has no console
window and doesn't hot reload. Its scene's [game GUIs](#game-gui) show over the scene and take mouse and keyboard
input. Flags (for tests): `--frames N` and `--screenshot out.png`, as in the editor, `--gui-click <id>` (click that
GUI element on frame 10), `--size WxH` (window size, default 1280x720), `--no-audio-device` (silent null audio backend) and `--gpu-stats` (log the average GPU time of
the frame's views, after 10 warmup frames, on exit). A missing
or corrupt `game.pak`, an invalid `project.kast`, Online Services enabled with a CA file missing from `game.pak` or
not a valid certificate, or a `startScene` that is missing or doesn't load shows an error
message box and exits 1; with `--frames` the message is only logged (stdout), so tests never block.

Ship from a `release` build: a `debug` build copies the debug player, which needs Visual Studio's debug runtime. Players
need the Microsoft Visual C++ Redistributable (x64); Build doesn't copy it.

## Coordinates and scenes

- Right-handed, **Y up**, 1 unit = 1 meter. Cameras and directional lights look down their local **-Z**.
- Rotations are stored as standard (Hamilton, glTF-style) quaternions `[x, y, z, w]`. The Inspector shows Euler
  degrees (X pitch, Y yaw, Z roll), applied Z, then X, then Y.
- Matrices follow bgfx/bx: row vectors, translation in elements 12–14, `world = local * parentWorld`.
- Built-in primitives fit the unit cube centered on the origin: `cube` (1 m), `plane` (1x1 m on XZ, facing +Y),
  `sphere` (0.5 m radius).
- Lighting (forward; shading model under [Materials](#materials)): `ambient + directional + point lights`.
  - **Ambient**: per scene, `"lighting"` in the scene file, edited in **Window > Lighting**. Default white at 0.2.
  - **Directional**: the first `DirectionalLight` in hierarchy order; others are ignored.
  - **Point**: every `PointLight`, from its entity's world position, falling off smoothly to exactly 0 at `range`
    (`saturate(1 - (d/r)^4)^2 / (d^2 + 1)`). **At most 8 per mesh**: the ones nearest the mesh's bounds center
    whose range reaches its bounds; the rest don't light that mesh. Selecting one in the Scene view shows its range.
  - **Shadows** (realtime shadow maps, in the Scene view, the Game view and the built game; no ambient occlusion,
    baked or contact shadows):
    - **Directional**: cascaded shadow maps when the `DirectionalLight`'s `castShadows` is on (default). The view
      from the camera's near plane to `maxDistance` (default 100 m, or the camera's far plane if nearer) splits into
      `cascadeCount` (1–4, default 4) cascades (a 0.75 mix of logarithmic and uniform splits), each 2048x2048 in one
      4096x4096 16-bit depth atlas. Each cascade is fit to a bounding sphere of its slice and moves in whole texels,
      so shadow edges don't shimmer as the camera moves or turns. Beyond `maxDistance` nothing is shadowed (a hard
      cut, no fade). Hardware depth comparison with 3x3 PCF.
    - **Point**: a 1024x1024-per-face 32-bit cube shadow map for each `PointLight` with `castShadows` on (default
      off), shadowing in every direction from 0.05 m to its `range`. **At most 4 per frame**: the `castShadows` lights
      nearest the camera; the rest still light, unshadowed. 3x3 PCF across the face.
    - **Acne vs. peter-panning**: `DirectionalLight.bias` (meters along the light, default 0.05) and `normalBias`
      (shadow-map texels along the surface normal, default 1.5); `PointLight.bias` (meters toward the light, default
      0.05; plus a fixed one-texel normal offset). Raise them for acne (striped self-shadowing), lower them if
      shadows detach from their casters.
    - **Per mesh**: `MeshRenderer` / `ModelRenderer` `castShadows` and `receiveShadows` (both default on). Alpha-mask
      materials cast a cutout shadow (alpha-tested in the depth pass); alpha-blend materials never cast but do
      receive. Skinned models cast their current pose.
    - Resolutions are engine constants (`engine/shadow.h`); shadow maps are created on first use, per renderer
      (the editor's Scene and Game views have one each). Each render path reserves `Renderer::kShadowViews` (28) bgfx
      views before its scene view: editor Scene view shadows 1–28, scene and post 29–40, overlays 41; Game view
      shadows 42–69, scene and post 70–81, game GUIs 82, GUI Builder preview 83, ImGui 255; player shadows 0–27, scene
      and post 28–39, GUIs 40.

Scene file format (`.scene`: JSON with 2-space indent, entities depth-first in hierarchy order, components and fields
in registration order, floats in shortest round-trip form, so re-saving an unchanged scene gives identical bytes):

```json
{
  "version": 4,
  "entities": [
    { "id": 1, "name": "Directional Light", "parent": null,
      "components": {
        "Transform": { "position": [0, 4, 0], "rotation": [-0.408218, -0.23457, -0.109382, 0.875426], "scale": [1, 1, 1] },
        "DirectionalLight": { "color": [1, 0.95, 0.9], "intensity": 1.1 } } },
    { "id": 4, "name": "Sphere", "parent": null,
      "components": {
        "Transform": { "position": [1.5, 0.75, 0], "rotation": [0, 0, 0, 1], "scale": [1.5, 1.5, 1.5] },
        "MeshRenderer": { "primitive": "sphere", "material": "materials/blue.mat" },
        "Rotator": { "axis": [0, 1, 0], "degreesPerSecond": 45 } } },
    { "id": 5, "name": "Satellite", "parent": 4,
      "components": {
        "Transform": { "position": [0.9, 0.6, 0], "rotation": [0, 0, 0.258819, 0.965926], "scale": [0.3, 0.3, 0.3] },
        "MeshRenderer": { "primitive": "cube", "material": "" } } },
    { "id": 6, "name": "Main Camera", "parent": null,
      "components": {
        "Transform": { "position": [0, 2.5, 6], "rotation": [-0.173648, 0, 0, 0.984808], "scale": [1, 1, 1] },
        "Camera": { "verticalFovDegrees": 60, "nearPlane": 0.1, "farPlane": 1000 } } }
  ],
  "systems": [],
  "lighting": { "ambientColor": [1, 1, 1], "ambientIntensity": 0.2 },
  "editor": { "camera": { "pivot": [0, 0.75, 0], "yaw": 20, "pitch": -20, "distance": 8 } }
}
```

- `id`: persistent nonzero integer, unique in the file. `parent` is the parent's `id`, or `null` for a root. Children
  keep file order.
- `components`: keyed by component type name; built-ins first (registration order), then [Lua
  components](#lua-components) (sorted by name), then preserved components of unknown type (sorted by name). For a
  built-in component every field is required, except the shadow fields below: added after scenes were saved without
  them, a missing one takes its default (no scene version change).
  - `Transform` (required): local `position`, `rotation` (quaternion), `scale`, relative to the parent.
  - `MeshRenderer`: `primitive` is `cube`, `plane` or `sphere`; `material` is a project-relative [`.mat`](#materials)
    file, empty for the default grey material. Scenes from before materials, with a `color` field instead, still load:
    `color` is dropped with a Console warning (the mesh renders with the default material). `castShadows` [true],
    `receiveShadows` [true] (see Shadows above).
  - `DirectionalLight`: `color` (linear RGB 0–1), `intensity`, `castShadows` [true], `bias` [0.05, >= 0],
    `normalBias` [1.5, >= 0], `maxDistance` [100, > 0], `cascadeCount` [4, 1–4].
  - `PointLight`: `color` (linear RGB 0–1), `intensity` (>= 0), `range` (meters, > 0), `castShadows` [false],
    `bias` [0.05, >= 0].
  - `Camera`: perspective camera looking down the entity's world -Z (scale ignored). `verticalFovDegrees` must be
    between 0 and 180 (exclusive), and `0 < nearPlane < farPlane`. The Inspector keeps the last valid value if an
    edit would break that.
  - `Rotator`: spins the entity in Play mode, `degreesPerSecond` around its local `axis` (normalized; zero = no spin).
  - `StateMachine`: gives the entity its own [state machine](#state-machines) in Play mode. `machine` is a definition
    name, `initialState` the state it enters at Play start (empty = the definition's `initial`). Both are plain
    strings: a name with no valid definition still loads and saves unchanged. Scene files stay version 4.
- `systems`: Lua system scripts in run order, as paths relative to the **project root** (see [Projects](#projects) and
  [Lua systems](#lua-systems)). Always written; optional when loading (missing = none).
- `lighting`: the scene's ambient light, `ambientColor` (linear RGB 0–1) times `ambientIntensity` (>= 0). Always
  written; optional when loading (missing = white at 0.2), but if present both fields are required.
- `postProcessing`: the scene's [post-processing and fog](#post-processing-and-fog), blocks `tonemapping`, `bloom`,
  `colorGrading`, `vignette`, `fxaa`, `fog`, each with `enabled`. Always written; optional when loading, and so is every
  block and field in it (missing = default, all effects off). Unknown blocks or fields, wrong types, out-of-range
  values and an `operator` other than `None` / `ACES` fail the load. The file stays version 4.
- `editor` belongs to the editor (camera pivot, yaw/pitch in degrees, distance in meters); the engine keeps it as-is.
- Loading rejects invalid JSON, a `version` other than 4 (versions 1–3 are not migrated), a `systems` that
  isn't an array of non-empty strings, a missing `Transform`, unknown, missing or mistyped fields of a built-in
  component, unknown enum names, out-of-range `Camera` or `PointLight` values, a malformed `lighting` or `postProcessing`, duplicate ids, parents that don't exist, and parent
  cycles. It reports why and leaves the current scene untouched. Lua components are lenient (schema drift), and a
  component name that is neither built-in nor a valid Lua definition is kept verbatim with a Console warning (missing
  definitions); see [Lua components](#lua-components).

### Materials

Meshes render with PBR metallic-roughness materials (glTF semantics) defined in `.mat` files: JSON, any folder of the
project, referenced by `MeshRenderer.material` (project-relative). Every field but `version` is optional (default in
brackets); unknown keys and invalid values make the file invalid.

```json
{
  "version": 1,
  "baseColor": [0.8, 0.8, 0.8, 1],
  "baseColorTexture": "textures/checker.png",
  "metallic": 0,
  "roughness": 0.5,
  "metallicRoughnessTexture": "",
  "emissiveEnabled": false,
  "emissive": [0, 0, 0],
  "emissiveIntensity": 1,
  "alphaMode": "opaque",
  "alphaCutoff": 0.5
}
```

- `baseColor` linear RGBA 0–1 [0.8 grey, alpha 1], times `baseColorTexture` (sRGB; project-relative, empty = none).
- `metallic`, `roughness` 0–1 [0, 0.5], times `metallicRoughnessTexture` (linear, glTF packing: G = roughness,
  B = metallic).
- `emissiveEnabled` [false]: when on, `emissive` (linear RGB 0–1 [black]) times `emissiveIntensity` (≥ 0 [1]) is added
  after lighting (shows without lights); an intensity that lifts it above the bloom `threshold` glows. Older files with
  only `emissive` load with emission on if it is non-black, and channels above 1 move into the intensity.
- `alphaMode` [`opaque`]: `mask` discards pixels whose alpha is below `alphaCutoff` [0.5]; `blend` draws after every
  opaque mesh, far to near by the mesh's world-space center (no per-triangle sorting), alpha-blended, depth-tested but
  not depth-written.
- Shading: Cook-Torrance (GGX distribution, Smith / Schlick-GGX geometry, Schlick Fresnel, F0 = mix(0.04, baseColor,
  metallic)) for the first `DirectionalLight` and up to 8 `PointLight`s per mesh; ambient adds a diffuse term and a flat
  specular term (roughness-aware Fresnel), so metals are not black with ambient light only. Direct light is scaled by
  the [shadow maps](#coordinates-and-scenes) (the directional term by its cascade, each shadowed point light by its
  cube) unless the mesh's `receiveShadows` is off. No image-based lighting. The shader outputs linear HDR color
  (formulas in `engine/shaders/fs_mesh.sc`); the [post chain](#post-processing-and-fog) tonemaps and encodes it to sRGB.
- Textures: PNG and JPG (stb_image), mipmapped at load (sRGB-correct filtering for base color), shared by path.
- Fallbacks (the scene always loads): a missing or invalid `.mat` renders unlit magenta; a missing or undecodable
  texture renders as white. Each is logged once per path, with the reason.
- **Editor**: Assets > Create > Material writes `new_material.mat` (default material) into the folder and starts a
  rename. Selecting a `.mat` in Assets shows it in the Inspector (until an entity is selected): color pickers, sliders
  (clamped), the alpha mode and texture combos (the project's `.png` / `.jpg`); every edit saves the file and the views
  show it the same frame. The MeshRenderer Inspector picks a material from the project's `.mat` files or None
  (default). Renaming or moving a `.mat` or texture (or a folder holding them) in Assets rewrites the open scene's
  `MeshRenderer.material` paths and the texture paths inside every valid `.mat`; deleting one leaves the references to
  fall back. `.mat` and texture files changed on disk (e.g. in an external editor; double-click opens a `.mat` there)
  reload within a frame: the renderer checks each loaded file's modification time every frame (not in the built game).
- **Lua** (Play): `kast.set(e, "MeshRenderer", {material = "materials/red.mat"})` swaps the material.
  `kast.setMaterialProperty(id, name, value)` changes one field for that entity only: the first call copies its material
  into a per-entity instance (Unity's `renderer.material`); other entities using the same `.mat` are unaffected.
  `kast.getMaterialProperty(id, name)` reads the instance, else the `.mat`. Values are checked like `.mat` fields
  (invalid raises); colors are `{r, g, b[, a]}` or `{x =, y =, z =[, w =]}` and come back as arrays. Instances are
  never saved: Stop discards them, and `kast.set` to another material drops the entity's instance.
- **Built game**: `.mat` and texture files ship in `game.pak` like every other project file; the player renders them
  the same way.
- **Sample**: `tests/project/materials.scene` (gold and red plastic spheres at roughness 0.1 / 0.5 / 0.9, PNG and JPG
  textured cubes, a metallic-roughness textured sphere, an emissive sphere, a mask cutout, two overlapping blended
  planes, a broken and a missing `.mat`, a missing texture) with its `materials/*.mat` and `textures/*`.

### Models & Animation

Model files placed anywhere in the project render through a `ModelRenderer`; skinned characters play skeletal
animation clips through an `Animator`. Loading uses Assimp at runtime (no cooked format), straight from the source file.

- **Formats**: glTF 2.0 (`.gltf` with its `.bin` and images, or `.glb`), FBX (`.fbx`) and OBJ (`.obj` with its `.mtl`).
  Sidecar files resolve next to the model through the engine's file layer, so they also load from `game.pak`.
- **Units and axes**: everything is normalized to meters and Y-up on load. glTF is meters / Y-up; OBJ has no unit and
  is taken as meters; FBX is converted from its `UnitScaleFactor` (centimeters per unit) and its up / front / coord
  axis metadata (e.g. Z-up files are turned Y-up). The same 1 m cube lines up as OBJ and glTF. Entity
  `Transform.scale` is the only user scale. (The sample Kenney FBX character is authored 3.76 m tall; `models.scene`
  scales it by 0.5.)
- **What loads**: meshes (one submesh per material slot; positions, normals, UV0, up to 4 joint weights per vertex,
  renormalized), the node hierarchy, skins (bind poses), animation clips (name, duration, per-node translation /
  rotation / scale keys) and embedded materials mapped onto [`.mat` fields](#materials): base color factor and texture,
  metallic, roughness, metallic-roughness texture, emissive, alpha mode and cutoff (OBJ `Kd` / `d`). Embedded GLB
  images decode like other textures (PNG/JPG). A mesh on a node without a skin renders at that node's transform.
- **Caching**: each model loads once per project-relative path and is shared by every entity using it; a changed file
  reloads within a frame (modification time, as `.mat` files; not in the built game).
- **`ModelRenderer`**: `model` (project-relative `.glb/.gltf/.fbx/.obj`; empty = renders nothing) and `materials`, the
  per-slot `.mat` overrides by slot index: a JSON array of strings, `""` (or no entry) = the model's embedded material.
  `castShadows` / `receiveShadows` [true] as for `MeshRenderer` (skinned meshes cast their animated pose).
  It is the first built-in field that is a list (`std::vector<std::string>`); it saves, loads, duplicates and goes
  through `kast.get` / `kast.set` like any other field. No scene version change.

  ```json
  "ModelRenderer": { "model": "models/female-128-anim.glb", "materials": ["materials/gold_r05.mat"] },
  "Animator": { "clip": "Idle_A", "loop": true, "speed": 1, "playOnStart": true }
  ```

- **`Animator`**: `clip` (a clip name of the model; empty = bind pose), `loop` [true], `speed` [1, 0–10; values
  outside are rejected] and `playOnStart` [true]. It drives the `ModelRenderer` on the same entity: **without one it
  does nothing** (the Inspector says so, `kast.*Animation*` raise). Keys are sampled with lerp (translation, scale) and
  slerp (rotation), looping wraps and not looping holds the last frame (then `playing` turns false). Switching clips
  crossfades from the current pose over the fade time. Root-motion clips (`*_RM`) play in place: the root bone moves as
  authored, the entity `Transform` never moves. Playback state (clip, time, fade, pose) is runtime-only: never saved,
  reset by Stop.
- **GPU skinning**: `vs_mesh_skinned.sc` (same PBR fragment shader, lights, alpha modes and fog), 4 weights per vertex,
  bone matrices in a uniform array of **at most 128 bones per skinned mesh**. A model over the limit logs one error and renders
  that mesh in bind pose, unskinned.
- **Edit mode**: Animators preview in the Scene view: a `playOnStart` clip plays; otherwise the clip's first frame shows,
  paused. The Animator Inspector has a **Clip** dropdown (the model's clips) and a **Preview** row: Play / Pause and a
  time scrub (dragging pauses). The preview never saves and never marks the scene dirty; Play mode starts every
  `playOnStart` clip from 0.
- **Editor**: drag a model from the Assets panel onto the Scene view (an entity at the origin) or the Hierarchy (at the
  root, or onto an entity for a child): the entity is named after the file and gets a `ModelRenderer`, plus an
  `Animator` if the model has clips, playing `Idle` if there is one, else `Idle_A`, else the first clip whose name
  contains "idle" (any case), else the first clip. **GameObject > Model** (and the Hierarchy's right-click menu) adds an
  empty `ModelRenderer`. Its Inspector has a **Model** combo (the project's model files) and a `.mat` combo per
  material slot, named as in the file, with **None (embedded)**. Scene view picking and focus use the model's bind-pose
  bounds. Renaming or moving a model, a `.mat` or a folder holding them in Assets rewrites the open scene's
  `ModelRenderer.model` and override paths.
- **Lua**:

  | Function | Does |
  |----------|------|
  | `kast.playAnimation(id, clip, opts)` | Plays `clip`; optional `opts = {loop =, speed =, fade = seconds}` (fade crossfades from the current pose, default 0). Writes the Animator's `clip`, `loop`, `speed`. |
  | `kast.stopAnimation(id)` | Stops at the current pose. |
  | `kast.getAnimationState(id)` | `{clip = name, time = seconds into the clip, playing = bool}`. |
  | `kast.getAnimationClips(id)` | The model's clip names, in file order. |

  `kast.get` / `kast.set` work on `Animator` fields (a `clip` change starts that clip). An invalid entity, an entity
  without `Animator` + `ModelRenderer`, a model that didn't load, an unknown clip, or a speed / fade out of range raise a
  Lua error naming the problem.
- **Fallbacks** (the scene always loads): a missing, corrupt or unsupported model renders a magenta cube at the entity
  and warns once per path; an unknown `Animator.clip` (from the scene or the Inspector) shows the bind pose and warns
  once; over 128 bones, see above.
- **Built game**: model files and their sidecars ship in `game.pak` like every other project file (with the Assimp DLLs
  next to the exe); the player renders and animates them the same way.
- **Limits**: 128 bones per skinned mesh, 4 weights per vertex, one UV set; no morph targets, no node (non-skinned)
  animation playback, no animation layers / blend trees / events / IK, no root-motion extraction, no LOD or instancing,
  embedded images only as PNG/JPG. Animations stored in separate animation-only files (e.g. Kenney's `run.fbx`) are not
  merged into a model. Assimp 6.0.4 crashes on a glTF skin without `inverseBindMatrices` (valid glTF); such files must
  include them.
- **Samples** (`tests/project/`): `models.scene` (OBJ cube, glTF cube, the GLB character, Kenney's CC0 FBX character
  with a `.mat` skin override, next to primitives), `models_anim.scene` (Cheering_Two_Hands on start),
  `models_material.scene` (slot 0 overridden with gold), `models_missing.scene` (missing model), `models_lua.scene`
  (`scripts/animation_test.lua`), with `models/` (`models/kenney/License.txt`: CC0).

### Post-processing and fog

Each scene can opt into an HDR post-processing stack and distance fog, set per scene (`"postProcessing"` in the scene
file) and edited in **Window > Post Processing** (hidden by default; a section per effect with its **Enabled** box;
edits show live and mark the scene dirty). Every effect is off by default, so a scene without settings renders as it
always did. The Scene view, the Game view and the built game all apply it.

- **Pipeline**: meshes render linear HDR color into an `RGBA16F` target (plus depth); `engine/post_process.h` then
  runs, in order: bloom, exposure, tonemap, color grading, vignette, sRGB encode, FXAA, into the view's image (the
  backbuffer in the player). Overlays (grid, axes, camera frustums, light range) and gizmos draw after it, depth-tested
  against the scene, and are never post-processed; game GUIs draw over the final image and are never post-processed.
- **Tonemapping**: `exposure` in EV (-10..10, color times 2^EV) and `operator` `None` (clamp to 0–1, the look without
  post) or `ACES` (Narkowicz's filmic fit, `x(2.51x + 0.03) / (x(2.43x + 0.59) + 0.14)`: bright lights and emissive
  surfaces roll off instead of clipping to flat white).
- **Bloom**: a soft-knee bright pass on HDR luminance above `threshold` [1] at half resolution, blurred down 5 levels
  (to 1/32) and back up, each level mixing in the blurred level below by the `radius` [0.5] scatter; added back times
  `intensity` [0.5]. Raise a material's emissive intensity above the threshold to make it glow.
- **Color grading** (after the tonemap): `contrast` [1, 0–2] (power curve around mid grey 0.18), `saturation` [1, 0–2],
  `colorFilter` [white, linear RGB tint] and `temperature` [0, -1 cool .. 1 warm white balance].
- **Vignette**: `intensity` [0.3, 0–1] darkens toward the corners over the outer `smoothness` [0.5, 0.01–1].
- **FXAA**: on/off, on the final sRGB image (the FXAA 3.11 console variant).
- **Fog** (shaded per mesh in `fs_mesh.sc`, not a post pass, so blended meshes fog by their own distance):
  exponential-squared, `1 - exp(-(density * max(distance - start, 0))^2)` toward `color` (linear RGB) [density 0.05,
  start 0 m]. While fog is on, the views clear to the fog color so distant meshes fade into the background.
- **Scene view**: the toolbar's **Post** box shows the scene with or without the post effects (on by default; fog
  always shows). `--no-scene-post` starts with it off.
- Blended meshes now blend in linear space (before the sRGB encode), which is physically correct but looks slightly
  different from the gamma-space blending before post-processing.

### Components and systems

Components are registered once with `entt::meta` in `engine/components.cpp` (`registerComponent<T>`, a stable type
name, then each field with a display name and an optional color hint). `ComponentInfo::validate` optionally checks
values beyond their types (e.g. `Camera` ranges); loading rejects and the Inspector reverts what it refuses. Field types: `float`, `int`, `bool`,
`std::string`, `std::vector<std::string>` (a JSON array of strings), `bx::Vec3`, `bx::Quaternion` and registered enums. Registration alone gives a component scene-file
save/load, copy on Duplicate, Inspector widgets and Add/Remove Component. `PersistentId`, `Name` and `Hierarchy` are
structural and not registered.

The uniform layer at the end of `engine/components.h` (`ComponentType`, `FieldDesc`, `allComponentTypes`,
`findAnyComponentType`, `hasComponent/addComponent/removeComponent/componentToJson/setComponentJson`) covers built-in
and [Lua components](#lua-components) alike, with JSON as the common value form; scene IO, the Inspector,
Add/Remove Component, Duplicate, Play snapshots and `kast.*` use it. The `entt::meta` functions above stay for C++ code
working with built-ins.

Systems (`engine/systems.h`) are plain functions `void(Scene&, float dt)`, listed in update order in
`engine/systems.cpp`. Only Play mode runs them, once per frame with the real frame time, never while paused or editing
(except that the editor also runs `animatorSystem` in Edit mode, for the [animation preview](#models--animation)).

### Lua systems

Game logic can be written as global ECS systems in Lua (LuaJIT, bound with sol2; `engine/lua_systems.h`). A scene
lists its system files in `"systems"`; Play runs each once per frame, **after** the C++ systems and in listed order.
They don't run while stopped or paused.

**File shape**: one `.lua` file is one system and returns a table:

```lua
local speed = 2 -- file-level locals and globals are private to this system

return {
    init = function() kast.log("ready") end,           -- optional: once when Play starts (or after a hot reload)
    update = function(dt)                                -- required: every Play frame, dt in seconds
        for e in kast.query("Transform", "MeshRenderer") do
            local p = kast.get(e, "Transform").position
            p.y = p.y + speed * dt
            kast.set(e, "Transform", {position = p})
        end
    end,
    shutdown = function() kast.log("bye") end,          -- optional: on Stop (or before a hot reload)
}
```

A file that doesn't return a table with an `update` function, or whose `init`/`shutdown` isn't a function, is an
error for that system.

**Paths**: `"systems": ["scripts/spin.lua", "shared/ai.lua"]` are relative to the active project's root (`/`
separators), wherever the scene file is. The Systems panel stores paths that way; Save As does not rewrite them. With no
active project, or for a path outside the project, that system is disabled with the reason.

**`kast` API** (one read-only table per system; assigning to `kast`, `kast.input` or `kast.fsm` is an error). Entities are
persistent ids (`EntityId` integers, the scene file's `id`). An invalid or destroyed id, an unknown component name or
a wrong argument type raises a Lua error at the calling line.

| Function | Returns | Notes |
|----------|---------|-------|
| `kast.query(name, ...)` | iterator of ids | Entities that have every listed component, in hierarchy order: `for e in kast.query("Transform", "Rotator") do`. No names = every entity. The matches are captured when `query` is called: entities destroyed later in the loop are skipped, entities created in the loop are not visited. |
| `kast.get(id, name)` | table | A copy of every field. Raises if the entity doesn't have the component. |
| `kast.set(id, name, fields)` | — | **Partial**: only the listed fields change. All-or-nothing: an unknown field, a wrong type or a value the component rejects (e.g. `Camera` ranges, a zero quaternion) raises and leaves the component unchanged. |
| `kast.getMaterialProperty(id, name)`, `kast.setMaterialProperty(id, name, value)` | value, — | The entity's material, per entity: see [Materials](#materials). Raises without a `MeshRenderer`, for an unknown property or (set) an invalid value. |
| `kast.has(id, name)` | boolean | |
| `kast.add(id, name)` | — | Default-constructed; no-op if already present. |
| `kast.remove(id, name)` | — | No-op if absent. `Transform` can't be removed (raises). |
| `kast.create(name [, parentId])` | id | New entity with a `Transform`, at the root or as the last child of `parentId`. |
| `kast.destroy(id)` | — | Destroys the entity and its children immediately. |
| `kast.log(msg)`, `kast.warn(msg)`, `kast.error(msg)` | — | To the Console, prefixed with the system's path: `[info] scripts/spin.lua: msg`. |
| `kast.input.keyDown(name)` | boolean | SDL scancode names (`SDL_GetScancodeFromName`): `"W"`, `"Space"`, `"Left Shift"`, `"Up"`... An unknown name raises. |
| `kast.input.mouseDelta()` | `dx, dy` | Pixels moved this frame. |
| `kast.loadScene(path)` | — | Switches the running scene. `path` is project-relative. Checked **at call time**, reading the file from disk (unsaved editor edits are not seen): no active project, a path outside the project, a missing file or a scene that fails to load (including an old version) raises at the calling line, and nothing is queued. On success the switch is applied at the **end of the frame**, after every system's `update`; the last call in a frame wins. Applying it calls the old scene's enabled systems' `shutdown`, discards the Lua state, replaces the scene, and starts a new session for the new scene's own `systems` (each `init` runs). Nothing persists across the switch. Calls from a `shutdown` are ignored; a call from an `init` is applied at the end of the next ticked frame. |
| `kast.publish(name [, payload])`, `kast.subscribe(name, fn)`, `kast.unsubscribe(handle)` | —, handle, — | Events between systems and C++: see [Events](#events). |
| `kast.currentScene()` | string or `nil` | The running scene's project-relative path (`"levels/b.scene"`), or `nil` with no active project or for an Untitled scene. |
| `kast.fsm.state(target)`, `kast.fsm.change(target, state)`, `kast.fsm.canChange(target, state)`, `kast.fsm.states(target)` | string, boolean, boolean, table | Game and entity state machines: see [State machines](#state-machines). `kast.fsm` is read-only too. |

Input is only live while the Game panel is focused and playing; otherwise every key is up and the delta is 0.

These work the same on [Lua components](#lua-components) (`kast.get(e, "Health")`, ...), whose `array` fields are
1-based Lua sequences (`{"gold", "potion"}`); `kast.set` replaces an array field as a whole.

Field values: `float`/`int` are Lua numbers (an `int` field needs an integral number; numbers must be finite),
`bool` is a boolean, `std::string` a string, `bx::Vec3` a table `{x=, y=, z=}`, `bx::Quaternion` `{x=, y=, z=, w=}`
(all keys required when setting), and enums their scene-file names (`"cube"`, `"plane"`, `"sphere"`). Component and
field names are the scene file's (`"MeshRenderer"`, `"degreesPerSecond"`).

**Sandbox**: one LuaJIT state per Play session; each file runs in its own environment with its own copies of the
globals and of the `table`, `string` and `math` libraries, so two systems can use the same global names. Only the
`base`, `table`, `string` and `math` libraries are loaded, and `dofile`, `loadfile`, `load`, `loadstring`, `require`,
`module`, `getfenv` and `setfenv` are removed: `io`, `os`, `ffi`, `debug`, `jit`, `bit` and `package` are not
reachable. Files are loaded as text only (no precompiled bytecode).

**Errors**: a syntax error, a missing file, a bad returned table or an error in `init`, `update` or `shutdown` (Lua
or raised by `kast.*`) is logged to the Console with the file and line (`Lua system disabled: scripts/spin.lua:12:
...`, plus a traceback for runtime errors) and disables that system until the next Play or a successful hot reload.
Play and the other systems keep running. Disabled systems' `shutdown` is not called. With no active project, every
listed system is disabled with `no project (open a scene inside a project to run Lua systems)`.

**Hot reload**: while playing, each listed file's modification time is checked every frame. When it changes, the old
instance's `shutdown` runs (if it was enabled), the file is reloaded into a fresh environment and its `init` runs.
A successful reload clears the error; the scene and entities are not reset, but the system's Lua locals start over.

### Events

Lua systems and engine C++ code exchange named events with plain-data payloads through one event bus per Play session
(`engine/event_bus.h`, built on `entt::dispatcher`; owned by `LuaSystems`, so editor Play and `kast_player` both have
it). Publishing only queues an event; the queue is delivered once per frame at a fixed point.

```lua
local handle
return {
    init = function()
        handle = kast.subscribe("health.empty", function(event)  -- event: a fresh copy of the payload
            kast.log("empty: " .. event.id)
        end)
    end,
    update = function(dt)
        kast.publish("heartbeat", {time = dt, tags = {"a", "b"}})
        if kast.input.keyDown("U") then kast.unsubscribe(handle) end
    end,
}
```

| Function | Returns | Notes |
|----------|---------|-------|
| `kast.publish(name [, payload])` | — | Queues an event. `name`: a non-empty string. `payload`: plain data (below); omitted or `nil` is JSON `null`. The payload is deep-copied now: later changes to the table don't affect the event. |
| `kast.subscribe(name, fn)` | handle | `fn(payload)` is called for every later event named exactly `name` (no wildcards). `name`: a non-empty string, `fn`: a function. |
| `kast.unsubscribe(handle)` | — | Stops deliveries at once, including the rest of the current drain. A handle already unsubscribed, or one owned by another system, is a no-op; any value that isn't a handle raises. |

A wrong argument raises a Lua error at the calling line, and nothing is queued or subscribed.

**Payloads** are plain data: `nil`, booleans, finite numbers, strings, and tables of these. A table whose keys are
exactly `1..n` becomes a JSON array; one with only string keys becomes an object; an empty table is `{}`. Mixed or
other keys (`{1, x = 2}`, `{1, nil, 3}`, `{[0] = 1}`, `{[true] = 1}`), functions, userdata, threads, cycles, NaN or
infinity, and tables nested more than **32** deep raise at the `publish` call (`payload.a.b: ...` names the bad
value). Integral numbers travel as JSON integers. Each subscriber gets its own fresh copy: mutating it affects no one
else. JSON arrays arrive as 1-based sequences, objects as string-keyed tables; a `null` inside a payload arrives as
`nil` (in an array that leaves a hole).

**Delivery**: the queue drains once per frame in `LuaSystems::tick`, **after** every Lua system's `update` and before
a `kast.loadScene` switch is applied, so an event published in `update` reaches subscribers in the same frame. Events
are delivered in publish order; each event goes to its subscribers in subscription order, C++ and Lua subscribers in
one shared order. Events published while the queue drains (by Lua or C++ subscribers) are delivered **next frame**,
so a subscriber that re-publishes can't loop within a frame. A subscription added during a drain takes effect from the
next event.

**Ownership**: a Lua subscription belongs to the system whose code called `kast.subscribe` (from `init`, `update`, a
subscriber or the file body). It is removed when that system is disabled (by any error), when it is hot-reloaded (the
old instance's subscriptions go before the new file runs), and when the Lua state goes on Stop or a scene switch. The
publisher recorded for a Lua event is the system's path as listed in the scene (`scripts/health_events.lua`).

**Errors, Pause, Stop, scene switch**:
- A Lua subscriber that raises is logged with file:line and its system is disabled exactly like an `update` error
  (which also removes all its subscriptions until a successful hot reload). The event's other subscribers still run,
  and Play continues.
- A C++ subscriber that throws is caught and logged (`Event 'x': C++ subscriber failed: ...`); delivery continues.
- Paused: nothing is delivered; the queue is held until Play resumes.
- Stop: queued events are discarded (including any published by `shutdown`). Play start resets the bus: queue, every
  subscription (C++ ones too), the history and the frame count.
- `kast.loadScene` switch: undelivered events (published during that frame's drain or by the old `shutdown`s) are
  dropped and the old scene's Lua subscriptions go with its Lua state. C++ subscriptions and the history carry over.

**C++ API** (`kast::EventBus`, from `LuaSystems::bus()`; no Lua or ImGui dependency):

```cpp
kast::EventBus& bus = lua.bus();
kast::EventBus::Subscription sub = bus.subscribe("health.empty", [](const nlohmann::json& payload) { /* ... */ });
bus.publish("door.opened", {{"id", 7}});   // publisher "C++"; delivered at the next drain
sub.unsubscribe();                          // or let the handle go out of scope (RAII, move-only)
```

`drain()` delivers (`LuaSystems::tick` calls it), `dropQueued()` discards the queue, `reset()` clears everything, and
`history()` returns the last 500 delivered events (`frame` = drains since Play start, `name`, compact one-line JSON
`payload`, `publisher`). A `Subscription` must not outlive its bus; after `reset()` it is a harmless no-op. Physics
publishes the [collision and trigger events](#physics).

**Events panel** (editor, tabbed with Console): the last 500 delivered events, oldest first, following new ones while
scrolled to the bottom. Columns: frame, event name, compact payload, publisher (script path or `C++`). **Clear**
empties it; the filter shows events whose name contains the text (case-sensitive). Play start clears it; it stays
after Stop. `--focus-panel Events` brings it to the front for screenshots.

**Sample**: `tests/project/events.scene` runs `scripts/health_events.lua` (a `heartbeat` every 20 frames and, on frame
30, `health.empty` with `{id, title}` for every entity with `Health`; it changes nothing itself) and
`scripts/on_health_empty.lua` (greys out the named entity's material with `kast.setMaterialProperty`).

### Physics

3D physics on [Jolt](https://github.com/jrouwe/JoltPhysics) (`engine/physics.h`). One Jolt world per Play session,
built from the scene at Play start (and on a `kast.loadScene` switch) and dropped at Stop; the editor's Stop then
restores the pre-Play scene as usual. Nothing simulates in Edit mode or while paused. Editor Play and `kast_player`
both run it.

**Components** (Add Component, Inspector, scene file; values outside the ranges are rejected):

| Component | Fields [default] |
|-----------|------------------|
| `Collider` | `shape` `box` / `sphere` / `capsule` [box]; `size` box full extents [1, 1, 1] (> 0); `radius` sphere / capsule [0.5] (> 0); `height` capsule **total** height along local Y, both hemispheres included [2] (>= 2 * radius); `center` local offset [0, 0, 0]; `isTrigger` [false]; `friction` [0.5] (>= 0); `restitution` [0] (0..1) |
| `Rigidbody` | `motionType` `static` / `dynamic` / `kinematic` [dynamic]; `mass` kg [1] (> 0); `gravityScale` [1]; `linearDamping` [0.05] (>= 0); `angularDamping` [0.05] (>= 0) |
| `CharacterController` | `radius` [0.4]; `height` total [1.8] (>= 2 * radius); `maxSlopeDegrees` [45] (0..90); `stepHeight` [0.3] (>= 0) |

- A `Collider` without a `Rigidbody` is a static body. A `Rigidbody` without a `Collider` is ignored, with one warning
  naming the entity. `CharacterController` plus `Collider` or `Rigidbody` is an error: logged once naming the entity,
  shown in red in the Inspector, and the entity gets no physics.
- Bodies use the entity's **world** transform at any hierarchy depth. World scale applies: a box per axis, a sphere or
  capsule by the largest axis; `center` is scaled too. After each step dynamic and kinematic bodies and characters
  write their pose back into the entity's local `Transform` (relative to its parent; scale untouched).
- **Kinematic** bodies follow their `Transform` (moved over one step, so they push dynamic bodies) and move by a
  velocity set with `kast.physics.setVelocity`.
- **Runtime changes** apply before the next step: adding / removing `Collider` / `Rigidbody` / `CharacterController`
  (`kast.add` / `kast.remove`, the Inspector) creates or removes the body; changing their fields rebuilds it;
  destroying the entity removes it. A `Transform` changed by anything (`kast.set`, `Rotator`, the gizmo in Play, a
  moved parent) teleports a static or dynamic body (a dynamic one keeps its velocity), moves a kinematic one and sets
  a character's position.

**Gravity**: per scene, `"physics": {"gravity": [x, y, z]}` in the scene file [0, -9.81, 0] (each axis -1000..1000;
missing = default, so older scenes load unchanged), edited in **Window > Physics** (hidden by default; changes apply
at once, also in Play, and Stop reverts them). `--focus-panel Physics` opens it.

**Timestep**: a fixed 60 Hz step (`kPhysicsStep`) with an accumulator, independent of the frame rate; at most 4 steps
per frame (`kMaxPhysicsSteps`), time beyond that is dropped (the game slows down instead of spiralling). Each Play
frame: C++ systems, then the physics steps (contact events queued), then the Lua `update`s, then the event drain; so
contact events reach subscribers in the same frame, and what Lua changes applies at the next frame's step. Rendering
shows the last step's pose (no interpolation).

**Events** (on the session's event bus, publisher `C++`; ids are the entity ids `kast.*` uses; no "stay" events):

| Event | Payload |
|-------|---------|
| `collisionEnter` | `{a, b, point = {x, y, z}, normal = {x, y, z}}`: the first contact point; `normal` points from `a` toward `b` |
| `collisionExit` | `{a, b}` |
| `triggerEnter` / `triggerExit` | `{a, b}`, when either collider `isTrigger` (triggers block nothing) |

Each fires once per pair when it starts / stops touching (a bouncing ball sends enter, exit, enter...; a body falling
asleep on the floor does not end its contact). Destroying an entity or removing its body sends the exits of its
contacts at once. Characters send these like any body.

**Lua: `kast.physics`** (vectors `{x =, y =, z =}` or `{x, y, z}`; rotations `{x, y, z, w}`; bad arguments and the
wrong kind of body raise at the calling line):

| Function | |
|----------|-|
| `raycast(origin, direction, maxDistance)` | `nil` or `{entity, point, normal, distance}`, the closest hit; `direction` need not be normalized |
| `overlapSphere(center, radius)`, `overlapBox(center, halfExtents[, rotation])` | list of entity ids touching the shape |
| `getVelocity(id)` / `setVelocity(id, v)` | dynamic or kinematic body, or character |
| `getAngularVelocity(id)` / `setAngularVelocity(id, v)` | dynamic or kinematic body (radians / s) |
| `addForce(id, f)` / `addImpulse(id, i)` | dynamic body; a force acts over the next step, an impulse at once (`dv = i / mass`) |
| `moveCharacter(id, v)` | character: walk at `v`'s horizontal part from now on (call again to change or stop; gravity is added) |
| `isGrounded(id)` | character: standing on walkable ground |
| `getGround(id)` | character: `nil` or `{entity, normal}` of what it stands on |

Queries see triggers and characters too. A raycast from inside a character hits that character.

```lua
local hero
return {
    init = function()
        hero = kast.query("CharacterController")()
        kast.subscribe("triggerEnter", function(e) kast.log("trigger " .. e.a .. " " .. e.b) end)
    end,
    update = function(dt)
        local x = (kast.input.keyDown("D") and 3 or 0) - (kast.input.keyDown("A") and 3 or 0)
        kast.physics.moveCharacter(hero, {x, 0, 0})
        if kast.input.keyDown("Space") and kast.physics.isGrounded(hero) then
            local v = kast.physics.getVelocity(hero)
            kast.physics.setVelocity(hero, {v.x, 5, v.z})  -- jump
        end
    end,
}
```

**Character controller** (Jolt `CharacterVirtual`): an upright capsule whose bottom is the entity's position (feet,
like imported characters' pivot); physics never changes its rotation. It is blocked by static and kinematic bodies,
pushes dynamic ones, walks up steps up to `stepHeight` and slopes up to `maxSlopeDegrees`, sticks to the floor going
down steps, and falls with the scene gravity (characters are Y-up: only gravity's Y part pulls them). `setVelocity`
sets walk and vertical speed together (a jump). It also has an inner body, so bodies, queries and triggers see it.

**Editor**: the selected entity's collider (or character capsule) draws as a green wireframe in the Scene view, as
physics builds it (world position, rotation, scale, center).

**Limits**: box, sphere and capsule colliders only (no mesh or convex hull), one collider per entity, no joints, no
collision layers (everything collides with everything), no stay events, no interpolation, client-side only (not
networked). Resting bodies sink up to 2 cm into each other (Jolt's penetration slop). Capacity: 65536 bodies.

C++: `kast::Physics` (from `LuaSystems::physics()`) has the same queries and controls (`std::invalid_argument` for the
wrong body) and `update(dt)` returning the steps taken. `kast_self_check --bench-physics` times 500 dynamic boxes.

**Sample**: `tests/project/physics.scene` (ground with a scaled box collider, two falling cubes, a bouncy sphere, a
trigger zone the first cube falls through, a character) runs `scripts/physics_log.lua`, which logs the four events and
a raycast at init.

### Audio

2D (non-spatial) sound on [miniaudio](https://miniaud.io) (`engine/audio.h`). Editor Play and `kast_player` both run
it; in Edit mode only the Inspector's **Preview** makes sound.

**Formats**: `.wav` and `.mp3` only (extension matched ignoring case); any other extension is an error. Every file is
read through `kast::readFile`, so a built game plays its clips from `game.pak`.

**`AudioSource`** component (Add Component, Inspector, scene file):

| Field | Meaning [default] |
|-------|-------------------|
| `clip` | project-relative `.wav` / `.mp3`; empty = none [""] |
| `bus` | `Master` / `Music` / `SFX` / `UI` / `Voice` [SFX] |
| `volume` | 0..1 [1] |
| `loop` | repeat until stopped [false] |
| `playOnStart` | start the clip when Play starts and when a scene loads during Play [false] |

A volume outside 0..1 or a clip with another extension is rejected (scene load, Inspector edits). A missing or
corrupt clip shows its error in red under the Inspector's clip picker, logs one error at Play
(`AudioSource on '<entity>': ...`), plays nothing, and Play goes on.

**Buses**: a fixed set, Master, Music, SFX, UI, Voice; every sound plays on exactly one (default SFX). Music, SFX, UI
and Voice feed Master. Gain = sound volume x bus volume x Master volume; a muted bus (or muted Master) is silent.
Volumes are 0..1.

**Voices**: at most 32 sounds at once. A play when all 32 are busy stops the **oldest non-looping** voice and reuses
it (its handle goes stale). Looping voices are never stolen: if all 32 loop, the new sound is dropped with a warning
(`Sound '<clip>' dropped: all 32 voices are looping`) and `play` returns 0 (a stale handle). A non-looping voice frees
itself when its clip ends; a looping one repeats seamlessly until stopped.

**Loading**: clips on the Music bus are decoded while they play (streamed from the file's bytes, one decoder per
voice). All other clips are decoded once, converted to the output format, and cached by project-relative path; the
cache is dropped at each Play start, so edited files are picked up.

**Lifecycle**: Play start stops every voice, sets the buses to the project defaults (below) and starts the
`playOnStart` sources (before the Lua `init`s). Pause holds every voice where it is; resume continues them from that
position. Stop stops and frees every voice. A `kast.loadScene` switch stops every voice, then the new scene's
`playOnStart` sources start; runtime bus levels carry over the switch. Music does not continue across a switch.

**`kast.audio`** (bus names are the strings above; errors raise at the calling line, like `kast.*`):

| Function | |
|----------|-|
| `play(path, opts?) -> handle` | `opts = {bus = "SFX", volume = 1, loop = false}`, every key optional; returns 0 if dropped |
| `stop(h)`, `pause(h)`, `resume(h)`, `setVolume(h, v)` | on one sound |
| `isPlaying(h) -> bool` | true while the voice lives and is not paused by `pause(h)` |
| `stopAll(bus?)` | every sound, or only those on `bus` |
| `setBusVolume(bus, v)`, `getBusVolume(bus) -> v`, `setBusMuted(bus, muted)` | runtime bus levels, not saved |
| `playSource(id) -> handle`, `stopSource(id)` | the entity's `AudioSource` (its clip, bus, volume, loop); `stopSource` stops the last voice `playSource` or `playOnStart` started for it |

A handle whose voice ended, was stopped or was stolen is **stale**: `isPlaying` returns false and the other calls do
nothing (not an error). A missing, unreadable or corrupt file, another extension, an unknown bus or option, a volume
outside 0..1, a non-number handle and `playSource` / `stopSource` on an entity without an `AudioSource` raise a Lua
error (`kast.audio.play: cannot read 'sounds/x.wav'`); the system keeps running.

```lua
local music, wasDown = nil, false
return {
    init = function()
        music = kast.audio.play("sounds/theme.mp3", {bus = "Music", volume = 0.6, loop = true})
    end,
    update = function(dt)
        local down = kast.input.keyDown("Space")
        if down and not wasDown then
            kast.audio.play("sounds/jump.wav", {volume = 0.8}) -- SFX, one-shot
        end
        wasDown = down
        if kast.input.keyDown("P") then kast.audio.pause(music) else kast.audio.resume(music) end
    end,
    shutdown = function() kast.audio.stopAll("SFX") end,
}
```

**No audio device**: if the output device can't be opened, one warning is logged
(`No audio output device: sound plays on the null backend (silent)`) and miniaudio's null backend is used; handles,
voices, buses and the cache behave normally. `--no-audio-device` (editor and `kast_player`) forces this; the tests use it.

**Editor**:
- Inspector `AudioSource`: a **Clip** combo of the project's `.wav` / `.mp3` files, then bus, volume, loop and
  playOnStart. **Preview** plays the clip in Edit mode at the component's volume and bus (with the project's bus
  defaults); **Stop Preview** stops it, and selecting another entity stops it too. Preview is disabled during Play.
- Assets panel: `.wav` / `.mp3` files have a speaker icon.
- **Window > Mixer**: a volume slider and Mute box per bus that edit the project's default levels, saved in
  `project.kast` (`"audio"`, see [Projects](#projects)) when a drag ends; during Play edits also apply to the session
  at once. During Play it also lists the active voices (clip, bus, volume, loop, Playing / Paused) and the count out
  of 32.

C++: `kast::Audio` (from `LuaSystems::audio()`); `Audio::Output::Offline` has no device and `render()` pulls the mix,
which the self-checks use to measure output. `kast::clipInfo` reads a clip's native frames, channels and sample rate.

**Limits**: no 3D / spatial sound (no listener, distance or panning), no effects, pitch or speed, no fades or
crossfades, no OGG / FLAC, no custom buses, no voice priorities.

**Sample**: `tests/project/audio.scene` (Music loops `sounds/wave-demo.wav` on the Music bus with `playOnStart`;
Jukebox has a UI-bus `AudioSource`) runs `scripts/audio_sfx.lua`, which plays `sounds/mp3-demo.mp3` once and then a
short tone twice a second, alternating `kast.audio.play` and `kast.audio.playSource`.

### Networking

Client TCP (plain or TLS), WebSocket (`ws://`, `wss://`) and HTTP/HTTPS from Lua (`kast.net`), on Boost.Asio + Beast
with OpenSSL (`engine/network.h`). Editor Play and `kast_player` both run it. Lua may reach any host and port.

**Lifetime**: connections belong to the Play session, not the scene. A `kast.loadScene` switch keeps every connection
open (same socket) and every request in flight; the new scene's systems re-acquire connections with
`kast.net.get(name)` and subscribe to the events again. Connections close, and requests are cancelled, only on editor
**Stop**, game exit, an explicit `close`, the peer or an error; Play always starts with none. There is no
auto-reconnect: reopen on `net.closed`.

**`kast.net`** (errors raise at the calling line as `kast.net.<fn>: ...`, like `kast.*`):

| Function | Does |
|----------|------|
| `open{name, type, ...} -> handle` | starts connecting (asynchronously); options below. A name that is connecting or open raises; a closed one is replaced |
| `get(name) -> handle \| nil` | the connection of that name (open, connecting or closed by the peer), nil if none |
| `close(name)` | closes it after its queued sends (`net.closed` "closed by script" follows) and forgets the name; unknown names are ignored |
| `request{url, method, headers, body, encoding, caFile, timeout} -> id` | a one-shot HTTP/HTTPS request; `id` (a number) is in its `net.http.response` / `net.http.error` |
| `base64Encode(bytes) -> text`, `base64Decode(text) -> bytes` | standard padded base64 (decode raises on anything else) |

A handle is a read-only table: `h.name`, `h:send(data)`, `h:close()`, `h:state()` (`"connecting"`, `"open"` or
`"closed"`). Handles act by name, so one from an earlier scene still works. `send` before the connection opens queues
the data; `send` on a closed or unknown connection raises.

`open` options:

| Option | Meaning [default] |
|--------|-------------------|
| `name` | string, unique among open connections (required) |
| `type` | `"tcp"` or `"ws"` (required) |
| `host`, `port` | tcp: the server (required) |
| `tls` | tcp: TLS over the stream [false] |
| `url` | ws: `ws://host[:port][/path]` or `wss://...` (required) |
| `framing` | tcp: `"raw"`, `"u16le"` or `"line"` [raw] |
| `encoding` | `"text"`, `"base64"` or `"json"` [text] |
| `caFile` | project-relative PEM file of extra trusted certificates (read from `game.pak` in a built game) |
| `timeout` | seconds for DNS + connect + TLS/WebSocket handshakes [10] |

`request` options: `url` (`http://` or `https://`, required), `method` ["GET"], `headers` (a string-to-string
table), `body` (sent as `encoding` says), `encoding` (of both bodies) [text], `caFile`, `timeout` (the whole request,
seconds) [10]. No redirects are followed.

**Events** (on the session's [event bus](#events), publisher `kast.net`; `conn` is the connection's name):

| Event | Payload |
|-------|---------|
| `net.open` | `{conn}` |
| `net.message` | `{conn, data}`: one message (see framing) as the encoding says |
| `net.closed` | `{conn, reason}`: `"closed by script"`, `"closed by peer"` or the error's message; once per connection |
| `net.error` | `{conn, message}`: a failure (followed by `net.closed`), or a message that didn't fit the encoding (dropped; the connection stays open) |
| `net.http.response` | `{id, status, headers, body}`: headers by name (repeats joined with `", "`), body as the encoding says |
| `net.http.error` | `{id, message}` |

**Encodings** (per connection or request; sends mirror receives):
- `text`: data is a UTF-8 string. Received bytes that aren't valid UTF-8 give a `net.error`; sending invalid UTF-8
  raises. WebSocket messages go as text frames.
- `base64`: received bytes arrive as base64 text (`kast.net.base64Decode` gives the raw bytes); `send` takes base64
  and sends the decoded bytes. WebSocket messages go as binary frames. Use this for binary protocols.
- `json`: received data is parsed into a Lua table (or number, string, boolean; invalid JSON gives a `net.error`);
  `send` takes plain data (as `kast.publish` payloads) and sends it as compact JSON.

**TCP framing**: `raw` delivers each read as it arrives (message boundaries are whatever TCP gives; a UTF-8 character
split across reads fails `text`, so prefer the others for text); `u16le` splits on a 2-byte little-endian length
prefix counting the bytes after it, and `send` prepends it (at most 65535 bytes); `line` splits on `\n` (removed), and
`send` appends one (data containing `\n` raises). Split and coalesced reads are reassembled. WebSocket messages are
whole messages already. Note: the world server's frames (`u16 length | u16 opcode | payload`, see
`services/world/PROTOCOL.md`) have a length that excludes the opcode, so `u16le` doesn't split them; use `raw` with
`base64` and reassemble frames in Lua.

**TLS** (`tls = true`, `wss://`, `https://`): the server's certificate must verify against the Windows ROOT
certificate store or `caFile`, and match the host name (or IP) connected to; SNI is sent for names. There is no way to
turn verification off. For a self-signed development server (e.g. the world server's) put its certificate in the
project and pass it as `caFile`.

**Limits**: one message, frame, line or HTTP body is at most 16 MiB: a larger one received gives a `net.error` and
closes the connection (fails the request); a larger send raises. Connect and request timeouts default to 10 s. DNS
failure, refused connection, TLS failure, timeout and peer close all end in events, never a Lua error.

**Threading**: one background thread runs every socket; Lua and the event bus are only touched on the main thread.
Results queue in the network layer (not the bus queue, which a scene switch drops) and are published at the start of
the next `LuaSystems` tick, before the systems' updates; after a `kast.loadScene` that is after the new scene's
inits, so nothing received around the switch is lost. While the editor is paused nothing is published (results wait).

```lua
-- Scene A: log in over HTTPS, then open the game connection.
return {
    init = function()
        kast.subscribe("net.http.response", function(e)
            kast.net.open{name = "world", type = "tcp", host = "localhost", port = 7777, tls = true,
                          caFile = "certs/world_dev.pem", encoding = "base64"}
        end)
        kast.net.request{method = "POST", url = "https://localhost:8443/login", encoding = "json",
                         body = {user = "me", password = "secret"}}
        kast.subscribe("net.open", function(e) if e.conn == "world" then kast.loadScene("town.scene") end end)
    end,
    update = function(dt) end,
}
-- Scene B (town.scene): the same connection, still open.
return {
    init = function()
        local world = kast.net.get("world")
        kast.subscribe("net.message", function(e)
            if e.conn == "world" then local bytes = kast.net.base64Decode(e.data) --[[ parse frames ]] end
        end)
        kast.subscribe("net.closed", function(e) kast.warn("lost " .. e.conn .. ": " .. e.reason) end)
    end,
    update = function(dt) end,
}
```

**Editor**: **Window > Network** (read-only) lists, during Play, a card per connection (name, state, type, host:port
or url, bytes in/out, last error) and the HTTP requests in flight; otherwise an empty-state line.

C++: `kast::Network` (from `LuaSystems::network()`; no Lua or ImGui): `open`, `send`, `close`, `request`, `poll` (the
queued events) and `status()` (what the panel shows). Every call must come from the thread that made it (asserted in
debug builds).

**Built game**: Build ships OpenSSL's `libssl-3-x64.dll` and `libcrypto-3-x64.dll` next to the exe.

**Sample**: `tests/project/net_a.scene` (`scripts/net_a.lua`) opens TCP, TLS, ws and wss connections to
`kast_net_test_server` (`tests/net_test_server.cpp`, ports 47701-47706) and, in the frame all four open, starts a
100-line stream and two slow requests and switches to `net_b.scene`; `scripts/net_b.lua` re-acquires the connections,
checks they are the same sockets and that every tick and both responses arrive, and logs `PASS` or `FAIL`.

### Lua components

Game developers can define data-only ECS component types in Lua, inside their project (`engine/lua_components.h`).
Once defined, a Lua component behaves like a built-in one: saved in scenes, edited in the Inspector, listed by Add
Component, copied on Duplicate, snapshot/restored by Play and reachable from Lua systems through `kast.*`.

**Files**: every `.lua` file under `<project>/components/`, scanned recursively, is one definition. They are loaded when
a project becomes active (opening a scene or project, New Project, `--project` / `--scene` at startup). No project =
no Lua components. Each file returns exactly one table:

```lua
-- components/health.lua
return {
  name = "Health",          -- type name and scene-file key; required, unique
  displayName = "Health",   -- optional: Inspector header / Add Component label, defaults to name
  fields = {                -- required list (may be empty), in save and Inspector order
    { name = "current", type = "float",  default = 100, min = 0 },
    { name = "lives",   type = "int",    default = 3, min = 0, max = 9 },
    { name = "regen",   type = "bool",   default = true },
    { name = "title",   type = "string", default = "Hero" },
    { name = "tint",    type = "vec3",   color = true, default = {x = 1, y = 0, z = 0} },
    { name = "facing",  type = "quat" },                                     -- identity
    { name = "faction", type = "enum",   values = {"neutral", "ally", "enemy"}, default = "neutral" },
    { name = "drops",   type = "array",  of = "string", default = {} },
  },
}
```

| `type` | Value (Lua / JSON) | Options |
|--------|--------------------|---------|
| `float` | number (stored as a 32-bit float) | `min`, `max` |
| `int` | integral number (32-bit) | `min`, `max` (integers) |
| `bool` | boolean | |
| `string` | string | |
| `vec3` | `{x=, y=, z=}` / `[x, y, z]` | `color = true`: color picker in the Inspector |
| `quat` | `{x=, y=, z=, w=}` / `[x, y, z, w]`, nonzero | |
| `enum` | one of `values`, as a string | `values`: non-empty list of unique non-empty strings (required) |
| `array` | Lua sequence / JSON array of `of` | `of`: any type above except `array` (no nesting); an enum array also takes `values`; `min`/`max` on a float/int array apply to each element; `color` on a vec3 array |

- Names (`name` of the type and of each field) are letters, digits and `_`, not starting with a digit. Field names are
  unique within the type.
- `default` is optional. Without it a field gets its type's zero value moved into `min`/`max`: `0`, `false`, `""`,
  `{0,0,0}`, the identity quaternion, the first enum value, an empty array. A given default that violates the field's
  type, range or `values` is a definition error.
- Unknown keys (at the top level or in a field) are errors, so typos don't pass silently.

**Sandbox**: one Lua state per scan, separate from Play's, each file in its own environment with the same rules as
[Lua systems](#lua-systems): only `base`, `table`, `string` and `math`; `io`, `os`, `ffi`, `debug`, `jit`, `package`,
`require`, `dofile`, `loadfile`, `load` and `loadstring` are not reachable; text only.

**Errors**: a file that isn't valid Lua, raises, doesn't return exactly one table, or breaks any rule above is
rejected. The message starts with the project-relative path (and the line for Lua errors, e.g.
`components/health.lua:3: ...`), is logged as `Lua component rejected: ...` and shown in the Components panel. Valid
files in the same project still load.

**Name clashes**: a name equal to a built-in component (`Transform`, `MeshRenderer`, `DirectionalLight`, `Camera`,
`Rotator`, `StateMachine`) or a structural one (`PersistentId`, `Name`, `Hierarchy`, `PreservedComponents`) is rejected. If two files
declare the same name, **both** are rejected, each error naming the other file, so the result never depends on scan
order.

**Scene files**: same shape as built-ins, after them, sorted by type name:
`"Health": { "current": 100.0, "lives": 3, ..., "tint": [1.0, 0.0, 0.0], "faction": "neutral", "drops": ["gold"] }`
(vec3/quat as arrays, enums by name, arrays as arrays, floats rounded to 32-bit). Output is deterministic; re-saving
gives identical bytes. Scene files stay version 4.

**Inspector**: one collapsible section per Lua component with generated widgets, removable with its **x**: drag fields
clamped to `min`/`max`, a checkbox, a text box, a color picker for `color` vec3s, Euler degrees for quats (as for
`Transform`), a combo for enums. Arrays show `name [count]` with one row per element (value, move up, move down,
remove **x**) and **Add Element** (appends the zero value moved into `min`/`max`). Edits are checked like `kast.set`.
Add Component lists Lua types the entity doesn't have; adding one gives the declared defaults.

**Lua systems**: `kast.query/get/set/has/add/remove` take Lua component names like built-in ones. `kast.set` stays
partial and all-or-nothing: an unknown field, a wrong type, an enum name not in `values`, a value outside `min`/`max`
(including one array element) or a non-sequence table for an array raises at the calling line and leaves the
component unchanged.

**Schema drift** (lenient, unlike built-ins): when stored data doesn't match the current definition, on scene load and
on definition hot reload, missing fields get their default, unknown fields are dropped, and mistyped or out-of-range
values (including a bad array element or an unknown enum name; a bad element resets the whole array) are reset to the
field default. Each change is a Console warning naming the entity, component and field, e.g.
`Entity 'Hero' (id 3): Health: "lives" 12 must be <= 9; reset to default 3`. The scene still loads. The document is
marked dirty only if some instance's data was changed this way.

**Missing definitions**: a component name that is neither built-in nor a valid Lua definition (never defined, file
deleted, or now broken) doesn't stop the scene from loading: a Console warning is logged, and its JSON is kept verbatim
on the entity (`PreservedComponents`), written back unchanged on Save (after the known components) and carried through
Duplicate and Play/Stop. The Inspector shows it read-only as `Missing: <name>` with its raw JSON, removable with **x**.
Lua systems can't reach it (`kast.*` raises `unknown component "<name>"`). When a valid definition for that name
appears (hot reload, or opening the scene in a project that has it), the preserved data is converted with the drift
rules.

**Hot reload**: while stopped, the editor checks the `components/` folder every 0.5 s for added, removed and modified
`.lua` files (modification time) and reloads the definitions: existing instances migrate with the drift rules, and a
removed or now-broken definition turns its instances into preserved Missing data (nothing is lost; restoring the file
converts them back). During Play definitions are frozen; changes made meanwhile apply after Stop, to the restored scene.

### State machines

Game logic can be organised as finite state machines defined in Lua (`engine/state_machines.h`): named states, at most
one current, with rules on which state each may be entered from and optional `enter`/`exit` callbacks. There are two
kinds, built from the same definition files:

- **Game state machines**: project-wide, one instance per Play session, listed by name in `project.kast`
  `"stateMachines"` (the State Machines panel's Global checkbox). They keep their state across `kast.loadScene`.
- **Entity state machines**: one instance per entity with the built-in `StateMachine` component (`machine`,
  `initialState`).

**Files**: every `.lua` file under `<project>/fsms/`, scanned recursively, is one definition. Each returns exactly one
table:

```lua
-- fsms/door.lua
return {
    name = "door",              -- required, unique: kast.fsm.* game target and StateMachine "machine"
    initial = "closed",         -- required: one of the states
    states = {                  -- required, non-empty: name -> state
        closed = { allowedFrom = {"open"}, enter = function(ctx) end, exit = function(ctx) end },
        open   = { allowedFrom = {"closed", "locked"} },
        locked = { },                      -- no allowedFrom: enterable from any state
        broken = { allowedFrom = {"*"} },  -- "*": from any state
    },
}
```

- `name` is a non-empty string; state names are non-empty strings other than `"*"`. `allowedFrom` is a list of state
  names of this machine, or `"*"`. `enter` and `exit` are optional functions. There is no per-state `update`: drive
  machines from Lua systems.
- A file that isn't valid Lua, raises, doesn't return exactly one table, has a missing or non-string `name`, an
  `initial` that isn't one of its states, an `allowedFrom` entry naming an unknown state, a wrong type or an unknown
  key is rejected. The message starts with the project-relative path (and the line for Lua errors), is logged as
  `State machine rejected: ...` and shown in the State Machines panel. If two files declare the same `name`, **both**
  are rejected, each naming the other.
- Callbacks run sandboxed like [Lua systems](#lua-systems): each definition file has its own environment with the full
  read-only `kast` API. `kast.log` and events published from a callback are prefixed / attributed with the definition's
  path (`fsms/door.lua`).

**Callbacks** get one argument, `ctx`:

| Field | Value |
|-------|-------|
| `ctx.machine` | The definition's `name`. |
| `ctx.entity` | The entity id for an entity machine; `nil` for a game machine. |
| `ctx.from` | The state being left; `nil` when the machine enters its first state (Play start, runtime add, hot reload reset). |
| `ctx.to` | The state being entered. |

`exit` and `enter` of one transition get the same `ctx`.

**Transition rules** (`kast.fsm.change` / `StateMachines::change`):
- A state may be entered from the current one if its `allowedFrom` is omitted, contains `"*"`, or lists the current
  state. That includes a **self-transition** (target == current): it is allowed only by the same rule (omitted or
  `"*"` count as listing itself), and then runs `exit` and `enter` and publishes an event like any other.
- Denied (not allowed, or an unknown target state): returns `false` with a Console warning
  (`State machine "door" (entity 3): "open" can't be entered from "open" (allowedFrom); staying in "open"`). Nothing
  else happens: same state, no callbacks, no event.
- Allowed: applied **immediately**, in this order: `exit` of the old state, the switch, `enter` of the new state, then
  `fsm.changed` is published on the [event bus](#events) with `{machine, entity, from, to}` (`entity` is `null` for a
  game machine; the publisher is the definition's path). Returns `true`.
- A change requested from inside `enter` or `exit` (of any machine) is **queued** and returns `true`; queued changes
  are applied after the current transition finishes, in request order (no recursion). One that turns out denied when
  applied logs the usual warning.
- An error in `enter` or `exit` is logged with file:line and a traceback (`State machine fsms/door.lua: enter "open"
  failed: fsms/door.lua:5: ...`). The transition still commits and returns `true`; the machine stays usable.

**Lifecycle**:
- Play start: definitions load into the session's Lua state, then every scene system's `init` runs, then each listed
  game machine enters its `initial` state, then every `StateMachine` entity (depth-first) enters its `initialState`.
  An empty or unknown `initialState` falls back to the definition's `initial`, with a warning. Entering a first state
  calls `enter` with `from = nil` and publishes no event. A machine touched by `kast.fsm.*` before that (from an
  `init`) enters right then, once.
- A listed game machine without a valid definition is logged as an error (once per Play) and unavailable:
  `kast.fsm.*` on it raises.
- An entity that gains `StateMachine` during Play (`kast.add` then `kast.set` of `machine`, an Inspector edit, a
  Duplicate) enters its initial state: at once from `kast.add`/`kast.set`, otherwise at the start of the next frame.
  Changing `machine` enters the new machine's initial state (no `exit` of the old one); changing only `initialState`
  does nothing until the next Play. `kast.add` alone leaves `machine` empty: no machine yet, and `kast.fsm.*` on it
  raises. A `machine` with no valid definition is logged once. A destroyed entity's machine, or one whose
  `StateMachine` is removed, just goes away: no `exit`.
- `kast.loadScene`: game machines keep their current state name, with no callbacks; their definitions (and callbacks)
  reload into the new scene's Lua state. The new scene's entity machines enter after its systems' `init`s.
- Stop resets everything; the next Play starts every machine from its initial state again. The current state is never
  saved: Stop restores the scene (and its `StateMachine` data) as it was.

**Lua API** (`kast.fsm`, read-only). `target` is a game machine's name (string) or an entity id (number):

| Function | Returns | Notes |
|----------|---------|-------|
| `kast.fsm.state(target)` | string | The current state. |
| `kast.fsm.change(target, state)` | boolean | The transition rules above. |
| `kast.fsm.canChange(target, state)` | boolean | The same check against the current state, with no side effects and no log. |
| `kast.fsm.states(target)` | table | The machine's state names, sorted. |

A bad argument raises a Lua error at the calling line: an unknown or unavailable game machine, an invalid entity id,
an entity without `StateMachine` (or with no valid `machine`), a target that is neither a string nor a number, a
non-string `state`. An unknown **state name** passed to `change` is a denial (false and a warning), not an error.

```lua
local door
return {
    init = function()
        for e in kast.query("StateMachine") do door = e end
    end,
    update = function(dt)
        if kast.input.keyDown("E") and kast.fsm.canChange(door, "open") then
            kast.fsm.change(door, "open")
        end
        if kast.fsm.state("game_flow") == "paused" then return end
    end,
}
```

**C++ API** (`kast::StateMachines`, from `LuaSystems::fsm()`; no Lua or ImGui dependency), same rules, events and
logging; a bad target throws `std::invalid_argument` (which `kast.fsm` turns into the Lua error):

```cpp
kast::StateMachines& fsm = lua.fsm();
std::string state = fsm.state("game_flow");           // a game machine by name
bool opened = fsm.change(kast::EntityId{7}, "open");  // an entity machine by persistent id
bool can = fsm.canChange("game_flow", "paused");
std::vector<std::string> names = fsm.states(kast::EntityId{7});
std::optional<std::string> shown = fsm.peek("game_flow"); // editor display: never enters or throws
```

**Hot reload**: while playing, the `fsms/` folder is checked every frame; on any added, removed or modified file
every definition reloads (each in a fresh environment). A running machine keeps its current state if the new
definition still has it; otherwise it resets to its initial state with a warning and `enter` runs with `from = nil`.
New callbacks are used from then on. A definition that becomes invalid (or whose file is deleted) logs the error, and
its machines keep their last valid definition until it is fixed.

**Editor**: the Inspector shows `StateMachine` as a **Machine** combo (valid definitions; a name with no valid
definition is shown as `(missing) <name>` with a red "No valid definition" note, and kept as-is until another machine
is chosen) and an **Initial** combo (`(initial: <state>)` for empty, then the machine's states; an unknown one shown as
`(missing) <state>`), plus the live **Current** state while playing. See the State Machines panel under
[Editor controls](#editor-controls).

**Built games**: `fsms/` is packed into `game.pak` like every project file, and `kast_player` runs the project's
game machines and entity machines exactly as Play does (definitions are read from the pak; hot reload doesn't apply).

**Sample**: `tests/project/fsm.scene` has two entities with `StateMachine` (`traffic_light`, from
`fsms/entities/traffic_light.lua`, whose `enter` recolors the entity's material) and the project lists the game machine
`game_flow` (`fsms/game_flow.lua`). `scripts/fsm_driver.lua` moves `game_flow` from `boot` to `playing`, then steps
each light once (red → green, green → yellow) and tries to switch both to red: the yellow one may, the green one is
denied with a warning.

The dock layout is saved to `imgui.ini` in the working directory; delete it to get the default layout back.

### Game GUI

Game GUIs are HTML/CSS-like documents rendered by [RmlUi](https://mikke89.github.io/RmlUiDoc/) (`engine/gui*.h`,
`engine/scene_gui.h`): an `.rml` file for the elements and a sibling `.rcss` file for their style. RmlUi reads them
through the engine's file access (disk in the editor, `game.pak` in the player), logs to the Console as
`RmlUi: ...`, and draws with bgfx (`engine/shaders/gui/`).

**Files**: an `.rml` links its style sheet in its `<head>`; the builder edits the elements under `<body>`:

```html
<rml>
    <head>
        <title>Main Menu</title>
        <link type="text/rcss" href="main_menu.rcss"/>
    </head>
    <body>
        <div id="menu" class="panel">
            <p id="title">Kast Quest</p>
            <button id="start_button" data-event="menu.start">Start</button>
        </div>
    </body>
</rml>
```

**Elements**: **Panel** = `<div>`, **Text** = `<p>`, **Button** = `<button>`. A Button's `data-event` names the event
its click publishes (see below). Any other element (hand-written `<span>`, `<img>`, ...) is **Unknown** to the builder:
listed and movable/deletable there, never created or edited, and kept with its attributes and children on save.
Attributes the builder doesn't edit (`style`, `title`, `data-*`, ...) and comments are kept too; saving only
normalizes whitespace and indentation.

**Style sheets** are RCSS (CSS2-like; see the RmlUi docs). RmlUi has no built-in styles: elements are inline until a
rule says otherwise, so sheets start with `div, p { display: block; }`. The only font is the bundled Roboto
(`font-family: Roboto;`, set on `body`); text without a `font-family` doesn't show (RmlUi warns). Images
(`<img>`, image decorators) are not supported yet: they log a warning and draw nothing.

**UIDocument** (built-in component, *UI Document* in the Inspector): `path` (project-relative `.rml`; the Inspector
offers a combo of the project's `.rml` files, an unknown path shows as `(missing) <path>` and is kept) and `visible`
(default true). Saved with the scene like any component. In Play (and in the built game), every `UIDocument` in the
scene loads at Play start and shows over the Game view while `visible`, stacked in entity creation order (later
entities on top); they unload on Stop and on a `kast.loadScene` switch (the new scene's load instead). Adding,
removing or editing a `UIDocument` during Play applies at once. A missing `.rml`, an `.rml` RmlUi can't parse or a
linked `.rcss` with problems logs an error naming the file and that document shows nothing; Play goes on. While the
Game panel has input (focused and playing) the GUIs get the mouse, wheel and keys too (the game's `kast.*` input
still sees them).

**Button events**: clicking a `<button>` with a non-empty `data-event` publishes that event on the Play session's
[event bus](#events), publisher `UI`, payload `{"id": "<button id or empty>", "document": "<rml path>"}` (the
UIDocument's `path`). No or empty `data-event`: nothing is published. Lua subscribes as to any event:

```lua
kast.subscribe("menu.start", function(event)
    kast.log("Menu start clicked: " .. event.id .. " in " .. event.document)
end)
```

**Hot reload** (Play): when a loaded `.rml` or a `.rcss` it links changes on disk (the editor's 0.5 s file poll), that
document reloads; fix a broken file and it shows again. Not in the built game.

**Sample**: `tests/project/ui.scene` has a *Main Menu* entity with a `UIDocument` for `ui/main_menu.rml`
(+ `main_menu.rcss`: a panel with a title, a subtitle holding an unknown `<span>`, and Start / Quit buttons publishing
`menu.start` / `menu.quit`); `scripts/ui_menu.lua` logs both events.

### GUI Builder

Four editor panels on one open GUI. **Assets > Create > GUI** writes `new_gui.rml` + `new_gui.rcss` (a Panel holding a
Text and a Button with `data-event="ui.clicked"`, the `.rml` linking the `.rcss`) into the chosen folder, as
`new_gui_2`, `new_gui_3`, ... when a name is taken (ignoring case), never overwriting, and starts renaming the
`.rml`. Renaming an `.rml` in Assets also renames the sibling `<stem>.rcss` it links and rewrites the link (refused,
nothing changed, if either new name is taken); moves and renames of the open files are followed. **Double-click** an
`.rml` in Assets to open it (one GUI at a time; with unsaved builder changes, Save / Discard / Cancel first); its
four panels come to the front.

- **GUI Hierarchy** (tab beside Hierarchy): the elements under **Body**. **+ Panel / + Text / + Button** add into the
  selected Panel, else into Body; **Delete**, **Up**, **Down** act on the selection; the right-click menu has the same.
  Drag an element onto a Panel or Body to move it there (into itself or its own children is refused). Unknown elements
  are greyed and labelled `<tag>`.
- **GUI Inspector** (tab beside Inspector): the selection's `id` and `class`, its text (Text and Button holding only
  text; one with child elements says so) and, for a Button, `data-event`. Other attributes are listed read-only.
  Unknown elements are read-only. Edits apply as you type.
- **GUI Style** (tab beside Console): the `.rcss` text, applied to the preview 0.3 s after the last keystroke. A sheet
  RmlUi reports problems for (in the Console) is not applied: the preview keeps the last good style and the panel
  says so in red.
- **GUI Preview** (tab beside Scene and Game): the document as RmlUi renders it at **1280x720**, **1920x1080** or the
  **Panel size**, scaled to fit. Click selects the deepest element under the cursor (Body if none); the selection has a
  yellow outline, drawn by the editor, never saved.

Selection is shared by the three. Any edit marks the GUI dirty: `*` after the file name in the builder panels'
titles. **Save** (GUI Hierarchy, GUI Style) or **Ctrl+S** while a builder panel is focused writes the `.rml` and
`.rcss` (elsewhere Ctrl+S still saves the scene). Quitting with unsaved builder changes asks Save / Discard / Cancel
first, then the scene's own prompt if it is dirty. When the open files change on disk (the 0.5 s poll), a clean
builder reloads them (keeping the selection by `id` and, if the new sheet is broken, the last good style); with
unsaved changes it asks **Reload** / **Keep mine**. A deleted `.rml` closes a clean builder.

## Tests

`ctest --preset debug` (or `release`) runs:

| Test | What it checks |
|------|----------------|
| `engine_self_check` | `kast_self_check`: rotation conventions, Euler/quaternion and matrix decomposition, picking rays and ray-box hits, reparent keeping the world transform, cycle rejection, subtree duplicate/delete, point lights (falloff 0 at and beyond `range`, positive and decreasing inside; nearest-8 selection with out-of-range lights skipped; `PointLight` validation; point light and `lighting` round trip, missing `lighting` defaults, every malformed `lighting` rejected), shadows (cascade splits near to far and lambda mix, frustum slice corners in both depth conventions, cascade bounds containing the slice and a caster toward the light, whole-texel cascade moves, the 4 nearest `castShadows` point lights with ties in list order, shadow field defaults, pre-shadow scenes loading with them, validation and round trip), component registry (add/remove/has, JSON round trip of every field type, duplicate copies all components), scene-file validation (including invalid `Camera` values and versions 1–3 rejected), active camera (first depth-first) and camera view/projection matrices, `kast::Input`, `rotatorSystem`, in-memory snapshot restore, editor Play/Pause/Stop on a `Document` (dirty flag, selection, byte-identical restore, Game/Scene focus requests, no-camera warning, default Main Camera), and Lua systems: scene `"systems"` round trip and validation, order after the C++ systems, init/update/shutdown with Play/Pause/Stop, the whole `kast.*` API and its errors, sandbox and per-system globals, syntax/runtime errors disabling only that system, hot reload, Systems panel edits; projects: `project.kast` load/save/validation, walk-up discovery, project-relative paths and escape rejection, opening a project and Save/Save As on a `Document` (New Project folder-name sanitizing, every validation rule, creating `<dir>/my-game/project.kast` with the trimmed name, never creating missing parents or writing into an existing folder) (no rebasing, project-change warning, title format), the Project Manager's recent list (save/load round trip, newest first, case-insensitive dedupe with trailing separators, Remove leaving the project on disk, missing folder / missing or invalid `project.kast` reported, three kinds of corrupt file backed up to `projects.json.bad` with a warning and read as empty) and an editor that can't be started reporting why, no-project Lua disabling, Add Script outside the project, template location; `kast.loadScene` (end-of-frame, last call wins, ignored in `shutdown`, init calls deferred a frame, shutdown/init order, every error case keeps the current scene) and `kast.currentScene`, and Stop restoring the pre-Play scene after two switches; events: the C++ bus (publish then subscription order, next-drain re-publish, subscribe/unsubscribe during a drain, a throwing subscriber, RAII and stale handles, dropQueued, reset, the 500-event history and Clear), Lua→Lua in the same frame after every update with independent payload copies, Lua→C++ and C++→Lua payload conversion, one subscription order across C++ and Lua, every `kast.publish/subscribe/unsubscribe` rejection raising at file:line and queueing nothing (bad names, functions, userdata, threads, cycles, nesting past 32, mixed keys, NaN), a failing subscriber disabling only its system, cleanup on disable/hot reload/Stop/Play start, a scene switch dropping queued events and old Lua subscribers but keeping C++ ones, and Pause holding / Stop discarding the queue in editor Play; Lua components: definition parsing (every field type and option, recursive discovery, every invalid-definition case naming the file), name clashes (built-in, structural, both duplicate files rejected), sandbox and per-file globals, registry parity (add/has/remove, strict JSON set, Duplicate, save order, byte-identical save/load, snapshot and Document Play/Stop restore), `kast.*` on a Lua component with every `kast.set` error leaving it unchanged, drift rules (one warning per fix, dirty only when data changed, built-ins still strict), missing definitions (load with warnings, verbatim save, `kast.*` raises, later conversion), hot reload of modified/added/deleted/broken files without data loss and the Play freeze, Components panel template naming and enable rules; game.pak: write/read round trip (byte-identical, binary and empty files), directory listing, rejection of bad magic, bad version, truncated header/index/data, an offset past the end and an overflowing size; a mounted pak serving project.kast, readFile and listFiles (and not the disk); Build: exe-name sanitizing, pak contents (no dotfiles, dot-directories or output folder), every validation failure writing nothing, a full build (exe, game.pak, DLLs, other files kept), the remembered folder in `.kast/editor.json` (project.kast unchanged) and the Build menu enable rule (no project, Play, Pause); Online Services: `isValidCaPem`, no `online` block loading disabled and saving without it, every rejected Save (no URL while enabled, non-http(s) or bad URL, missing / invalid / outside / absolute / dot-folder CA file) leaving `project.kast` byte-identical, the round trip (exact bytes, version 1), disabled keeping values with no CA file required, every malformed block rejected with `out` untouched, a deleted CA file loading but failing `loadOnlineCa`, Build failing on it and packing the block and CA file when valid, a disabled project packing nothing extra, the modal blocking file actions, and the online config and CA file loading from a mounted pak; Build Settings: Start Scene choices (`tests/project` lists `audio.scene`, `events.scene`, `fsm.scene`, `health.scene`, `missing_component.scene`, `net_a.scene`, `net_b.scene`, `point_lights.scene`, `scene_a.scene`, `scene_b.scene`, `ui.scene`; a dotfile or dot-directory scene is left out), Save writing only `startScene` (same keys and order, reloads, `doc.project` updated, the open scene's dirty flag and path kept, no `.kast/editor.json`), a scene that doesn't load or escapes the project leaving `project.kast` byte-identical, opening (then Cancel) writing nothing, and Build / Build and Run with `startScene` null or missing opening Build Settings; state machines: recursive discovery, every invalid-definition case naming the file (bad or missing `name`, bad `initial`, unknown `allowedFrom` state, wrong types, unknown keys, syntax and sandbox errors, both duplicate files), `allowedFrom` / omitted / `"*"` / self-transition rules, editor rescans, entry order at Play start (after the inits, game then entity machines, no event, `initialState` fallback warning, first touch during `init`), allowed and denied transitions (return value, warning, no callbacks, no event), `exit` → switch → `enter` → `fsm.changed {machine, entity, from, to}` with the definition as publisher, queued changes from callbacks (no recursion, denial on apply), callback errors (file:line and traceback, commit, still usable), C++ misuse throwing, every `kast.fsm.*` misuse raising at the calling line, read-only `kast.fsm`, identical results from Lua and C++, game state kept across `kast.loadScene` with no callbacks and new callbacks after it, Stop then Play starting over, runtime add (`kast.add` + `kast.set`, registry edit, Duplicate), destroy/remove without `exit`, a missing definition logged once, hot reload (state kept, reset with warning when removed, last valid definition kept when broken, game machines too), `StateMachine` byte-identical save/load, Duplicate and Stop restore, `project.kast` `stateMachines` round trip and validation, New State Machine name rules / template / no overwrite, Global toggling `project.kast`, and both blocked during Play; game GUI: the builder model (malformed RML and a missing `<body>` refused, parse → serialize → parse unchanged keeping unknown elements, their children, unedited attributes, comments, entities and `<head>`, Panel/Text/Button/Unknown kinds, editable-text rule, preview numbering and inline sheet), add / delete / move up and down / reparent (own subtree and `<body>` refused, Unknown elements move with their children), `data-event` set and cleared, an edited document reopening identically, Create GUI naming (`new_gui`, `_2`, skipping a stem whose `.rcss` is taken, never overwriting, each `.rml` linking its `.rcss`), GUI rename (linked `.rcss` follows, refusal leaves the disk unchanged), `UIDocument` registration, defaults and scene JSON round trip, and the button click adapter (payload `{id, document}`, publisher `UI`, empty event publishes nothing); models: the GLB character (2 skinned meshes, 66 joints, 178 clips, 2 embedded materials with the file's factors, upright and human sized, weights summing to 1), the OBJ cube with its MTL color and the same bounds as the glTF cube, the Kenney FBX (skinned, centimeters to meters, Y-up), missing / corrupt / garbage files as errors, 128 vs 130 generated joints against the bone limit, key sampling (lerp / slerp at mid-key, loop wrap, clamp), crossfade weights 0 to 1 and a crossfaded pose, skin matrices, the Animator system (playOnStart, no ModelRenderer, unknown clip, Transform untouched), `ModelRenderer` / `Animator` JSON round trip and rejection, model drops / Create > Model / rename and move remaps on a `Document`, and the Edit-mode preview never dirtying or saving; physics: `Collider` / `Rigidbody` / `CharacterController` validation and scene round trip, gravity default / round trip / rejection, a box resting on the floor, a body under a moved and rotated two-level parent (world and local result), scaled box and sphere rest heights, nothing in Edit mode, Play / Pause / Stop restore, the same result for 1/30 and 2 x 1/60 frames, the step cap, trigger enter / exit once without blocking, one collisionEnter (point, normal from a to b) and the exit on destroy, a bouncing ball, set / get velocity, impulse, force, angular velocity, kinematic velocity, wrong-body errors, runtime add / remove / field change / destroy and Transform teleports (static, dynamic, kinematic), the Rigidbody-only warning and the CharacterController conflict reported once, characters (grounded, ground entity and normal, jump, walk, step climbed, taller block, slopes under and over the limit, static wall and kinematic body block, a dynamic crate pushed, contact events), `kast.physics` from Lua (raycast, overlaps with rotation, body control, every error, `kast.add` / `kast.set` / `kast.remove` / `kast.destroy` sync) and the collider / capsule wireframes; audio (offline rendering, no device): the WAV and MP3 fixtures and a generated WAV decode with the expected frames, channels and sample rate (corrupt WAV / MP3, `.ogg`, missing and outside paths rejected), a playing clip is non-silent and silent with its bus or Master muted, rendered peak = amplitude x volume x bus x Master (within 0.005), Music streams through the same buses, a loop (cached and streamed) sounds past 2.5x its length with no silent chunk, a one-shot goes silent after its length and frees its voice, 33 one-shots steal the oldest (stale handle, count 32), loops never stolen and a 33rd play with 32 loops dropped with one warning, pause / resume / setPaused holding the playback position (within 64 frames), stale handles as no-ops, `play` errors taking no voice, the null backend's one warning and normal behavior, `AudioSource` validation and save/load round trip, `project.kast` `audio` defaults / partial block / round trip / rejection and the Mixer's `setBusDefault`, Play applying the defaults, and an editor Play session (`playOnStart`, a corrupt clip's one error, every `kast.audio` call and error from Lua with the system running on, Pause silence and resume, zero voices after `kast.loadScene` and Stop); a mounted pak decoding and playing a WAV and an MP3; networking (`tests/net_self_check.cpp`, against in-process loopback servers on ephemeral ports with self-signed certificates made at run time): base64, UTF-8 and URL parsing, TCP `raw` / `u16le` / `line` framing with coalesced and split reads (inside the length prefix too) and send framing, `text` / `base64` / `json` receive and send (invalid UTF-8 and bad JSON as `net.error` with the connection kept), TLS trusted by `caFile` with SNI, an untrusted self-signed server and a host-name mismatch failing, ws and wss (binary and text frames, script close), HTTP and HTTPS (method, headers, body, status, JSON body), refused, DNS failure, connect and request timeouts, peer close, a line, a WebSocket message and a body over 16 MiB, every bad send / open / request argument, duplicate names, `stop()` closing every socket (the servers see it) and cancelling requests promptly, every `kast.net` argument error and json / base64 sends from Lua, and a `kast.loadScene` mid-stream (500 ticks, none lost or reordered, the ws echo and a request made before the switch reaching the new scene, one accept per server, bus delivery on the main thread), then Stop closing both connections and Play starting from none. `kast_self_check --net` runs only these. No window. |
| `editor_smoke` | `kast_editor --frames 120 --screenshot build/<preset>/smoke.png` with no project or scene (the Project Manager, empty list): exit 0 and a non-empty PNG. Every screenshot test passes `--projects-file <its run folder>/projects.json`. |
| `editor_smoke_scaled` | Same, with `--ui-scale 2.0`. |
| `scene_screenshot` | Same, with `--scene tests/scenes/basic.scene`. |
| `scene_roundtrip` | `basic.scene` → `--save-scene roundtrip_out.scene` → load that → `roundtrip_out2.scene`; the two saves must be byte-identical. |
| `scene_play_screenshot` | Same as `scene_screenshot` plus `--play`, writing `scene_play.png`. |
| `scene_play_differs` | `scene_play.png` must differ from `scene_basic.png` (runs after both). |
| `game_play_screenshot` | Same as `scene_play_screenshot` (Play brings the Game tab to the front), writing `game_play.png`. |
| `game_play_differs` | `game_play.png` must differ from `scene_basic.png`. |
| `game_no_camera` | `--scene tests/scenes/no_camera.scene --play`: exit 0 and a PNG (Game panel shows "No Camera in scene"). |
| `lua_scene_screenshot` | `--scene tests/scenes/lua.scene` (Systems panel lists its two scripts), writing `lua_scene.png`. |
| `lua_play_screenshot`, `lua_play_no_systems_screenshot` | `--play` on `lua.scene` and on a copy with `"systems": []` generated at configure time, writing `lua_play.png` / `lua_play_no_systems.png`. |
| `lua_play_differs` | `lua_play.png` must differ from `lua_play_no_systems.png`: only the Lua systems (lift the cube, tint the sphere) can make them differ. |
| `scene_bad_json`, `scene_missing`, `scene_v1_rejected`, `scene_v3_rejected` | `--scene` with `tests/scenes/malformed.scene` / a missing file / the version 1 `tests/scenes/v1.scene` / the version 3 `tests/scenes/v3.scene` must exit 1. |
| `materials_screenshot`, `materials_play_screenshot`, `materials_default_play_screenshot`, `materials_differs` | `tests/project/materials.scene` in the Game view (`materials.png`, log must name the broken `.mat`), `--play` on it (log must name the missing texture) and on a configure-time copy with every material path emptied; the two Play shots must differ. |
| `materials_ambient_only_screenshot` | A project copy with the sun at intensity 0, writing `materials_ambient_only.png`: metals lit by ambient only (not black). |
| `material_inspector_screenshot`, `mesh_renderer_material_screenshot` | `materials.scene --select 3` with `--assets-select materials/cutout.mat` (the Inspector edits the `.mat`) and without (the MeshRenderer material combo), writing `material_inspector.png` / `mesh_renderer_material.png`. |
| `assets_create_material_screenshot`, `assets_create_material_written` | `--assets-create material:materials` on a fresh copy of `tests/project`: `materials/new_material.mat` exists and holds the default material. |
| `materials_roundtrip` | `materials.scene` saved twice: byte-identical. |
| `build_game_materials`, `player_materials_screenshot` | The build and run of a copy with startScene `materials.scene`: materials and textures from `game.pak` in the standalone player, `player_materials.png`. |
| `point_lights_screenshot`, `lighting_panel_screenshot`, `point_lights_play_screenshot`, `point_lights_off_play_screenshot`, `point_lights_play_differs` | `tests/project/point_lights.scene` (three colored point lights, bluish ambient): editor with `--select 7` (Blue Light's range sphere) in `point_lights.png`, `--focus-panel Lighting` (opens the hidden Lighting panel) in `lighting_panel.png`, and `--play` on it and on a configure-time copy with every `PointLight` intensity 0; the two Play shots must differ. |
| `point_lights_roundtrip`, `point_lights_bad_range_rejected`, `point_lights_bad_lighting_rejected` | Save twice gives identical bytes; configure-time copies with `"range": 0` / `"ambientIntensity": -1` must exit 1. |
| `post_processing_screenshot`, `post_processing_toggled_off_screenshot`, `post_processing_toggle_differs` | `tests/project/post_processing.scene` (every effect on; emissive sphere, bright point lights, near and far glass, pillars out to 55 m, a game GUI) in the Scene view with the **Post** box on and off (`--no-scene-post`); the two must differ. |
| `shadows_screenshot`, `shadows_sun_off_screenshot`, `shadows_sun_off_differs`, `point_shadows_screenshot`, `point_shadows_off_screenshot`, `point_shadows_off_differs`, `shadows_inspector_screenshot` | `tests/project/shadows.scene` (sun over a cube, sphere, posed skinned character, alpha-mask cutout, alpha-blend glass and a pillar ~49 m out) and `point_shadows.scene` (5 `castShadows` point lights with pillars) in the Scene view, against configure-time copies with the sun's / every point light's `castShadows` off: each pair must differ. `--select 1` shows the Sun's shadow fields in the Inspector. |
| `*_play_screenshot`, `*_play_differs`, `*_play_same` (shadows) | Game view (`--play`, the Assets tab covering the Console) of one-change copies of those scenes against the base: sphere / cutout / character / far pillar `castShadows` off, ground `receiveShadows` off, `maxDistance` 20 and the nearest point light's `castShadows` off must **differ**; glass (alpha blend) `castShadows` off, far pillar `castShadows` off with `maxDistance` 20 (beyond it), and the farthest (5th) point light's `castShadows` off must be **identical**. |
| `shadows_roundtrip`, `point_shadows_roundtrip`, `build_game_shadows`, `build_game_shadows_off`, `build_game_point_shadows`, `player_shadows_screenshot`, `player_shadows_off_screenshot`, `player_point_shadows_screenshot`, `player_shadows_differs` | Save twice gives identical bytes; the built games show them in the player, with and without the sun's shadow (must differ). |
| `post_processing_panel_screenshot` | `--focus-panel "Post Processing"` opens the hidden panel, `post_processing_panel.png`. |
| `models_screenshot` | `tests/project/models.scene` in the Scene view: OBJ, glTF, GLB and FBX models next to primitives, `models_screenshot.png`. |
| `models_anim_play_60`, `models_anim_play_120`, `models_anim_advances` | `--play` on `models_anim.scene` after 60 and 120 frames (`FRAMES`); the shots must differ (the pose advances). |
| `models_no_animator_play_120`, `models_anim_vs_bind_pose` | `--play` on a configure-time copy without its `Animator` (in `build/<preset>/model_project`); must differ from `models_anim_play_120.png`. |
| `models_material_override`, `models_material_embedded`, `models_material_override_differs` | `models_material.scene` (slot 0 overridden with gold) and a configure-time copy with `"materials": []`; must differ. |
| `models_missing` | `models_missing.scene`: a magenta cube, and the missing-model warning logged exactly once (`EXPECT_COUNT=1`). |
| `models_lua` | `--play` on `models_lua.scene`: the log shows `kast.getAnimationClips`, `kast.playAnimation` with fade, `kast.getAnimationState` and the Lua errors for an unknown clip, a bad entity and an entity without Animator. |
| `physics_edit_screenshot`, `physics_play_screenshot`, `physics_edit_vs_play_differs` | `tests/project/physics.scene` in the Scene view and in Play after 240 frames (objects fell); the log must show `scripts/physics_log.lua`'s raycast, the cube's triggerEnter / triggerExit, its collisionEnter on the ground and the sphere's collisionExit (`EXPECT_OUTPUT`). |
| `physics_play_no_gravity_screenshot`, `physics_play_no_gravity_vs_play_differs` | Play of a configure-time copy with zero gravity (in `build/<preset>/physics_project`): must differ from the Play shot. |
| `physics_collider_screenshot`, `physics_collider_none_screenshot`, `physics_collider_none_vs_collider_differs`, `physics_character_screenshot` | `--select 6` (Trigger Zone, no mesh) against the same selection in a copy without its `Collider`: only the green wireframe differs; `--select 7` shows the character capsule. |
| `physics_panel_screenshot`, `physics_roundtrip`, `physics_bad_gravity_rejected` | `--focus-panel Physics`; save twice gives identical bytes; a copy with a non-number gravity must exit 1. |
| `build_game_physics`, `player_physics_screenshot`, `player_physics_events` | The built game runs `physics.scene` from `game.pak` in the player; its log shows the same events and raycast. |
| `audio_inspector_screenshot`, `audio_mixer_play_screenshot`, `audio_mixer_edit_screenshot`, `audio_assets_screenshot`, `audio_play_log` | `tests/project/audio.scene` with `--no-audio-device`: `--select 1` (the Inspector's `AudioSource`: clip picker, Preview), `--play --focus-panel Mixer` (voices listed; exactly one null-backend warning), the Mixer in Edit mode, `--assets-select sounds/wave-demo.wav --focus-panel Assets` (speaker icons), and Play logging `scripts/audio_sfx.lua`'s one-shot as playing. |
| `net_panel_idle_screenshot` | `--project tests/project --focus-panel Network` outside Play: the panel's empty-state line, `net_panel_idle.png`. |
| `net_play_scene_switch` | `kast_net_test_server` (fixed ports 47701-47706) runs the editor on `tests/project/net_a.scene --play` with `--focus-panel Network`: `scripts/net_b.lua` must log its one `PASS` line (4 connections kept across `kast.loadScene`, 100/100 ticks, 2/2 responses); `net_play.png` shows the Network panel. |
| `build_game_net`, `player_net_scene_switch` | The built `net` game (startScene `net_a.scene`) does the same from `game.pak` in the player with PATH limited to System32, so the OpenSSL DLLs must have been shipped. Both net tests hold the `net_test_ports` resource lock. |
| `build_game_audio`, `player_audio_from_pak`, `player_audio_no_focus` | The built audio game (with `--no-audio-device`) decodes and plays its clips from `game.pak` (the log shows the MP3 one-shot playing), and doesn't take focus. |
| `models_inspector_preview` | `models_anim.scene --select 3`: the Model Renderer pickers and the Animator clip dropdown and Preview row. |
| `build_game_models`, `player_models_screenshot` | The build and run of a copy with startScene `models_anim.scene`: the animated character from `game.pak` in the player. |
| `post_processing_play_screenshot`, `post_processing_off_play_screenshot`, `post_processing_differs` | `--play` on it (game GUI over the post-processed Game view) and on a configure-time project copy without the `"postProcessing"` block; the two must differ. |
| `post_processing_roundtrip`, `post_processing_bad_rejected` | Save twice gives identical bytes; a copy with `"operator": "Reinhard"` must exit 1. |
| `build_game_post`, `player_post_screenshot` | The built game with startScene `post_processing.scene` (`Post Test.exe`): post effects and fog in the player, GUI on top, `player_post.png`. |
| `build_game_lights`, `player_lights_screenshot` | The build and run of a copy with startScene `point_lights.scene` (`Lights Test.exe`): point lights and ambient in the standalone player, `player_lights.png`. |
| `project_startup` | `--project tests/project`: exit 0 and a PNG (opens its `startScene`, `scene_a.scene`). |
| `project_missing` | `--project tests/scenes/scripts` (no `project.kast`) must exit 1. |
| `launcher_screenshot` | `--launcher` with a list holding `tests/project` and a missing folder, writing `launcher.png` (one normal row, one greyed with "Folder not found"). |
| `launcher_corrupt_list` | No arguments with an invalid `projects.json`: exit 0, `projects.json.bad` written, `launcher_corrupt.png` shows an empty list. |
| `launcher_open` | `--launcher-open tests/project -- --frames 120 --screenshot launcher_open.png`: exit 0 (the started editor's, after waiting for it), the editor wrote `launcher_open.png`, and `tests/project` is first in the list. |
| `editor_project_records_recent` | `--project tests/project`: the list file then holds `tests/project`. |
| `new_project_valid_screenshot` | Project Manager `--new-project "My Game" build/<preset>/new_project_parent` (an existing folder), writing `new_project_valid.png` (preview `...\my-game`, Create enabled). Fails if `my-game` was created. |
| `new_project_invalid_screenshot` | Same with a missing directory, writing `new_project_invalid.png` (inline "does not exist", Create disabled). Fails if the missing directory was created. |
| `switch_play_screenshot`, `switch_play_no_systems_screenshot` | `--play` on `tests/project/scene_a.scene` (its script calls `kast.loadScene("scene_b.scene")` on the first frame) and on a copy with `"systems": []` generated at configure time, writing `switch_play.png` / `switch_play_no_systems.png`. |
| `lua_components_screenshot` | `--scene tests/project/health.scene --select 3 --size 1280x1000`: the Components panel lists `Health` as OK and the Inspector shows Hero's `Health` with every widget kind, writing `lua_components.png`. |
| `lua_components_play_screenshot`, `lua_components_play_no_systems_screenshot` | `--play` on `health.scene` and on a copy with `"systems": []` (in `build/<preset>/health_project/`, a configure-time copy of `tests/project`), writing `lua_components_play.png` / `lua_components_play_no_systems.png`. |
| `lua_components_play_differs` | The two must differ: only `scripts/health_drain.lua` (drains `Health`, fades the entity's material `baseColor` from `Health.tint` with `kast.setMaterialProperty`) can change them. |
| `events_play_screenshot`, `events_play_no_systems_screenshot` | `--play --focus-panel Events` on `tests/project/events.scene` and on a copy with `"systems": []` (in `build/<preset>/health_project/`), writing `events_play.png` (Events panel listing the sample's events) / `events_play_no_systems.png`. |
| `events_play_differs` | The two must differ: the publisher changes nothing itself, so only the delivered `health.empty` events (greying Hero and Slime) can change them. |
| `fsm_play_screenshot`, `fsm_play_no_systems_screenshot` | `--play --focus-panel Events` on `tests/project/fsm.scene` and on a copy with `"systems": []` (in `build/<preset>/health_project/`), writing `fsm_play.png` (Events panel listing `fsm.changed`) / `fsm_play_no_systems.png`. |
| `fsm_play_differs` | The two must differ: both copies enter their initial states (red cube, green sphere), so only the transitions driven by `scripts/fsm_driver.lua` (green cube, red sphere) can change them. |
| `state_machines_panel_screenshot` | `fsm.scene --select 3 --size 1280x1000 --play --focus-panel "State Machines"`, writing `state_machines_panel.png`: both definitions OK, `game_flow` Global with `State: playing`, Global and New State Machine disabled; the Inspector shows Light A's `StateMachine` with `Current` green. |
| `state_machine_inspector_screenshot` | The same, stopped, writing `state_machine_inspector.png`: the Machine / Initial combos, Global and New State Machine enabled. |
| `new_state_machine_valid_screenshot`, `new_state_machine_collide_screenshot` | `fsm.scene --select 3` with `--new-state-machine-prompt TrafficSignal` (preview `fsms/traffic_signal.lua`, Create enabled; fails if the file was written) and `--new-state-machine-prompt traffic_light` (inline "already exists", Create disabled), writing `new_state_machine_valid.png` / `new_state_machine_collide.png`. |
| `lua_components_roundtrip` | `health.scene` saved twice into `build/<preset>/health_project/` (so `Health` stays defined): byte-identical. |
| `new_component_valid_screenshot`, `new_component_collide_screenshot` | `health.scene --select 3` with `--new-component-prompt PlayerHealth` (preview `components/player_health.lua`, Create enabled; fails if the file was written) and `--new-component-prompt Health` (inline "already exists", Create disabled), writing `new_component_valid.png` / `new_component_collide.png`. |
| `add_component_popup_screenshot`, `add_component_popup_play_screenshot` | `health.scene --select 3 --add-component-popup`, stopped and with `--play`, writing `add_component_popup.png` ("New Lua Component..." first, enabled) / `add_component_popup_play.png` (disabled). |
| `new_component_screenshot`, `new_component_file_written`, `new_component_differs` | `--new-component PlayerHealth` on Hero in `build/<preset>/new_component_project/` (a fresh copy of `tests/project` made by `new_component_project_clean` / `_copy`), writing `new_component.png`; the file must contain `name = "PlayerHealth"` and the shot must differ from `lua_components.png`. |
| `switch_play_differs` | `switch_play.png` (scene B: three green spheres) must differ from `switch_play_no_systems.png` (scene A: a red cube). |
| `build_game_switch`, `build_game_switch_no_systems`, `build_game_health`, `build_game_health_no_systems`, `build_game_events`, `build_game_events_no_systems` | `--project <copy> --build build/<preset>/game_out/<name>` on configure-time copies of `tests/project` in `build/<preset>/game_projects/` (startScene `scene_a.scene` / `health.scene` / `events.scene`, each also with its `"systems"` emptied; each copy has a `.kast/editor.json`): exit 0; the folder holds `Switch Test.exe` / `Health Test.exe` / `Events Test.exe`, `game.pak` and the DLLs; `game.pak` lists `project.kast` but nothing under `.kast/`; `project.kast` is unchanged. |
| `player_switch_screenshot`, `player_switch_no_systems_screenshot`, `player_health_screenshot`, `player_health_no_systems_screenshot`, `player_events_screenshot`, `player_events_no_systems_screenshot` | The built game, run from a copy of its folder with `--frames 120 --screenshot`: exit 0, a PNG (`player_<name>.png`) and the log line `Window title: <project name>`. |
| `player_switch_differs` | `player_switch.png` (scene B, reached by `kast.loadScene` from the pak) must differ from `player_switch_no_systems.png` (scene A). |
| `player_health_differs` | `player_health.png` (Lua component `Health` drained by a Lua system) must differ from `player_health_no_systems.png`. |
| `build_game_fsm`, `build_game_fsm_no_systems`, `player_fsm_screenshot`, `player_fsm_no_systems_screenshot` | The same build and run for `fsm.scene` (project.kast of the copies lists `game_flow`), writing `player_fsm.png` / `player_fsm_no_systems.png`. |
| `player_fsm_differs` | `player_fsm.png` must differ from `player_fsm_no_systems.png`: the driver (which only acts once the game machine `game_flow` is `playing`) and the entity machines run from `game.pak` in `kast_player` too. |
| `player_events_differs` | `player_events.png` (Hero and Slime greyed by a `health.empty` subscriber) must differ from `player_events_no_systems.png`: events are delivered in `kast_player` too. |
| `player_missing_pak`, `player_corrupt_pak` | The built game with its `game.pak` deleted / replaced by text, with `--frames`: exit 1 (no message box, no hang). |
| `build_bad_start_scene` | `--build` on a copy whose `startScene` is `missing.scene`: exit 1, and the (empty) output folder gets no exe and no `game.pak`. |
| `build_no_project` | `--build` without `--project`: exit 1, nothing written. |
| `build_menu_screenshot`, `build_menu_play_screenshot`, `build_menu_no_project_screenshot` | `--open-menu Build` with `--project tests/project` (Build Settings..., a separator, Build..., Build and Run: enabled), with `--play` too (disabled) and with `--scene tests/scenes/basic.scene`, outside any project (disabled), writing `build_menu*.png`. |
| `build_settings_screenshot`, `build_settings_missing_screenshot`, `build_settings_no_scenes_screenshot` | `--open-build-settings` on `tests/project` (`scene_a.scene` selected), on the `bad_start` copy (`missing.scene (missing)`, Save disabled) and on `build/<preset>/game_projects/no_scenes` (only `project.kast` and an excluded `.kast/scene_a.scene`: hint text, dropdown and Save disabled), writing `build_settings*.png`. |
| `build_no_start_scene` | `--build` on the `no_scenes` copy (`startScene: null`): exit 1, nothing written, and the error mentions `Build Settings`. |
| `online_settings_disabled_screenshot`, `online_settings_screenshot`, `online_settings_error_screenshot` | `--open-online-settings` on `tests/project` (no `online` block: unchecked, empty fields), on `build/<preset>/game_projects/online` (enabled, `https://localhost:3100`, `certs/net_test_ca.pem`) and on the `online_bad_ca` copy (CA file `certs/missing.pem`: the error in red, Save disabled). |
| `build_game_online`, `player_online_config`, `build_online_bad_ca` | `--build` of the `online` copy, whose built game logs `Online Services enabled: auth https://localhost:3100, CA file 'certs/net_test_ca.pem'` (read from `game.pak`); `--build` of `online_bad_ca` exits 1 naming the missing CA file. |
| `ui_play_screenshot`, `ui_play_hidden_screenshot`, `ui_play_differs` | `--play` on `tests/project/ui.scene` (the main menu over the Game view) and on a copy with the `UIDocument` `visible: false` (in `health_project`), writing `ui_play.png` / `ui_play_hidden.png`; they must differ. |
| `ui_bad_path_screenshot` | `--play` on a copy whose `UIDocument` names `ui/missing.rml`: exit 0, a PNG, and the log has `UI document 'ui/missing.rml' not found`. |
| `ui_click_screenshot` | `ui.scene --play --gui-click start_button --focus-panel Events`, writing `ui_click.png` (Events panel lists `menu.start` from `UI`); the log must have `Menu start clicked: start_button in ui/main_menu.rml` from `scripts/ui_menu.lua`. |
| `build_game_ui`, `player_ui_screenshot`, `player_ui_click` | The build and run of a copy with startScene `ui.scene` (`UI Test.exe`): the GUI from `game.pak` in `player_ui.png`, and with `--gui-click start_button` the Lua log line above (`player_ui_click.png`). |
| `gui_builder_screenshot` | `--project tests/project --open-gui ui/main_menu.rml --gui-select start_button --size 1600x1000`, writing `gui_builder.png`: GUI Hierarchy (with the Unknown `<span>`), GUI Inspector (id, text, data-event, `title` kept), GUI Style and GUI Preview with the outline. |
| `gui_builder_restyled_screenshot`, `gui_builder_restyle_differs` | The same in `health_project`, whose `main_menu.rcss` (configure-time copy) has a wider panel and red buttons, writing `gui_builder_restyled.png`; it must differ from `gui_builder.png`. |
| `gui_builder_pick_screenshot`, `gui_builder_pick_differs` | `--gui-pick 640,277` (the Quit button): the log must have `selected 'quit_button'`, and `gui_builder_pick.png` must differ from `gui_builder.png`. |
| `gui_builder_add_screenshot` | `--gui-select buttons --gui-add Button`, writing `gui_builder_add.png` (a third button in tree and preview, `*` in the titles; nothing saved). |
| `gui_builder_save`, `gui_builder_saved_file` | The same plus `--gui-save` on `build/<preset>/new_gui_project/` (a fresh copy): `Saved GUI` is logged and `ui/main_menu.rml` keeps its comment, the `<span>` and the `title` attribute and has the new `<button>Button</button>`. |
| `ui_document_inspector_screenshot` | `ui.scene --select 5`, writing `ui_document_inspector.png` (UI Document: Path combo, Visible). |
| `ui_scene_roundtrip` | `ui.scene` saved twice: byte-identical. |
| `assets_create_gui_screenshot`, `assets_create_gui_written` | `--assets-create gui:ui --focus-panel Assets` on `new_gui_project` (made by `new_gui_project_clean` / `_copy`): `ui/new_gui.rcss` exists, `ui/new_gui.rml` links `new_gui.rcss`, and the shot shows its rename. |
| `assets_create_menu_screenshot` | `--assets-menu / --focus-panel Assets`, writing `assets_create_menu.png` (Create: Lua System, State Machine, GUI). |
| `editor_no_focus`, `launcher_no_focus`, `player_no_focus`, `editor_audio_play_no_focus` | `kast_focus_probe` runs `kast_editor --frames 120`, `kast_editor --launcher --frames 120` the built `switch` game with `--frames 120` and the editor playing `audio.scene` with the Mixer open, sampling the foreground window until exit: fails if it ever belongs to the Kast process, if no window was shown, or on a nonzero exit. A `--frames` run shows its window without activating it and logs init failures instead of showing a message box. |

The screenshot tests each open a real window for ~2 seconds, in a fresh working directory (`<png>.run/`) so they
always start from the default layout.

## Shaders

Shaders (`editor/shaders/*.sc` for ImGui, `engine/shaders/*.sc` for meshes and grid lines, `engine/shaders/gui/*.sc`
for game GUIs, `engine/shaders/post/*.sc` for the post-processing passes; each directory has its own `varying.def.sc`) are compiled at build time to Direct3D 11 bytecode headers
(`-p s_5_0`) with `shaderc` from vcpkg's `bgfx[tools]` (a host dependency in `vcpkg.json`). Sources stay editable and
always match the installed bgfx version.

## Layout

```
engine/   log (stdout + in-memory lines), app (SDL3 window, bgfx init, main loop, resize, quit hook, shutdown),
          math3d (quaternion/Euler/TRS helpers, picking rays), scene (EnTT components + hierarchy),
          components (entt::meta component registry, generic JSON/copy), scene_io (.scene load/save),
          project (project.kast load/save, walk-up discovery, project-relative paths),
          systems (Play-mode systems, rotatorSystem), lua_systems (Lua systems: LuaJIT/sol2 session, kast.* API),
          lua_sandbox (shared Lua sandbox, Lua <-> field JSON), lua_components (Lua component definitions, hot
          reload, drift migration),
          input (game keyboard/mouse state),
          renderer (primitives, PBR shader, textures, grid/axes/camera frustums as optional overlays),
          material (.mat parsing, validation, path cache), pak (game.pak format),
          files (content reads from disk or a mounted pak), play (one Play frame: C++ systems, physics, Lua systems),
          physics (Jolt world per Play session, contact events, queries, character controller, collider wireframes),
          audio (miniaudio: voices, buses, clip cache, Music streaming, null-device fallback, offline render),
          network (kast.net: TCP/TLS, WebSocket and HTTP clients on Boost.Asio/Beast, one background thread),
          gui (RmlUi setup, file/log interfaces, GuiContext: one RmlUi context drawn into a bgfx view, SDL input),
          gui_render (RmlUi render interface on bgfx), gui_events (button click -> event bus adapter),
          scene_gui (a scene's UIDocuments in Play: load, stack, show/hide, hot reload), shaders/
player/   main.cpp (kast_player: the built game client)
editor/   main.cpp (CLI, dock layout, menus, console), document (scene, Play mode, file actions, dialogs, modals, title),
          camera (editor camera), viewport (Scene panel: render, camera input, picking, gizmo),
          game_view (Game panel: render from the active camera, game GUIs over it in Play),
          gui_doc (GUI Builder document model: RML tree edits, serialize, Create/rename GUI files),
          gui_builder (GUI Hierarchy, GUI Inspector, GUI Style, GUI Preview),
          panels (Hierarchy, Inspector incl. AudioSource Preview, Systems, Components, Assets, Lighting, Mixer, Network), build (Build: game.pak, exe, DLLs, last folder),
          imgui_bgfx (ImGui renderer for bgfx), shaders/
tests/    self_check.cpp, net_self_check.cpp, net_test_util (loopback servers, test certificates),
          net_test_server.cpp (kast_net_test_server) + net_test_server_data/ (its key), focus_probe.cpp, scenes/ (a project: project.kast, basic.scene, lua.scene + scripts/, no_camera.scene,
          malformed.scene, v1.scene, v3.scene), project/ (scene-switch and Lua components project: project.kast,
          scene_a.scene, scene_b.scene, health.scene, ui.scene + ui/main_menu.rml/.rcss, scripts/,
          components/health.lua, audio.scene + sounds/ (tone.wav, wave-demo.wav, mp3-demo.mp3) +
          scripts/audio_sfx.lua, net_a.scene, net_b.scene + scripts/net_a.lua, net_b.lua + certs/net_test_ca.pem)
cmake/    smoke_test.cmake, roundtrip_test.cmake, load_fails_test.cmake, differs_test.cmake, build_test.cmake,
          player_test.cmake
```

bgfx views: 1 = Scene panel, 2 = Game panel (each an offscreen color + depth target), 3 = game GUIs (into the Game
panel's target), 4 = GUI Preview (its own target), 255 = ImGui (backbuffer). kast_player: 0 = scene, 1 = game GUIs.
