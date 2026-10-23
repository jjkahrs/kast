# Kast
![Kast](kast-icon.png)

Current Version: 0.1.0

Kast is a C++20 game engine for building MMORPGs. `engine/` is the reusable runtime (SDL3 window + bgfx Direct3D 11, EnTT
scene, renderer, scene files); `editor/` is the Unity-style editor (`kast_editor`) built on it with Dear ImGui docking
and ImGuizmo. `services/` holds the game services: the authentication server (`services/auth`, accounts, sessions and
the realm list), the world server (`services/world`) and the chat service (`services/chat`: whispers, channels, zone
chat, GM mutes; one per realm). `services/admin` is the admin web app (React SPA + Node API) admins and GMs use to
manage accounts, bans, characters, realms and online players. `samples/starter-mmo` is a complete online game project.

Documentation is HTML under [`docs/`](docs/index.html): [Editor](docs/editor/index.html),
[Engine](docs/engine/index.html), [Auth Server](docs/services/auth/index.html),
[World Server](docs/services/world/index.html), [Chat Server](docs/services/chat/index.html), [Admin App](docs/services/admin/index.html), [Development](docs/development/tests.html) (tests, source layout).
Zone maps are built in the editor's Zone tab ([Zone Builder](docs/editor/zone-builder.html)). Tools document
themselves in their own folders: [Map Bake](services/world/tools/mapbake/README.md).

## Prerequisites (Windows)

- Visual Studio 2022 with the C++ workload (MSVC v143)
- CMake 3.25+ and Ninja on `PATH`
- vcpkg checkout with `VCPKG_ROOT` set (e.g. `C:/vcpkg`)

Dependencies (`sdl3`, `bgfx`, `imgui[docking-experimental,sdl3-binding]`, `imguizmo`, `imgui-node-editor` (the editor's Shader Graph canvas), `entt`, `nlohmann-json`, `luajit`, `sol2`) install automatically from `vcpkg.json`
on first configure. The first configure builds bgfx's shader tools (glslang, SPIRV-Tools, ...) and takes several minutes.

`CMakePresets.json` sets `VCPKG_VISUAL_STUDIO_PATH` to `C:/Program Files/Microsoft Visual Studio/2022/Community`
so vcpkg builds ports with VS 2022 even when newer Visual Studio versions are installed. Edit it if VS 2022 lives elsewhere.

## Quick start

```bat
build.bat              :: configure + build (relwithdebinfo: optimized, still debuggable). build.bat debug|release for the others
run.bat                :: run build\relwithdebinfo\kast_editor.exe from the repo root
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

cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo
ctest --preset relwithdebinfo
```

Which preset: `debug` has the debug CRT and STL checks (asserts, iterator checks) and is many times slower, which
matters in large scenes such as the starter MMO's meadow. `relwithdebinfo` is optimized like `release` but keeps the
PDBs, so a debugger still breaks, steps and shows call stacks (locals may be optimized away). Use it for playing and
profiling; `release` for shipping.

Output goes to `build/<preset>/`: `kast_editor.exe`, `kast_player.exe` (the game client Build copies) and
`kast_self_check.exe`; runtime DLLs are copied next to them. The editor also gets `shaderc.exe` and a `kshader/` folder
(bgfx shader headers and Kast's mesh shader templates) next to it, to compile [user shaders](docs/engine/shaders.html),
and `texturec.exe` (with `miniz.dll` and `squish.dll`) to [compress textures](docs/engine/materials.html#texture-compression);
keep them beside `kast_editor.exe` if you copy it elsewhere. Built games don't need them.

## Start the editor

```bat
run.bat                :: build\relwithdebinfo\kast_editor.exe from the repo root
build\release\kast_editor.exe
```

Without `--project`, `--scene`, `--save-scene` or `--build`, `kast_editor` runs the Project Manager; e.g.
`run.bat debug --project samples\starter-mmo` opens a project directly. Every flag:
[Editor command line](docs/editor/command-line.html).

## Start the services

Both need Docker. Each `docker compose` stack is self-contained; their host ports differ, so both can run at once.

**Auth server** (Node 24 + MySQL 8.4 + Mailpit; details in [Auth Server setup](docs/services/auth/setup.html)):

```sh
cd services/auth
cp .env.example .env        # dev defaults; change the passwords
docker compose up -d --build
curl -s localhost:3000/health   # {"status":"ok","db":"ok"}
```

**World server and chat service** (world + chat + auth built from `../auth` + MySQL + Mailpit; details in
[World Server setup](docs/services/world/setup.html) and [Chat service setup](docs/services/chat/setup.html)):

```sh
cd services/world
cp .env.example .env              # dev defaults; change the passwords and CHAT_LINK_SECRET (16+ characters)
docker compose up -d --build      # world (7777) + chat (7778) + auth (3100) + admin (3200) + MySQL 8.4 + Mailpit
docker compose ps                 # all healthy
docker compose logs world         # "migration applied", "listening", "chat link: up"
docker compose logs chat          # "listening", "world link up"
```

**Create accounts** with the user CLI. It runs in the `auth` container, so it works from `services/auth` or
`services/world` while that stack is up. Accounts are created pre-verified, and the role is `player`, `gm` or `admin`.
Details in [Auth Server setup](docs/services/auth/setup.html#commands).

```sh
docker compose exec auth node dist/user-cli.js create you@example.com admin   # prompts for the password (hidden)
docker compose exec auth node dist/user-cli.js set-role you@example.com gm    # change an existing account's role
```

Without `--password`, `create` prompts for the password, which keeps it out of shell history. Passwords are 10-128
characters. `create` fails if the email is already taken; use `set-role` to change that account instead. From a host
checkout with `npm ci` done, `npm run user -- ...` in `services/auth` does the same against `DB_*` in `.env`.

**Admin app** (in the same world stack; details in [Admin App setup](docs/services/admin/setup.html)): set
`ADMIN_DB_PASSWORD`, `ADMIN_WORLD_DB_PASSWORD`, `WORLD_ADMIN_SECRET`, `SEED_ADMIN_EMAIL` and `SEED_ADMIN_PASSWORD` in
`services/world/.env` (see `.env.example`), then `docker compose up -d --build` also builds and starts `admin`. Open
http://localhost:3200 and sign in with the seed admin. To build it outside Docker:

```sh
cd services/admin
npm ci
npm run build       # API -> dist/, SPA -> web/dist/
npm start           # serves both on :3200 (configure .env from .env.example first)
npm run dev         # development: Vite on :5173, proxying /api to `npm run dev:api` on :3200
```

The chat service also builds on its own on Windows: `cd services/chat && cmake --preset debug && cmake --build --preset
debug && ctest --preset debug` (it compiles `services/world`'s protocol and shared server code).

`docker compose down` stops a stack and keeps its data; `down -v` also deletes it. To build the world server on
Windows instead (MSVC + vcpkg), see [World Server setup](docs/services/world/setup.html#building-on-windows-msvc--vcpkg).
