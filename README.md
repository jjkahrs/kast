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
| `--focus-panel <Window>` | Bring that editor window (e.g. `Events`, `"State Machines"`) to the front of its dock after the first 10 frames, for screenshots. |

Without `--project`, `--scene`, `--save-scene` or `--build`, `kast_editor` runs the Project Manager instead of the
editor. Editor-only flags (`--select`, `--play`, `--add-component-popup`, `--new-component[-prompt]`,
`--new-state-machine-prompt`, `--open-menu`, `--open-build-settings`, `--focus-panel`) then are invalid arguments, as are Project Manager flags (`--launcher`, `--new-project`,
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
  "stateMachines": ["game_flow"]
}
```

- `version` must be 1, `name` a non-empty string, `startScene` a project-relative path or `null` (only its type is
  checked; opening the project opens it, and a built game starts there, see [Build](#build)). These three keys are
  required.
- `stateMachines` (optional): the [game state machines](#state-machines) every Play session runs, as definition names;
  an array of unique non-empty strings. A name without a valid definition is an error at Play start, not here. The
  State Machines panel's **Global** checkbox edits it.
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
- **Build...** asks for an output folder, then builds the game into it.
- **Build and Run** builds into the project's last output folder (asks if there is none), then starts the game as a
  separate process.

Both first offer to save unsaved scene changes (the same prompt as File > Open), because the game is built from the
files on disk. The last output folder is remembered per project in `<project>/.kast/editor.json`
(`{"buildFolder": "..."}`), never in `project.kast`. Errors go to the Console and an Error popup.

If `startScene` is not set, outside the project or doesn't load, **Build...** and **Build and Run** log why and open
Build Settings instead (after the unsaved-changes prompt, before any folder dialog); saving there doesn't resume the
build, run it again. Headless `--build` fails instead (exit 1), its error pointing at Build > Build Settings.

Before writing anything, Build checks that `startScene` is set, inside the project and loads, that the output folder
isn't the project folder itself, and that `kast_player.exe` and its DLLs are next to the editor. Then it writes, into
the output folder (created if needed, overwriting only these files):

```
<Name>.exe   copy of kast_player.exe
game.pak     the project's files
SDL3.dll     runtime DLLs kast_player.exe imports (dumpbin /dependents)
lua51.dll
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
window and doesn't hot reload. Flags (for tests): `--frames N` and `--screenshot out.png`, as in the editor. A missing
or corrupt `game.pak`, an invalid `project.kast` or a `startScene` that is missing or doesn't load shows an error
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
- Lighting: Lambert from the first directional light (in hierarchy order) plus a fixed 0.2 ambient; ambient only if
  there is no light.

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
        "MeshRenderer": { "primitive": "sphere", "color": [0.3, 0.45, 0.85] },
        "Rotator": { "axis": [0, 1, 0], "degreesPerSecond": 45 } } },
    { "id": 5, "name": "Satellite", "parent": 4,
      "components": {
        "Transform": { "position": [0.9, 0.6, 0], "rotation": [0, 0, 0.258819, 0.965926], "scale": [0.3, 0.3, 0.3] },
        "MeshRenderer": { "primitive": "cube", "color": [0.95, 0.8, 0.2] } } },
    { "id": 6, "name": "Main Camera", "parent": null,
      "components": {
        "Transform": { "position": [0, 2.5, 6], "rotation": [-0.173648, 0, 0, 0.984808], "scale": [1, 1, 1] },
        "Camera": { "verticalFovDegrees": 60, "nearPlane": 0.1, "farPlane": 1000 } } }
  ],
  "systems": [],
  "editor": { "camera": { "pivot": [0, 0.75, 0], "yaw": 20, "pitch": -20, "distance": 8 } }
}
```

- `id`: persistent nonzero integer, unique in the file. `parent` is the parent's `id`, or `null` for a root. Children
  keep file order.
- `components`: keyed by component type name; built-ins first (registration order), then [Lua
  components](#lua-components) (sorted by name), then preserved components of unknown type (sorted by name). For a
  built-in component every field is required.
  - `Transform` (required): local `position`, `rotation` (quaternion), `scale`, relative to the parent.
  - `MeshRenderer`: `primitive` is `cube`, `plane` or `sphere`; `color` is linear RGB 0–1.
  - `DirectionalLight`: `color` (linear RGB 0–1), `intensity`.
  - `Camera`: perspective camera looking down the entity's world -Z (scale ignored). `verticalFovDegrees` must be
    between 0 and 180 (exclusive), and `0 < nearPlane < farPlane`. The Inspector keeps the last valid value if an
    edit would break that.
  - `Rotator`: spins the entity in Play mode, `degreesPerSecond` around its local `axis` (normalized; zero = no spin).
  - `StateMachine`: gives the entity its own [state machine](#state-machines) in Play mode. `machine` is a definition
    name, `initialState` the state it enters at Play start (empty = the definition's `initial`). Both are plain
    strings: a name with no valid definition still loads and saves unchanged. Scene files stay version 4.
- `systems`: Lua system scripts in run order, as paths relative to the **project root** (see [Projects](#projects) and
  [Lua systems](#lua-systems)). Always written; optional when loading (missing = none).
- `editor` belongs to the editor (camera pivot, yaw/pitch in degrees, distance in meters); the engine keeps it as-is.
- Loading rejects invalid JSON, a `version` other than 4 (versions 1–3 are not migrated), a `systems` that
  isn't an array of non-empty strings, a missing `Transform`, unknown, missing or mistyped fields of a built-in
  component, unknown enum names, out-of-range `Camera` values, duplicate ids, parents that don't exist, and parent
  cycles. It reports why and leaves the current scene untouched. Lua components are lenient (schema drift), and a
  component name that is neither built-in nor a valid Lua definition is kept verbatim with a Console warning (missing
  definitions); see [Lua components](#lua-components).

### Components and systems

Components are registered once with `entt::meta` in `engine/components.cpp` (`registerComponent<T>`, a stable type
name, then each field with a display name and an optional color hint). `ComponentInfo::validate` optionally checks
values beyond their types (e.g. `Camera` ranges); loading rejects and the Inspector reverts what it refuses. Field types: `float`, `int`, `bool`,
`std::string`, `bx::Vec3`, `bx::Quaternion` and registered enums. Registration alone gives a component scene-file
save/load, copy on Duplicate, Inspector widgets and Add/Remove Component. `PersistentId`, `Name` and `Hierarchy` are
structural and not registered.

The uniform layer at the end of `engine/components.h` (`ComponentType`, `FieldDesc`, `allComponentTypes`,
`findAnyComponentType`, `hasComponent/addComponent/removeComponent/componentToJson/setComponentJson`) covers built-in
and [Lua components](#lua-components) alike, with JSON as the common value form; scene IO, the Inspector,
Add/Remove Component, Duplicate, Play snapshots and `kast.*` use it. The `entt::meta` functions above stay for C++ code
working with built-ins.

Systems (`engine/systems.h`) are plain functions `void(Scene&, float dt)`, listed in update order in
`engine/systems.cpp`. Only Play mode runs them, once per frame with the real frame time, never while paused or editing.

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
`payload`, `publisher`). A `Subscription` must not outlive its bus; after `reset()` it is a harmless no-op. No engine
code publishes events yet.

**Events panel** (editor, tabbed with Console): the last 500 delivered events, oldest first, following new ones while
scrolled to the bottom. Columns: frame, event name, compact payload, publisher (script path or `C++`). **Clear**
empties it; the filter shows events whose name contains the text (case-sensitive). Play start clears it; it stays
after Stop. `--focus-panel Events` brings it to the front for screenshots.

**Sample**: `tests/project/events.scene` runs `scripts/health_events.lua` (a `heartbeat` every 20 frames and, on frame
30, `health.empty` with `{id, title}` for every entity with `Health`; it changes nothing itself) and
`scripts/on_health_empty.lua` (greys out the named entity's `MeshRenderer`).

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
`fsms/entities/traffic_light.lua`, whose `enter` recolors the `MeshRenderer`) and the project lists the game machine
`game_flow` (`fsms/game_flow.lua`). `scripts/fsm_driver.lua` moves `game_flow` from `boot` to `playing`, then steps
each light once (red → green, green → yellow) and tries to switch both to red: the yellow one may, the green one is
denied with a warning.

The dock layout is saved to `imgui.ini` in the working directory; delete it to get the default layout back.

## Tests

`ctest --preset debug` (or `release`) runs:

| Test | What it checks |
|------|----------------|
| `engine_self_check` | `kast_self_check`: rotation conventions, Euler/quaternion and matrix decomposition, picking rays and ray-box hits, reparent keeping the world transform, cycle rejection, subtree duplicate/delete, component registry (add/remove/has, JSON round trip of every field type, duplicate copies all components), scene-file validation (including invalid `Camera` values and versions 1–3 rejected), active camera (first depth-first) and camera view/projection matrices, `kast::Input`, `rotatorSystem`, in-memory snapshot restore, editor Play/Pause/Stop on a `Document` (dirty flag, selection, byte-identical restore, Game/Scene focus requests, no-camera warning, default Main Camera), and Lua systems: scene `"systems"` round trip and validation, order after the C++ systems, init/update/shutdown with Play/Pause/Stop, the whole `kast.*` API and its errors, sandbox and per-system globals, syntax/runtime errors disabling only that system, hot reload, Systems panel edits; projects: `project.kast` load/save/validation, walk-up discovery, project-relative paths and escape rejection, opening a project and Save/Save As on a `Document` (New Project folder-name sanitizing, every validation rule, creating `<dir>/my-game/project.kast` with the trimmed name, never creating missing parents or writing into an existing folder) (no rebasing, project-change warning, title format), the Project Manager's recent list (save/load round trip, newest first, case-insensitive dedupe with trailing separators, Remove leaving the project on disk, missing folder / missing or invalid `project.kast` reported, three kinds of corrupt file backed up to `projects.json.bad` with a warning and read as empty) and an editor that can't be started reporting why, no-project Lua disabling, Add Script outside the project, template location; `kast.loadScene` (end-of-frame, last call wins, ignored in `shutdown`, init calls deferred a frame, shutdown/init order, every error case keeps the current scene) and `kast.currentScene`, and Stop restoring the pre-Play scene after two switches; events: the C++ bus (publish then subscription order, next-drain re-publish, subscribe/unsubscribe during a drain, a throwing subscriber, RAII and stale handles, dropQueued, reset, the 500-event history and Clear), Lua→Lua in the same frame after every update with independent payload copies, Lua→C++ and C++→Lua payload conversion, one subscription order across C++ and Lua, every `kast.publish/subscribe/unsubscribe` rejection raising at file:line and queueing nothing (bad names, functions, userdata, threads, cycles, nesting past 32, mixed keys, NaN), a failing subscriber disabling only its system, cleanup on disable/hot reload/Stop/Play start, a scene switch dropping queued events and old Lua subscribers but keeping C++ ones, and Pause holding / Stop discarding the queue in editor Play; Lua components: definition parsing (every field type and option, recursive discovery, every invalid-definition case naming the file), name clashes (built-in, structural, both duplicate files rejected), sandbox and per-file globals, registry parity (add/has/remove, strict JSON set, Duplicate, save order, byte-identical save/load, snapshot and Document Play/Stop restore), `kast.*` on a Lua component with every `kast.set` error leaving it unchanged, drift rules (one warning per fix, dirty only when data changed, built-ins still strict), missing definitions (load with warnings, verbatim save, `kast.*` raises, later conversion), hot reload of modified/added/deleted/broken files without data loss and the Play freeze, Components panel template naming and enable rules; game.pak: write/read round trip (byte-identical, binary and empty files), directory listing, rejection of bad magic, bad version, truncated header/index/data, an offset past the end and an overflowing size; a mounted pak serving project.kast, readFile and listFiles (and not the disk); Build: exe-name sanitizing, pak contents (no dotfiles, dot-directories or output folder), every validation failure writing nothing, a full build (exe, game.pak, DLLs, other files kept), the remembered folder in `.kast/editor.json` (project.kast unchanged) and the Build menu enable rule (no project, Play, Pause); Build Settings: Start Scene choices (`tests/project` lists `events.scene`, `fsm.scene`, `health.scene`, `scene_a.scene`, `scene_b.scene`; a dotfile or dot-directory scene is left out), Save writing only `startScene` (same keys and order, reloads, `doc.project` updated, the open scene's dirty flag and path kept, no `.kast/editor.json`), a scene that doesn't load or escapes the project leaving `project.kast` byte-identical, opening (then Cancel) writing nothing, and Build / Build and Run with `startScene` null or missing opening Build Settings; state machines: recursive discovery, every invalid-definition case naming the file (bad or missing `name`, bad `initial`, unknown `allowedFrom` state, wrong types, unknown keys, syntax and sandbox errors, both duplicate files), `allowedFrom` / omitted / `"*"` / self-transition rules, editor rescans, entry order at Play start (after the inits, game then entity machines, no event, `initialState` fallback warning, first touch during `init`), allowed and denied transitions (return value, warning, no callbacks, no event), `exit` → switch → `enter` → `fsm.changed {machine, entity, from, to}` with the definition as publisher, queued changes from callbacks (no recursion, denial on apply), callback errors (file:line and traceback, commit, still usable), C++ misuse throwing, every `kast.fsm.*` misuse raising at the calling line, read-only `kast.fsm`, identical results from Lua and C++, game state kept across `kast.loadScene` with no callbacks and new callbacks after it, Stop then Play starting over, runtime add (`kast.add` + `kast.set`, registry edit, Duplicate), destroy/remove without `exit`, a missing definition logged once, hot reload (state kept, reset with warning when removed, last valid definition kept when broken, game machines too), `StateMachine` byte-identical save/load, Duplicate and Stop restore, `project.kast` `stateMachines` round trip and validation, New State Machine name rules / template / no overwrite, Global toggling `project.kast`, and both blocked during Play. No window. |
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
| `lua_components_play_differs` | The two must differ: only `scripts/health_drain.lua` (drains `Health`, fades `MeshRenderer.color` from `Health.tint`) can change them. |
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

The screenshot tests each open a real window for ~2 seconds, in a fresh working directory (`<png>.run/`) so they
always start from the default layout.

## Shaders

Shaders (`editor/shaders/*.sc` for ImGui, `engine/shaders/*.sc` for meshes and grid lines; each directory has its own
`varying.def.sc`) are compiled at build time to Direct3D 11 bytecode headers
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
          renderer (primitives, lit shader, grid/axes/camera frustums as optional overlays), pak (game.pak format),
          files (content reads from disk or a mounted pak), play (one Play frame: C++ then Lua systems), shaders/
player/   main.cpp (kast_player: the built game client)
editor/   main.cpp (CLI, dock layout, menus, console), document (scene, Play mode, file actions, dialogs, modals, title),
          camera (editor camera), viewport (Scene panel: render, camera input, picking, gizmo),
          game_view (Game panel: render from the active camera),
          panels (Hierarchy, Inspector, Systems, Components), build (Build: game.pak, exe, DLLs, last folder),
          imgui_bgfx (ImGui renderer for bgfx), shaders/
tests/    self_check.cpp, scenes/ (a project: project.kast, basic.scene, lua.scene + scripts/, no_camera.scene,
          malformed.scene, v1.scene, v3.scene), project/ (scene-switch and Lua components project: project.kast,
          scene_a.scene, scene_b.scene, health.scene, scripts/, components/health.lua)
cmake/    smoke_test.cmake, roundtrip_test.cmake, load_fails_test.cmake, differs_test.cmake, build_test.cmake,
          player_test.cmake
```

bgfx views: 1 = Scene panel, 2 = Game panel (each an offscreen color + depth target), 255 = ImGui (backbuffer).
