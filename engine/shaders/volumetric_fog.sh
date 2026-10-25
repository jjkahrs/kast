// Volumetric fog's froxel grid (volumetric_fog.h VolumetricFog), shared by its compute passes (cs_volfog_*.sc), its
// composite (post/fs_volfog_composite.sc), the blended meshes (fs_mesh.sc) and particles (fs_particle.sc).
#define FROXELS_X 160.0     // kFroxelsX in volumetric_fog.h
#define FROXELS_Y 90.0      // kFroxelsY
#define FROXEL_SLICES 64.0  // kFroxelSlices
#define FROXEL_NEAR 0.5     // kFroxelNear (m)
#ifndef PI
#define PI 3.14159265
#endif

// The continuous slice (0 at FROXEL_NEAR, FROXEL_SLICES at farZ) of view depth d: froxelSlice in volumetric_fog.cpp.
float kastFroxelSlice(float d, float farZ)
{
    return FROXEL_SLICES * log(max(d, 1e-4) / FROXEL_NEAR) / log(farZ / FROXEL_NEAR);
}

// Its inverse, the view depth of slice s: froxelSliceDepth in volumetric_fog.cpp.
float kastFroxelDepth(float s, float farZ)
{
    return FROXEL_NEAR * pow(farZ / FROXEL_NEAR, s / FROXEL_SLICES);
}

// Henyey-Greenstein (henyeyGreenstein in volumetric_fog.cpp): per steradian, c the cosine between the light's travel
// and the direction toward the eye.
float kastHenyeyGreenstein(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-6), 1.5));
}

#if BGFX_SHADER_TYPE_COMPUTE
// The compute passes' frame (VolumetricFog::update): [0] xyz the eye, w maxDistance (farZ); [1] xyz the ray per meter
// of view depth through the image's first texel corner, w this frame's slice jitter; [2] xyz the ray's change across
// the image's width, w FogVolumes; [3] xyz down its height, w lights; [4] x density, y heightFalloff, z baseHeight, w
// anisotropy; [5] rgb albedo, w the sun's cascades; [6] rgb ambient * ambientIntensity, w lightIntensity; [7] xyz the
// last frame's eye, w the history's weight (0: none); [8] xyz the last frame's forward; [9] xy this frame's jitter
// across the froxels (in froxels), z 1 to light each froxel at 4 points (temporalBlend 0: no jitter to average).
uniform vec4 u_volFogGrid[10];

// The ray per meter of view depth through screen uv.
vec3 kastFroxelRay(vec2 uv)
{
    return u_volFogGrid[1].xyz + u_volFogGrid[2].xyz * uv.x + u_volFogGrid[3].xyz * uv.y;
}

// The world position of froxel cell's center moved by offset (in froxels and slices) and the frame's jitter.
vec3 kastFroxelPositionAt(ivec3 cell, vec3 offset)
{
    vec2 uv = (vec2(cell.xy) + 0.5 + offset.xy + u_volFogGrid[9].xy) / vec2(FROXELS_X, FROXELS_Y);
    float d = kastFroxelDepth(float(cell.z) + 0.5 + offset.z + u_volFogGrid[1].w, u_volFogGrid[0].w);
    return u_volFogGrid[0].xyz + kastFroxelRay(uv) * d;
}

vec3 kastFroxelPosition(ivec3 cell)
{
    return kastFroxelPositionAt(cell, vec3_splat(0.0));
}
#else
// Interleaved gradient noise (Jimenez 2014) at pixel p: 0..1, decorrelated between neighbours.
float kastFroxelNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The fog between the eye and view depth d at screen uv ((0, 0) the image's first row in memory) from the atlas
// cs_volfog_integrate.sc writes (texel slice i: up to slice i + 1's depth): rgb the light it scatters toward the eye,
// a its transmittance. Nearer than slice 1, from none at the eye; past farZ, all of the grid's. Dithered per pixel
// (window pixel position) by up to half a froxel across and half a slice in depth, so the grid's steps (sharp shadow
// edges in the fog, most of all without temporalBlend's jitter) become fine noise instead of blocks.
vec4 kastFroxelFog(sampler2D atlas, vec2 uv, float d, float farZ, vec2 pixel)
{
    float n = kastFroxelNoise(pixel);
    float m = kastFroxelNoise(pixel + vec2(47.0, 17.0));
    uv += (vec2(n, m) - 0.5) / vec2(FROXELS_X, FROXELS_Y);
    float s = kastFroxelSlice(d, farZ) - 1.0 + fract(n + m * 0.618034) - 0.5;
    float x = clamp(uv.x * FROXELS_X, 0.5, FROXELS_X - 0.5); // within the slice's columns
    float s0 = clamp(floor(s), 0.0, FROXEL_SLICES - 1.0);
    float s1 = min(s0 + 1.0, FROXEL_SLICES - 1.0);
    vec4 a = texture2DLod(atlas, vec2((s0 * FROXELS_X + x) / (FROXELS_X * FROXEL_SLICES), uv.y), 0.0);
    vec4 b = texture2DLod(atlas, vec2((s1 * FROXELS_X + x) / (FROXELS_X * FROXEL_SLICES), uv.y), 0.0);
    if (s < 0.0)
        return mix(vec4(0.0, 0.0, 0.0, 1.0), a, saturate(d / kastFroxelDepth(1.0, farZ)));
    return mix(a, b, saturate(s - s0));
}
#endif
