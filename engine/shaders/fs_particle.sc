$input v_color0, v_texcoord0, v_worldPos

#include <bgfx_shader.sh>
#include "volumetric_fog.sh"

SAMPLER2D(s_particle, 0); // sRGB, sampled as linear
SAMPLER2D(s_volFog, 1);   // volumetric fog's atlas (volumetric_fog.h), while u_volFog[0].w is 1

uniform vec4 u_particleParams; // y: 1 textured, else a soft circle; z: 1 additive (src * alpha + dst), else alpha blend
uniform vec4 u_cameraPos;      // as fs_mesh.sc
uniform vec4 u_fogColor;
uniform vec4 u_fogParams;
uniform vec4 u_volFog[2]; // as fs_mesh.sc's

// Unlit, in linear HDR.
void main()
{
    vec4 color = v_color0;
    if (u_particleParams.y > 0.5) {
        color *= texture2D(s_particle, v_texcoord0);
    } else {
        color.a *= smoothstep(1.0, 0.0, length(v_texcoord0 * 2.0 - 1.0));
    }
    // fs_mesh.sc's fog: alpha particles fade to the fog color, additive ones fade out (adding fog would brighten).
    if (u_fogColor.w > 0.5) {
        float fd = u_fogParams.x * max(length(u_cameraPos.xyz - v_worldPos) - u_fogParams.y, 0.0);
        float fog = 1.0 - exp(-fd * fd);
        color.rgb = u_particleParams.z > 0.5 ? color.rgb * (1.0 - fog) : mix(color.rgb, u_fogColor.rgb, fog);
    }
    // And fs_mesh.sc's kastVolumetricFog.
    if (u_volFog[0].w > 0.5) {
        vec4 volume = kastFroxelFog(s_volFog, (gl_FragCoord.xy - u_viewRect.xy) * u_viewTexel.xy,
                                    dot(v_worldPos - u_cameraPos.xyz, u_volFog[0].xyz), u_volFog[1].y, gl_FragCoord.xy);
        color.rgb = u_particleParams.z > 0.5 ? color.rgb * volume.a : color.rgb * volume.a + volume.rgb;
    }
    gl_FragColor = color;
}
