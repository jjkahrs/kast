$input v_worldPos

#include <bgfx_shader.sh>

SAMPLERCUBE(s_sky, 0);
uniform vec4 u_skyTop;     // xyz: linear color looking straight up; w: 1 = cubemap (s_sky), 0 = gradient
uniform vec4 u_skyHorizon; // xyz: at eye level
uniform vec4 u_skyBottom;  // xyz: straight down

// Unlit and unfogged: the sky is background only.
void main()
{
    vec3 dir = normalize(v_worldPos);
    vec3 color;
    if (u_skyTop.w > 0.5) {
        // Kast is right-handed, D3D cubes left-handed: flipping z shows every face unmirrored (sky.cpp swaps pz / nz).
        color = textureCube(s_sky, vec3(dir.x, dir.y, -dir.z)).rgb;
    } else {
        // bottom -> horizon -> top by elevation; sqrt keeps the horizon band narrow (proceduralSkyColor in sky.h).
        color = mix(u_skyHorizon.xyz, dir.y >= 0.0 ? u_skyTop.xyz : u_skyBottom.xyz, sqrt(abs(dir.y)));
    }
    gl_FragColor = vec4(color, 1.0);
}
