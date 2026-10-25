$input v_texcoord0

#include <bgfx_shader.sh>
#include "../volumetric_fog.sh"

// Volumetric fog over the opaque scene (volumetric_fog.h VolumetricFog::update): the fog in front of each pixel's depth
// (the sky's: all of the grid's) as rgb in-scattered light and a transmittance, blended ONE, SRC_ALPHA: color *
// transmittance + in-scattered. Both times 1 +- 1/255 of noise, so the fog's wide, smooth gradients don't band once
// encoded to 8 bits.
SAMPLER2D(s_tex0, 0); // the scene's depth
SAMPLER2D(s_tex1, 1); // the integrated atlas (cs_volfog_integrate.sc)

uniform vec4 u_volFog[2]; // VolumetricFog::drawParams: [1] y maxDistance

void main()
{
    float depth = texture2D(s_tex0, v_texcoord0).r;
    vec4 p = mul(u_invProj, vec4(0.0, 0.0, depth, 1.0)); // as fs_mesh.sc linearDepth
    vec4 fog = kastFroxelFog(s_tex1, v_texcoord0, -p.z / p.w, u_volFog[1].y, gl_FragCoord.xy);
    gl_FragColor = fog * (1.0 + (kastFroxelNoise(gl_FragCoord.xy + vec2(91.0, 13.0)) - 0.5) * (2.0 / 255.0));
}
