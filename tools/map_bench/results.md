# Map size benchmark results

Numbers from `map_bench.ps1` ([README.md](README.md)); the raw lines are in `build/map_bench/results.log` of the run.
Later phases append their "after" tables below the baseline.

## Baseline (before the bigger-maps fixes)

Code under test: `f86a899` (main, 8 x 8 cap), Release builds. Machine: 32 threads, 64 GB RAM, RTX 4070 Ti SUPER 16 GB
(driver 610.88), Windows 11; Docker Desktop with the world stack. Measured 2026-10-30. Window 1600 x 900 on the
secondary display, no vsync. Size 8 is the same generator at today's maximum size (its spawn at 600, 600, see
`map_bench.ps1`); size 0 is today's default load test map (`tests/project/maps`, map 1).

Budgets (gates): editor >= 60 FPS sculpting; client <= 16.7 ms per frame moving across the map; world tick p99 no worse
than today's baseline at 500 players.

### Generated maps

| Size | Generate (editor, headless) | Heights on disk | kast_mapbake | Navtiles | Most polys in a tile | mapbake peak RAM |
|------|-----------------------------|-----------------|--------------|----------|----------------------|------------------|
| 8 x 8 | 0.8 s, 28 MB | 8 MB | 15.5 s | 64, 3 MB | 297 | 33 MB |
| 16 x 16 | 2.1 s, 110 MB | 32 MB | 86 s | 256, 9 MB | 419 | 65 MB |
| 32 x 32 | 11.6 s, 400 MB | 129 MB | 282 s | 1024, 19 MB | 261 | 172 MB |
| 64 x 64 | 49.9 s, 1,564 MB | 516 MB | 1,114 s | 4096, 52 MB | 358 | 596 MB |

The Detour per-tile poly limit (`1 << (22 - tileBits)`) is 16,384 at 16 x 16, 4,096 at 32 x 32 and 1,024 at 64 x 64:
these maps stay well under it (most 419), and the server loads all of them.

### Editor: sculpting (`--sculpt-loop --log-fps --no-vsync`, 1200 frames)

FPS over the last 10 one-second windows; RAM is peak working set, GPU is peak dedicated memory of the process.

| Size | FPS average | FPS lowest | CPU / GPU ms per frame | Peak RAM | Peak GPU memory | Result |
|------|-------------|------------|------------------------|----------|-----------------|--------|
| 8 x 8 | 116.8 (115.4 rerun) | 108.4 | 7.9 / 7.3 | 475 MB | 503 MB | meets 60 |
| 16 x 16 | 81.7 | 78.4 | 10.9 / 12.2 | 1,312 MB | 1,283 MB | meets 60; ~5 s at ~1 FPS after opening |
| 32 x 32 | 42.0 | 40.5 | 22.3 / 21.6 | 4,654 MB | 4,422 MB | **fails** 60 |
| 64 x 64 | - | - | - | 726-1,348 MB | ~0 | **fails**: the editor exits with 0xC0000005 (access violation) a few seconds after opening the map, or (one run) draws no terrain (Scene view 0 draws) |

Noise: an earlier pass while another program held ~7.6 GB of the GPU gave 70-77 / 47-59 / 32.5 FPS for 8 / 16 / 32;
the table is from a pass with the GPU otherwise idle (2.7 GB in use). The 64 x 64 cause is not diagnosed; one mesh per
tile (4096) reaching bgfx's default vertex/index buffer handle limits is a guess, not verified. The 32 x 32 editor
screenshots come out mostly black (menu, panels and Scene view) while the log shows 756 terrain draws: not diagnosed.

### Client: moving across the map (Terrain.radius 2, 0.7 m a frame on x and z, 3000 frames)

Frame time = each Lua update's dt (no vsync, no fixed dt): average / 99th percentile / max, ms.

| Size | Editor Play | Play peak RAM / GPU | kast_player | Player peak RAM / GPU | game.pak |
|------|-------------|---------------------|-------------|-----------------------|----------|
| 8 x 8 | 0.64 / 0.88 / 51.8 | 313 / 349 MB | 0.45 / 1.18 / 49.3 | 180 / 231 MB | 8 MB |
| 16 x 16 | 0.69 / 0.83 / 53.7 (0.69 / 0.82-0.91 / 52-53 reruns) | 330 / 352 MB | 0.50 / 1.22 / 49.9 (0.51 / 1.37-1.42 / 49-65 reruns) | 200 / 231 MB | 32 MB |
| 32 x 32 | 0.68 / 0.86 / 52.5 | 435 / 364 MB | 0.46 / 1.37 / 65.7 | 300 / 268 MB | 129 MB |
| 64 x 64 | **crashes** (0xC0000005) when Play opens | - | 0.47 / 1.47 / 70.6 | 989 / 235 MB | 516 MB |

Average and p99 meet 16.7 ms at every size that runs. The max (a single frame) is 50-70 ms at every size, 8 x 8
included: the frame a row of tiles streams in (loaded synchronously); it is not size dependent and is today's behaviour.
Player RAM grows with the map (the whole game.pak is read: 989 MB at 64 x 64).

### World server

Startup: Release `kast_world.exe` on Windows, no database (it stops right after `maps loaded`): time from launch to
`maps loaded` (all heights and navtiles read) and peak RSS.

| Size | Startup to `maps loaded` | Peak RSS |
|------|--------------------------|----------|
| 0 (default maps) | 0.12 s | 13 MB |
| 8 x 8 | 0.12-0.22 s | 22 MB |
| 16 x 16 | 0.25 s | 53 MB |
| 32 x 32 | 0.66 s | 160 MB |
| 64 x 64 | 1.91 s | 582 MB |

Load test: `tests/load.sh`, 500 wanderers (today's default COUNT), 120 s, MAP_THREADS 1, against the compose stack on
the same machine. Bench maps: `MAP_DIR`, `START_MAP`, `ZONE` = the map's edge, `TERRAIN` = bots at ground height.

| Size | Worst 10 s window tick p99 | Run tick p99 | Enter p50 / p99 | Corrections | load.sh |
|------|----------------------------|--------------|-----------------|-------------|---------|
| 0 (default maps, run 1) | 10.86 ms | 8.99 ms | 11 / 37 ms | 0 | PASS |
| 0 (run 2) | 10.68 ms | 8.96 ms | 8 / 41 ms | 0 | PASS |
| 0 (run 3) | 10.95 ms | 9.30 ms | 8 / 35 ms | 0 | PASS |
| 8 x 8 | 15.61 ms | 14.49 ms | 7 / 42 ms | 116,400 | FAIL (corrections) |
| 16 x 16 | 15.15 ms | 12.83 ms | 8 / 37 ms | 34,090 | FAIL (corrections) |
| 32 x 32 | 14.75 ms | 12.72 ms | 8 / 34 ms | 37,412 | FAIL (corrections) |
| 64 x 64 | 15.58 ms | 14.06 ms | 7 / 32 ms | 1,719 | FAIL (corrections) |

All tick p99s are under load.sh's 25 ms limit. The bench maps cost ~4-6 ms more p99 than the default map at every size,
8 x 8 included (terrain, a navmesh and NPC spawns vs. the flat default map), so the size itself moves p99 little; compare
a size against bench 8 x 8 as well as against size 0. The FAILs are load.sh's honest-mover check: the wanderers don't
follow the navmesh, and steps onto slopes too steep for it are corrected (`off_mesh`). An earlier 64 x 64 run had 327
of 500 bots time out on CharCreate (enter p99 3,010 ms); a rerun and a 500-bot direct run entered all 500 in < 35 ms p99,
so it was treated as the known bimodal enter latency (docs/services/world/tests.html), not size.

## After phase 3: editor terrain by camera radius

The Scene view meshes only the tiles within `zone::kEditRadius` = 4 of the camera's pivot (9 x 9 at most); a map past
9 x 9 frames those around its centre. Same machine and settings as the baseline, 2026-10-30. The machine's speed moved
between passes (8 x 8, unchanged code path, measured 117.7 then 71-86 FPS in later passes); the table is the pass with
the fastest 8 x 8 (14:02), the range over four later passes in brackets.

| Size | Terrain meshes (logged) | FPS average | FPS lowest | Peak RAM | Peak GPU memory | Result |
|------|-------------------------|-------------|------------|----------|-----------------|--------|
| 8 x 8 | 64 | 117.7 (71-86) | 108.8 (62-79) | 477 MB | 503 MB | meets 60 |
| 16 x 16 | 81 | 97.9 (69-99) | 90.9 (59-86) | 577 MB | 575 MB | meets 60 |
| 32 x 32 | 81 | 93.6 (65-95) | 79.6 (58-84) | 688 MB | 587 MB | meets 60 |
| 64 x 64 | 81 | 85.9 (56-86) | 79.9 (50-78) | 1,065 MB | 571 MB | meets 60 in a fast pass; 56-60 in slow ones |

The 64 x 64 crash was the Map tab: one thumbnail texture per tile, 4096 of bgfx's 4096 texture handles. Maps past
1024 tiles get no thumbnails until phase 4 builds only visible cells'. The 32 x 32 black screenshots are gone. 64 x 64
stays slower than 16 x 16 with the same 81 meshes: per-frame work over every tile (Map tab `declared_tiles`, Zone tab
grid, preview checks) is phase 4 / 6.

## After phase 6: other per-frame costs that grew with the map

Same machine and settings, 2026-10-30, Release. Profiled (sampling the editor's main thread, RelWithDebInfo) 64 x 64 vs
16 x 16 sculpting, both with 81 meshes. The 64 x 64 extra was:

- the Assets tree rescan every 0.5 s (`Panels::refreshAssets`: a 64 x 64 map is 4096 heights files and 4096 `.meta`s,
  ~6% of the frame time, a ~30 ms hitch twice a second): now only after a Windows change notification says something
  under the project changed (`FolderChanges`, editor/assets.h);
- the generated-terrain preview check comparing and copying the whole heights map every frame though the map doesn't
  use that material (~3%): the material is checked first;
- the Zone tab's spawn overlay copying every tile's spawns JSON every frame (~3%): read by reference.

Editor sculpting, three passes (the machine was in its slow state: 8 x 8 at 84-91 FPS, vs 117 in the fastest
baseline pass); median FPS average (range), lowest one-second window over the three passes:

| Size | FPS average, median (range) | FPS lowest | Peak RAM | Peak GPU memory | Before, same session (8324a51) | Result |
|------|-----------------------------|------------|----------|-----------------|--------------------------------|--------|
| 8 x 8 | 85.3 (84.3-91.2) | 64.7 | 476 MB | 502 MB | 86.7 | meets 60 |
| 16 x 16 | 71.1 (70.9-71.6) | 62.8 | 572 MB | 571 MB | 74.0 | meets 60 |
| 32 x 32 | 70.8 (69.4-71.2) | 62.1 | 671 MB | 571 MB | 64.9 | meets 60 |
| 64 x 64 | 67.9 (67.7-68.5) | 59.2 | 1,064 MB | 571 MB | 54.7 | meets 60 (average); one window at 59.2 |

Interleaved A/B of the two builds (before, after, before, after, ...; 1200 frames each): 64 x 64 before 57.4 / 61.1 /
58.6, after 68.2 / 67.3 / 66.4 FPS; 8 x 8 within noise (70.8-117 either build). 64 x 64 now runs within ~3 FPS of 16 x 16.

Client, two passes (average / p99 / max ms):

| Size | Editor Play | Play peak RAM | kast_player | Player peak RAM |
|------|-------------|---------------|-------------|-----------------|
| 8 x 8 | 0.65 / 0.88 / 51.9; 0.64 / 0.82 / 52.7 | 309 MB | 0.44 / 1.04 / 49.8; 0.45 / 1.10 / 51.3 | 176 MB |
| 16 x 16 | 0.70 / 1.06 / 62.5; 0.69 / 0.81 / 64.6 | 332 MB | 0.50 / 1.18 / 63.7; 0.50 / 1.20 / 49.7 | 201 MB |
| 32 x 32 | 0.68 / 0.80 / 65.6; 0.70 / 1.08 / 65.6 | 435 MB | 0.45 / 1.39 / 50.6; 0.46 / 1.44 / 50.2 | 300 MB |
| 64 x 64 | 1.04 / 2.40 / 86.7; 1.05 / 1.96 / 108.9 | 826 MB | 0.90 / 1.36 / 88.0; 0.91 / 1.20 / 81.6 | 989 MB |

Play now runs at 64 x 64 (the baseline crash was the Map tab's thumbnails, fixed in phases 3-4). Average and p99 meet
16.7 ms at every size. The single-frame max (a row of tiles streaming in synchronously) is 50-66 ms up to 32 x 32 and
82-109 ms at 64 x 64, where the average is also ~0.4 ms higher with the same p99: the streaming frames cost more on the
64 x 64 map. Not diagnosed (the sampling profile shows no single size-dependent function; Play also spends ~15% in
`zone::same`, the editor's unsaved-changes check comparing 4096 heights entries a frame, ~0.15 ms).

World server: startup to `maps loaded` 0.15-0.30 s / 22 MB (8), 0.25 s / 53 MB (16), 0.58-0.62 s / 160 MB (32),
1.97 s / 582 MB (64) (a first pass with a cold disk cache: 1.06 / 3.78 / 14.85 s for 16 / 32 / 64). Load test (no
server change in this phase):

| Size | Worst 10 s window tick p99 | Run tick p99 | Enter p50 / p99 | Corrections | load.sh |
|------|----------------------------|--------------|-----------------|-------------|---------|
| 0 (default maps) | 10.61 ms | 8.92 ms | 8 / 30 ms | 0 | PASS |
| 8 x 8 | 15.67 ms | 13.08 ms | 8 / 44 ms | 112,738 | FAIL (corrections) |
| 16 x 16 | 15.77 ms | 13.46 ms | 8 / 34 ms | 37,496 | FAIL (corrections) |
| 32 x 32 | 15.06 ms | 13.06 ms | 8 / 45 ms | 33,386 | FAIL (corrections) |
| 64 x 64 | 15.40 ms | 12.94 ms | 8 / 34 ms | 1,883 | FAIL (corrections) |

Every size's tick p99 matches bench 8 x 8 (and the baseline); the bench maps' +4-6 ms over size 0 is terrain/navmesh/NPCs,
not size.
