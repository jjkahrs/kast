# Third-Party Notices

Kast uses the third-party software and assets listed below. Each is the property of its authors and is used under the license shown.

The full license text for each C++ library comes with it from vcpkg. You can find it at `vcpkg_installed/<triplet>/share/<port>/copyright` in a build directory. Node packages carry theirs in `services/auth/node_modules/<package>/LICENSE`. Versions are the ones at vcpkg baseline `ed8445dd2a9ba44e0e3ace4e952b2e33130082d9`.

## Editor, player and built games

These are linked into `kast_editor` and the player, or shipped beside them as DLLs.

| Library | Version | License | Homepage |
|---|---|---|---|
| SDL3 | 3.4.2 | Zlib | https://www.libsdl.org |
| bgfx / bx / bimg | 1.129.8940 | BSD-2-Clause | https://github.com/bkaradzic/bgfx |
| Dear ImGui | 1.92.6 | MIT | https://github.com/ocornut/imgui |
| ImGuizmo | 2024-05-29 | MIT | https://github.com/CedricGuillemet/ImGuizmo |
| imgui-node-editor | 0.9.3 | MIT | https://github.com/thedmd/imgui-node-editor |
| EnTT | 3.16.0 | MIT | https://github.com/skypjack/entt |
| nlohmann/json | 3.12.0 | MIT | https://github.com/nlohmann/json |
| LuaJIT | 2026-03-04 | MIT | https://github.com/LuaJIT/LuaJIT |
| sol2 | 3.5.0 | MIT | https://github.com/ThePhD/sol2 |
| RmlUi | 6.2 | MIT | https://github.com/mikke89/RmlUi |
| FreeType | 2.13.3 | FreeType License (FTL) | https://www.freetype.org |
| pugixml | 1.15 | MIT | https://github.com/zeux/pugixml |
| Assimp | 6.0.4 | BSD-3-Clause | https://github.com/assimp/assimp |
| Jolt Physics | 5.5.0 | MIT | https://github.com/jrouwe/JoltPhysics |
| stb | 2024-07-29 | MIT or Public Domain | https://github.com/nothings/stb |
| miniaudio | 0.11.25 | Public Domain (Unlicense) or MIT-0 | https://github.com/mackron/miniaudio |
| Boost (asio, beast and dependencies) | 1.90.0 | BSL-1.0 | https://www.boost.org |
| OpenSSL | 3.6.1 | Apache-2.0 | https://www.openssl.org |
| zlib | 1.3.1 | Zlib | https://www.zlib.net |
| minizip | 1.3.1 | Zlib | https://github.com/madler/zlib |
| libpng | 1.6.55 | libpng-2.0 | https://github.com/pnggroup/libpng |
| brotli | 1.2.0 | MIT | https://github.com/google/brotli |
| bzip2 | 1.0.8 | bzip2-1.0.6 (BSD-style) | https://sourceware.org/bzip2/ |
| draco | 1.5.7 | Apache-2.0 | https://github.com/google/draco |
| poly2tri (jhasse) | 2023-12-27 | BSD-3-Clause | https://github.com/jhasse/poly2tri |
| Clipper (polyclipping) | 6.4.2 | BSL-1.0 | https://sourceforge.net/projects/polyclipping/ |
| kuba--/zip | 0.3.6 | MIT | https://github.com/kuba--/zip |
| utfcpp | 4.0.9 | BSL-1.0 | https://github.com/nemtrif/utfcpp |
| RapidJSON | 2025-02-26 | MIT | https://rapidjson.org |
| cgltf | 1.15 | MIT | https://github.com/jkuhlmann/cgltf |
| meshoptimizer | 1.0.1 | MIT | https://github.com/zeux/meshoptimizer |
| miniz | 3.1.1 | MIT | https://github.com/richgel999/miniz |
| tinyexr | 1.0.12 | BSD-3-Clause | https://github.com/syoyo/tinyexr |
| lodepng | 2021-12-04 | Zlib | https://github.com/lvandeve/lodepng |
| libsquish | 1.15 | MIT | https://sourceforge.net/projects/libsquish/ |
| mattiasgustavsson/libs | 2019-09-29 | MIT or Public Domain | https://github.com/mattiasgustavsson/libs |
| itlib | 1.12.1 | MIT | https://github.com/iboB/itlib |
| robin-hood-hashing | 3.11.5 | MIT | https://github.com/martinus/robin-hood-hashing |
| xatlas (vendored in `third_party/xatlas`, its `LICENSE` beside it) | jpcy/xatlas f700c77 (2022-07-25) | MIT | https://github.com/jpcy/xatlas |

### FreeType

Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved.

## Shader compiler (bgfx `shaderc`, used by the editor)

| Library | Version | License | Homepage |
|---|---|---|---|
| glslang | 16.2.0 | BSD-3-Clause and others (see its copyright file) | https://github.com/KhronosGroup/glslang |
| SPIRV-Cross | 1.4.341.0 | Apache-2.0 | https://github.com/KhronosGroup/SPIRV-Cross |
| SPIRV-Tools | 1.4.341.0 | Apache-2.0 | https://github.com/KhronosGroup/SPIRV-Tools |
| SPIRV-Headers | 1.4.341.0 | MIT (Khronos) | https://github.com/KhronosGroup/SPIRV-Headers |

## Services

### World and chat servers (C++)

| Library | Version | License | Homepage |
|---|---|---|---|
| MySQL Connector/C (libmysql) | 8.0.40 | GPL-2.0 with the Universal FOSS Exception 1.0 | https://github.com/mysql/mysql-server |
| Asio | 1.32.0 | BSL-1.0 | https://think-async.com/Asio/ |
| cpp-httplib | 0.38.0 | MIT | https://github.com/yhirose/cpp-httplib |
| jwt-cpp | 0.7.2 | MIT | https://github.com/Thalhammer/jwt-cpp |
| Quill | 11.1.0 | MIT | https://github.com/odygrd/quill |
| Recast & Detour | 1.6.0 | Zlib | https://github.com/recastnavigation/recastnavigation |
| LZ4 | 1.10.0 | BSD-2-Clause | https://github.com/lz4/lz4 |
| Zstandard | 1.5.7 | BSD-3-Clause (dual with GPL-2.0; BSD chosen) | https://facebook.github.io/zstd/ |
| OpenSSL, zlib, RapidJSON, EnTT, LuaJIT, sol2, stb, nlohmann/json | — | as listed above | — |

`libmysql` is statically linked into `kast_world` and `kast_chat`. Its Universal FOSS Exception allows this when Kast is distributed under an OSI-approved open-source license. Distributing those binaries under a non-FOSS license would bring in the GPL-2.0 terms.

### Auth service (Node.js, runtime dependencies)

| Package | License |
|---|---|
| bcrypt | MIT |
| express | MIT |
| helmet | MIT |
| jose | MIT |
| mysql2 | MIT |
| nodemailer | MIT-0 |
| zod | MIT |

Their transitive dependencies are MIT, ISC, BSD-2-Clause, BSD-3-Clause, Apache-2.0 and BlueOak-1.0.0. The only MPL-2.0 package (`lightningcss`) is a dev-only build tool and isn't shipped.

### Docker images (run as separate services, not linked)

`mysql:8.4` (GPL-2.0), `axllent/mailpit` (MIT, dev only), `node:24-bookworm-slim`, `ubuntu:24.04`.

## Assets

| Asset | Source | License |
|---|---|---|
| `editor/assets/fonts/Roboto-Regular.ttf` | Google (Roboto 2.138) | Apache-2.0 (`editor/assets/fonts/LICENSE.txt`) |
| `editor/assets/fonts/Roboto-Bold.ttf`, `Roboto-Italic.ttf`, `Roboto-BoldItalic.ttf` | Google (Roboto 2.138, `roboto-unhinted.zip` from the googlefonts/roboto v2.138 release) | Apache-2.0 (`editor/assets/fonts/LICENSE.txt`) |
| `tests/project/fonts/LatoLatin-Regular.ttf` (test font) | Łukasz Dziedzic (Lato, as shipped in RmlUi 6.2's samples) | OFL-1.1 (`tests/project/fonts/LatoLatin-OFL.txt`) |
| `editor/assets/fonts/MaterialSymbolsOutlined-Filled.ttf` | Google (Material Symbols 2.975) | Apache-2.0 (`editor/assets/fonts/LICENSE.txt`) |
| `tests/project/models/kenney/*` | Kenney, Animated Characters 2 (www.kenney.nl) | CC0-1.0 (`tests/project/models/kenney/License.txt`) |
| `samples/starter-mmo/models/female-128-anim.glb`, `tests/project/models/female-128-anim.glb` | Mesh2Motion (https://mesh2motion.org, https://github.com/Mesh2Motion/mesh2motion-assets) | CC0-1.0 |
| `samples/starter-mmo/models/grass-mesh.obj` | Kast authors | Kast's license |
| `samples/starter-mmo/textures/**` (icons, particles, checker, terrain, silhouette) | Unverified | Unverified |
