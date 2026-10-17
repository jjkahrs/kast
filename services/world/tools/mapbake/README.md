# Kast Map Bake

`kast_mapbake` lints a zone map folder (height seams, spawn files) and bakes one Recast navtile per declared tile into
`<mapDir>/.server/nav/` for the world server's navmesh. Exit 0: all baked; 1: lint or bake errors (nothing written);
2: usage.

Full documentation: [docs/index.html](docs/index.html).

## Build

Built with the world server, from `services/world` in a VS 2022 x64 developer prompt with `VCPKG_ROOT` set (see
[World Server setup](../../../../docs/services/world/setup.html)):

```bat
cmake --preset debug
cmake --build --preset debug
```

Output: `services/world/build/debug/kast_mapbake.exe`. The Docker image (`docker compose build`) contains it too.

## Run

```sh
kast_mapbake <mapDir> [--out <dir>]
build/debug/kast_mapbake.exe ../../tests/project/maps/hills      # from services/world
```
