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

Output goes to `build/<preset>/`; runtime DLLs are copied next to `kast_editor.exe`.

## Run

```bat
build\release\kast_editor.exe
```

| Flag | Effect |
|------|--------|
| `--frames N` | Run N frames, then quit cleanly (exit 0). |
| `--screenshot <path.png>` | With `--frames`: capture the backbuffer after frame N, write a PNG, then exit. Exit 1 if it can't be written. |
| `--size WxH` | Initial window size (default `1280x720`). |
| `--ui-scale F` | Force UI scale `F` (positive number, clamped to 0.75–3.0) instead of the monitor's display scale. |
| `--scene <path>` | Open this `.scene` at startup instead of the default scene. Exit 1 if it can't be loaded. |
| `--save-scene <path>` | Save the startup scene (default or `--scene`) to `path` and exit 0 without opening a window. Exit 1 on write failure. |
| `--select <id>` | Select the entity with this persistent `id` at startup (shows it in the Inspector). Exit 1 if there is none. |
| `--play` | Enter Play mode right after the startup scene loads. Works with `--frames`, `--screenshot`, `--scene`, `--select`. |

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
file, **New from Template** writes `scripts/new_system.lua` (then `new_system_2.lua`, ...) next to the scene and adds
it; the arrows reorder and **Remove** drops an entry (the file stays). Both add buttons need a saved scene (tooltip
"Save the scene first"), and the list can't be edited during Play. Each edit marks the scene dirty. While playing,
each entry shows **OK** or **Disabled** with its last error. Layouts saved before the panel existed get it docked as a
tab beside Hierarchy.

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
they never mark the scene dirty. File New/Open/Save/Save As are disabled during Play; Exit or closing the window
stops Play first.

**File**: New (Ctrl+N), Open... (Ctrl+O), Save (Ctrl+S), Save As... (Ctrl+Shift+S), Exit. The title shows
`Kast Editor - <scene>` with `*` when there are unsaved changes; New, Open, Exit and closing the window ask
Save / Don't Save / Cancel first. Opening an invalid scene shows an error and keeps the current scene.
Moving the camera does not mark the scene dirty, but Save stores the current camera pose.

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
  "version": 3,
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
- `components`: keyed by registered component type name; every field of a present component is required.
  - `Transform` (required): local `position`, `rotation` (quaternion), `scale`, relative to the parent.
  - `MeshRenderer`: `primitive` is `cube`, `plane` or `sphere`; `color` is linear RGB 0–1.
  - `DirectionalLight`: `color` (linear RGB 0–1), `intensity`.
  - `Camera`: perspective camera looking down the entity's world -Z (scale ignored). `verticalFovDegrees` must be
    between 0 and 180 (exclusive), and `0 < nearPlane < farPlane`. The Inspector keeps the last valid value if an
    edit would break that.
  - `Rotator`: spins the entity in Play mode, `degreesPerSecond` around its local `axis` (normalized; zero = no spin).
- `systems`: Lua system scripts in run order, as paths relative to the `.scene` file's directory (see
  [Lua systems](#lua-systems)). Always written; optional when loading (version 2 files have none).
- `editor` belongs to the editor (camera pivot, yaw/pitch in degrees, distance in meters); the engine keeps it as-is.
- Loading rejects invalid JSON, a `version` other than 2 or 3 (version 1 files are not migrated), a `systems` that
  isn't an array of non-empty strings, unknown component types,
  a missing `Transform`, unknown, missing or mistyped fields, unknown enum names, out-of-range `Camera` values, duplicate ids, parents that don't
  exist, and parent cycles. It reports why and leaves the current scene untouched.

### Components and systems

Components are registered once with `entt::meta` in `engine/components.cpp` (`registerComponent<T>`, a stable type
name, then each field with a display name and an optional color hint). `ComponentInfo::validate` optionally checks
values beyond their types (e.g. `Camera` ranges); loading rejects and the Inspector reverts what it refuses. Field types: `float`, `int`, `bool`,
`std::string`, `bx::Vec3`, `bx::Quaternion` and registered enums. Registration alone gives a component scene-file
save/load, copy on Duplicate, Inspector widgets and Add/Remove Component. `PersistentId`, `Name` and `Hierarchy` are
structural and not registered.

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

**Paths**: `"systems": ["scripts/spin.lua", "../shared/ai.lua"]` are relative to the `.scene` file's directory
(`/` separators). The Systems panel stores paths that way, and Save As to another directory rewrites them so they
still point at the same files.

**`kast` API** (one read-only table per system; assigning to `kast` or `kast.input` is an error). Entities are
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

Input is only live while the Game panel is focused and playing; otherwise every key is up and the delta is 0.

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
Play and the other systems keep running. Disabled systems' `shutdown` is not called.

**Hot reload**: while playing, each listed file's modification time is checked every frame. When it changes, the old
instance's `shutdown` runs (if it was enabled), the file is reloaded into a fresh environment and its `init` runs.
A successful reload clears the error; the scene and entities are not reset, but the system's Lua locals start over.

The dock layout is saved to `imgui.ini` in the working directory; delete it to get the default layout back.

## Tests

`ctest --preset debug` (or `release`) runs:

| Test | What it checks |
|------|----------------|
| `engine_self_check` | `kast_self_check`: rotation conventions, Euler/quaternion and matrix decomposition, picking rays and ray-box hits, reparent keeping the world transform, cycle rejection, subtree duplicate/delete, component registry (add/remove/has, JSON round trip of every field type, duplicate copies all components), scene-file validation (including invalid `Camera` values), active camera (first depth-first) and camera view/projection matrices, `kast::Input`, `rotatorSystem`, in-memory snapshot restore, editor Play/Pause/Stop on a `Document` (dirty flag, selection, byte-identical restore, Game/Scene focus requests, no-camera warning, default Main Camera), and Lua systems: scene `"systems"` round trip and validation, order after the C++ systems, init/update/shutdown with Play/Pause/Stop, the whole `kast.*` API and its errors, sandbox and per-system globals, syntax/runtime errors disabling only that system, hot reload, Systems panel edits and Save As path rebasing. No window. |
| `editor_smoke` | `kast_editor --frames 120 --screenshot build/<preset>/smoke.png` (default scene): exit 0 and a non-empty PNG. |
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
| `scene_bad_json`, `scene_missing`, `scene_v1_rejected` | `--scene` with `tests/scenes/malformed.scene` / a missing file / the version 1 `tests/scenes/v1.scene` must exit 1. |

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
          systems (Play-mode systems, rotatorSystem), lua_systems (Lua systems: LuaJIT/sol2 session, kast.* API),
          input (game keyboard/mouse state),
          renderer (primitives, lit shader, grid/axes/camera frustums as optional overlays), shaders/
editor/   main.cpp (CLI, dock layout, menus, console), document (scene, Play mode, file actions, dialogs, modals, title),
          camera (editor camera), viewport (Scene panel: render, camera input, picking, gizmo),
          game_view (Game panel: render from the active camera),
          panels (Hierarchy, Inspector, Systems), imgui_bgfx (ImGui renderer for bgfx), shaders/
tests/    self_check.cpp, scenes/ (basic.scene, lua.scene + scripts/, no_camera.scene, malformed.scene, v1.scene)
cmake/    smoke_test.cmake, roundtrip_test.cmake, load_fails_test.cmake, differs_test.cmake
```

bgfx views: 1 = Scene panel, 2 = Game panel (each an offscreen color + depth target), 255 = ImGui (backbuffer).
