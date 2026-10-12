$input v_normal

#include <bgfx_shader.sh>

uniform vec4 u_baseColor;
uniform vec4 u_lightDir;   // xyz: world direction the light travels; w: 1 if the scene has a light, else 0
uniform vec4 u_lightColor; // rgb: light color * intensity

#define AMBIENT vec3(0.2, 0.2, 0.2)

void main()
{
    vec3 normal = normalize(v_normal);
    float lambert = max(dot(normal, -u_lightDir.xyz), 0.0) * u_lightDir.w;
    gl_FragColor = vec4(u_baseColor.rgb * (AMBIENT + u_lightColor.rgb * lambert), 1.0);
}
