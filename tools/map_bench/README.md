# Map size benchmark

`map_bench.ps1` measures how the editor, the runtime client and the world server cope with big zone maps. It generates
N x N tile maps (default 16, 32 and 64) **at run time** under `build/map_bench/<N>/` (nothing large is committed) and
appends every number to `build/map_bench/results.log`. Recorded results: [results.md](results.md).

## What it generates

`build/map_bench/<N>/project/`: a minimal project (`project.kast`, `materials/terrain_grass.mat` and its texture from
`tests/project`, `scripts/map_bench_move.lua`, `scenes/bench.scene`) with `maps/bench<N>` (map id 200 + N):

- `map.json` written by the script (tiles `[0, 0, N-1, N-1]`, height range 0..400 m, the grass material). It is written
  directly, not with `--new-map`, so the editor's New Map size cap does not limit it.
- Heights from the editor's own zone flags: `--generate-heights 7,1024,6,0.5,2` (fBm, 1 km features), the map spawn
  on the ground, one `Bat` spawn per 4 x 4 tiles, `--save-zone --headless`.
- Navtiles from `kast_mapbake` (the `bake` step).
- `scenes/bench.scene`: the Terrain (radius `-Radius`, default 2), a sun, and a camera that
  `map_bench_move.lua` flies 0.7 m a frame on x and z (~1 m a frame) along the map's diagonal, 450 m up, so every size
  streams the same tiles on the same frames; it logs the frame time (average, 99th percentile, max) over 3000 frames
  after 120 of warmup. `play` and `player` refresh the script from this folder.

## Steps (`-Steps`, default all but `shots`)

| Step | What it runs | Logged |
|------|--------------|--------|
| `generate` | the project and map above (kept if present; `-Force` regenerates) | time, peak RAM, heights size |
| `bake` | `kast_mapbake maps/bench<N>` | time, navtile count and size, most polys in a tile |
| `shots` | Zone tab (Map section) and Map tab screenshots | `build/map_bench/screenshots/` |
| `editor` | `kast_editor --map bench<N> --sculpt-loop --log-fps --no-vsync`, 1200 frames | FPS average and lowest over the last 10 one-second windows, CPU / GPU ms, seconds at under half that FPS after opening, Scene view draws, peak RAM and GPU dedicated memory |
| `play` | the editor's Play of `bench.scene`, no vsync | frame time average / p99 / max while flying, peak RAM / GPU |
| `player` | Build (`kast_editor --build`), then the built `kast_player` the same way | the same |
| `server` | Release `kast_world` on the map with no database (it stops after `maps loaded`) | time to `maps loaded`, peak RSS |
| `load` | `services/world/tests/load.sh` with `MAP_DIR`, `START_MAP`, `ZONE` (the map's edge) and `TERRAIN` (bots walk at ground height) | worst window and run tick p99, enter p50 / p99, corrections, PASS / FAIL |

The load bots wander without following the navmesh, so on relief some steps land on slopes too steep for it and are
corrected (`off_mesh`); `load.sh` then reports FAIL (its honest-mover check). Compare the tick p99 against size 0 and
size 8 (the same generator at today's maximum size).

Size `0` means today's default map (`tests/project/maps`, `START_MAP` 1) and runs only `server` and `load`: the load
test baseline.

Each run is killed after `-TimeoutSec` (900; `-BakeTimeoutSec` 3600 for `kast_mapbake`) and logged as `TIMED OUT`,
so a size the current code can't handle still gives a result. GPU memory is the Windows `GPU Process Memory`
counter of the process (dedicated usage, all adapters); RAM is its peak working set.

## Run

Release builds first: `build.bat release` and, in `services/world`, `cmake --build --preset release` (see the
[World Server setup](../../docs/services/world/setup.html)). Close GPU-heavy apps. Windows open off the main monitor
(`KAST_WINDOW_POS=secondary`; `-WindowPos X,Y` pins them instead).

```bat
rem everything but the load test, all three sizes (64 x 64 takes ~19 min of kast_mapbake)
powershell -ExecutionPolicy Bypass -File tools\map_bench\map_bench.ps1 -Steps generate,bake,shots,editor,play,player,server

rem one size, some steps
powershell -ExecutionPolicy Bypass -File tools\map_bench\map_bench.ps1 -Sizes 16 -Steps editor,play

rem load tests: the compose stack up first (cd services\world && docker compose up -d), then the baseline and the maps
rem (the bot runs from the kast-world:dev image: docker compose build world after bot changes)
powershell -ExecutionPolicy Bypass -File tools\map_bench\map_bench.ps1 -Sizes 0,8,16,32,64 -Steps load
```

Load test options: `-LoadCount` (default 500, `load.sh`'s default) and `-LoadDuration` (default 120 s).
