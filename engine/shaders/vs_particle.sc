$input a_position, i_data0, i_data1, i_data2
$output v_color0, v_texcoord0, v_worldPos

#include <bgfx_shader.sh>

// One camera-facing quad per particle slot (cs_particle_sim.sc): a_position is the corner (-1..1), i_data0..2 the
// particle; u_model[0] takes it to world space (identity for world space emitters). Dead slots collapse to a point.
uniform vec4 u_particleStart;  // rgb: linear color, a: alpha at birth
uniform vec4 u_particleEnd;    // the same at death
uniform vec4 u_particleParams; // x: end size multiplier

void main()
{
    float age = i_data0.w;
    float lifetime = i_data1.w;
    float t = saturate(age / max(lifetime, 0.0001));
    vec3 worldPos = mul(u_model[0], vec4(i_data0.xyz, 1.0)).xyz;
    vec4 viewPos = mul(u_view, vec4(worldPos, 1.0));
    float size = age < lifetime ? i_data2.x * mix(1.0, u_particleParams.x, t) : 0.0;
    viewPos.xy += a_position.xy * (0.5 * size);
    gl_Position = mul(u_proj, viewPos);
    v_color0 = mix(u_particleStart, u_particleEnd, t);
    v_texcoord0 = vec2(a_position.x, -a_position.y) * 0.5 + 0.5; // (0, 0) is the image's top-left
    v_worldPos = worldPos;
}
