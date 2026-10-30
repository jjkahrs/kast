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
