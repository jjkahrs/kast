# Kast Zone Builder

`kast_zone_builder` creates and edits a project's zone maps (`maps/<folder>/`) in 3D and saves them in place, in the
formats the engine and the world server load. It is a standalone app; the editor does not need to be open.

Full documentation: [docs/index.html](docs/index.html).

## Build

Built with everything else from the repo root (see the root [README](../../README.md)):

```bat
build.bat              :: or build.bat release
```

Output: `build\<preset>\kast_zone_builder.exe`.

## Run

```bat
build\debug\kast_zone_builder.exe
build\debug\kast_zone_builder.exe --project samples\starter-mmo --map meadow
```

Or use `tools\zone-builder\run.bat [debug|release] [flags...]` (default debug) from any directory; it runs from
the repo root, so relative paths like `samples\starter-mmo` still resolve.

File > Open Project... (or `--project <dir>`) lists the project's `maps/*/map.json`; click one to open it. The
**Bake** panel runs `kast_mapbake` (see [services/world/tools/mapbake](../../services/world/tools/mapbake/README.md)).

## Generated terrain

The **Terrain** panel can **Generate** random, seamless heights for the whole map (fBm Perlin noise: seed, scale,
octaves, persistence, lacunarity), color the ground by height with a **Height colors** gradient (`map.json`
`"terrainColors"`), and **Create texture & material**: the map then uses `materials/<folder>_terrain.mat` with one
texture spanning the map (`"terrainUv": "map"`). Every save re-bakes `textures/<folder>_terrain.png` (256 px per tile,
at most 4096 px) and the `.mat` from the current heights; no other material or texture is ever written. From the
command line:

```bat
build\debug\kast_zone_builder.exe --project samples\starter-mmo --new-map dunes,2,2 --generate-heights 7 ^
  --terrain-colors 0,0.76,0.7,0.5,40,0.33,0.52,0.21,100,0.95,0.95,0.97 --create-terrain-material --save --headless
```

Details: [docs/index.html#generated-terrain](docs/index.html#generated-terrain).
