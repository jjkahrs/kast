$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_baseColor, 0); // as fs_mesh.sc

uniform vec4 u_baseColor; // as fs_mesh.sc
uniform vec4 u_material;  // z: alpha cutoff (-1 = none), as fs_mesh.sc

// Shadow map depth pass: only depth is written; alpha-mask materials cut their shadow out as they cut themselves.
void main()
{
    if (u_material.z >= 0.0 && (u_baseColor * texture2D(s_baseColor, v_texcoord0)).a < u_material.z)
        discard;
    gl_FragColor = vec4_splat(0.0);
}
