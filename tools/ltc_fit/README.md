# LTC fit (`kast_ltc_fit`)

Fits the lookup table Kast's area lights (`AreaLight`, see [Coordinates and scenes](../../docs/engine/coordinates-and-scenes.html))
shade their GGX specular with, and writes it as `engine/shaders/ltc_table.sh`. The table is checked in; the build never
runs the tool. Run it again only after changing the fit or the BRDF in `engine/shaders/fs_mesh.sc` `shade()`.

## Build and run

`kast_ltc_fit` builds with everything else (`build.bat`), next to the editor:

```bat
build\relwithdebinfo\kast_ltc_fit.exe engine\shaders\ltc_table.sh   :: refit and write the table (a few seconds)
build\relwithdebinfo\kast_ltc_fit.exe --verify engine\shaders\ltc_table.sh   :: exit 1 if the checked-in table differs
build\relwithdebinfo\kast_ltc_fit.exe --check          :: accuracy against brute force (below), 32x32, 16x16 and 8x8
build\relwithdebinfo\kast_ltc_fit.exe --check-verbose  :: the same, every case
```

The fit is deterministic (fixed stratified samples, no randomness), so `--verify` is a ctest (`ltc_table_up_to_date`).
After regenerating, rebuild: `fs_mesh.sc` and `renderer.cpp` include the file at compile time.

## What it fits

Linearly Transformed Cosines (Heitz, Dupuy, Hill, Neubelt, *Real-Time Polygonal-Light Shading with Linearly Transformed
Cosines*, SIGGRAPH 2016): for each (roughness, view angle) a 3x3 matrix `M` such that the clamped cosine distribution
transformed by `M` matches the BRDF lobe. The integral of the lobe over a polygon is then the clamped cosine's
integral over the polygon transformed by `M^-1`, which has a closed form (the shader's `ltcIntegrate`).

- **BRDF**: exactly `shade()`'s specular with Fresnel 1: GGX `D` with `alpha = roughness^2`, Smith Schlick-GGX `G` with
  `k = (roughness + 1)^2 / 8`, times `N.L`. Roughness is floored at 0.04 as in the shader.
- **Grid**: 16 x 16 (`LTC_SIZE` in `fs_mesh.sc`, `kLtcSize` in `renderer.h`). Columns: roughness `a / 15`; rows:
  `sqrt(1 - N.V) = t / 15` (denser toward grazing; the last row is held at 89.95 degrees).
- **Per entry**: the BRDF's albedo split by Schlick's Fresnel weight `w = (1 - V.H)^5` into `scale = integral of
  BRDF * N.L * (1 - w)` and `bias = integral of BRDF * N.L * w` (the shader's specular is `(F0 * scale + bias) * LTC
  integral`), and its average direction, from 256 x 256 GGX importance samples. `M` is that direction's frame times
  `[[m00, 0, m02], [0, m11, 0], [0, 0, 1]]`, fit by Nelder-Mead on the error `|f - g|^3 / (pdf_f + pdf_g)` between the
  normalized lobe `f` and the LTC `g`, over 32 x 32 stratified samples drawn from each (multiple importance sampling).
  Normal incidence is fit isotropic (`m00 = m11`, `m02 = 0`); each view angle starts from the last one's fit. One
  thread per roughness column.
- **Stored**: two `vec4` per entry, `M^-1` scaled so its `m11` is 1 (the integral doesn't depend on the scale):
  `(m00, m02, m20, m22)`, then `(scale, bias, 0, 0)`. Float literals both HLSL and C++ parse (no leading zeros in the
  exponent: bgfx's shader preprocessor would read `e-08` as octal).

The table is Kast's own fit of Kast's own BRDF; no published table was copied.

## Why a shader constant, and why 16 x 16

Every one of the mesh shader's 16 sampler stages is taken (material maps, shadow maps, `.kshader` texture params,
`SCREEN_TEXTURE` / `DEPTH_TEXTURE`; see [Shaders](../../docs/engine/shaders.html#kshader-samplers)), so the table is not
a texture: `fs_mesh.sc` declares it as a `static const vec4` array (an immediate constant buffer) and interpolates it
bilinearly itself (`ltcLookup`). The depth-only shadow pass leaves it out.

The Direct3D compiler spends time on every constant: the full 32 x 32 table made each `.kshader` compile about 1.5 s
slower (the hot reload test allows 2 s); 16 x 16 costs about 0.3 s. `--check` shows the smaller table is as accurate
for the tested cases (the error comes from the LTC shape itself, not from the grid):

```
32x32 table, 96 cases: error / albedo mean 1.17%, max 16.04%; relative error (37 cases with a tenth of the albedo or more) mean 5.6%, max 61.1%
16x16 table, 96 cases: error / albedo mean 1.11%, max 14.53%; relative error (37 cases with a tenth of the albedo or more) mean 5.0%, max 55.5%
8x8 table, 96 cases: error / albedo mean 1.09%, max 9.38%; relative error (37 cases with a tenth of the albedo or more) mean 4.2%, max 36.9%
```

The cases: roughness 0.1 to 1, view angles 0 to 80 degrees, four rects (overhead, beside and tilted, large and near,
low toward grazing), each against the BRDF integrated over the rect with 256 x 256 area samples. "error / albedo" is the
error as a share of what a rect filling the hemisphere would reflect. The largest errors are at an 80 degree view of
low-roughness lobes, where the LTC shape fits the stretched GGX lobe least well; at normal and moderate angles the
specular is within a few percent. Roughness below 0.1 is not in `--check`: its highlight is too sharp for the brute
force sampling.
