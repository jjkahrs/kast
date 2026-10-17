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

File > Open Project... (or `--project <dir>`) lists the project's `maps/*/map.json`; click one to open it. The
**Bake** panel runs `kast_mapbake` (see [services/world/tools/mapbake](../../services/world/tools/mapbake/README.md)).
