#include <bgfx_compute.sh>

// VoxelGI bounce (voxel_gi.h VoxelGiRenderer::update): one thread per baked voxel. Gathers the mipped radiance volume
// s_voxelSource (the last pass's) with the preset's diffuse cones around the voxel's normal, as fs_mesh.sc
// kastVoxelDiffuse does (but a cone leaving the volume sees nothing: the environment isn't bounced), and writes its
// direct light + albedo * that gathered irradiance / pi into level 0 of the other radiance volume.
BUFFER_RO(b_voxels, vec4, 0); // as cs_voxelgi_inject.sc's
BUFFER_RO(b_direct, vec4, 1); // what cs_voxelgi_inject.sc wrote this frame
SAMPLER3D(s_voxelSource, 2);
IMAGE3D_WO(s_radiance, rgba16f, 3);

#define MAX_CONES 9 // kMaxVoxelGiCones in voxel_gi.h

// [0] xyz: the grid's min corner, w: voxel size (m); [1] x: voxels; [2] xyz: 1 / the grid's extent (m), w: its last
// mip; [3] x: cones, y: their aperture (tan of the half angle), z: step scale, w: steps per cone at most
uniform vec4 u_voxel[4];
uniform vec4 u_voxelCones[MAX_CONES]; // voxelGiCones in voxel_gi.h: xyz in the tangent frame (z: the normal), w weight

// As fs_mesh.sc kastVoxelCone.
vec4 voxelCone(vec3 p, vec3 n, vec3 dir, float aperture, float maxSteps)
{
    float voxel = u_voxel[0].w;
    vec4 sum = vec4_splat(0.0);
    float t = voxel;
    for (int i = 0; i < 64; ++i) {
        if (float(i) >= maxSteps || sum.a >= 0.95)
            break;
        float diameter = max(voxel, 2.0 * aperture * t);
        vec3 uvw = (p + dir * t + n * diameter - u_voxel[0].xyz) * u_voxel[2].xyz;
        if (any(lessThan(uvw, vec3_splat(0.0))) || any(greaterThan(uvw, vec3_splat(1.0))))
            break;
        float lod = min(log2(diameter / voxel), u_voxel[2].w);
        vec4 s = texture3DLod(s_voxelSource, uvw, lod);
        float a = min(s.a * exp2(lod), 1.0); // as fs_mesh.sc kastVoxelCone
        s = vec4(s.rgb * (a / max(s.a, 1e-4)), a);
        sum += (1.0 - sum.a) * s;
        t += diameter * u_voxel[3].z;
    }
    return sum;
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (float(i) >= u_voxel[1].x)
        return;
    vec4 cell = b_voxels[i * 4u];
    vec3 albedo = b_voxels[i * 4u + 1u].xyz;
    vec3 n = b_voxels[i * 4u + 2u].xyz;
    vec4 direct = b_direct[i];
    float voxel = u_voxel[0].w;
    vec3 gathered = vec3_splat(0.0);
    if (dot(n, n) > 0.25) { // a voxel whose surfaces' normals cancel out gathers nothing
        vec3 t = normalize(cross(abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
        vec3 b = cross(n, t);
        vec3 origin = u_voxel[0].xyz + (cell.xyz + 0.5) * voxel + n * voxel;
        for (int c = 0; c < MAX_CONES; ++c) {
            if (float(c) >= u_voxel[3].x)
                break;
            vec4 cone = u_voxelCones[c];
            gathered += cone.w * voxelCone(origin, n, t * cone.x + b * cone.y + n * cone.z, u_voxel[3].y, u_voxel[3].w).rgb;
        }
    }
    imageStore(s_radiance, ivec3(cell.xyz), vec4(direct.rgb + albedo * gathered * cell.w, direct.a));
}
