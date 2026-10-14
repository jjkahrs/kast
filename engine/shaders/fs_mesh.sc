$input v_normal, v_worldPos

#include <bgfx_shader.sh>

#define MAX_POINT_LIGHTS 8 // kMaxPointLights in renderer.h

uniform vec4 u_baseColor;
uniform vec4 u_lightDir;   // xyz: world direction the light travels; w: 1 if the scene has a light, else 0
uniform vec4 u_lightColor; // rgb: light color * intensity
uniform vec4 u_ambient;    // rgb: scene ambient color * intensity
uniform vec4 u_pointPosRange[MAX_POINT_LIGHTS]; // xyz: world position; w: range (unused slots: 1)
uniform vec4 u_pointColor[MAX_POINT_LIGHTS];    // rgb: color * intensity (unused slots: 0)

// Windowed inverse square, as pointLightFalloff in renderer.cpp: saturate(1 - (d/r)^4)^2 / (d^2 + 1).
float falloff(float d, float range)
{
    float ratio = d / range;
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window / (d * d + 1.0);
}

void main()
{
    vec3 normal = normalize(v_normal);
    vec3 light = u_lightColor.rgb * max(dot(normal, -u_lightDir.xyz), 0.0) * u_lightDir.w;
    for (int i = 0; i < MAX_POINT_LIGHTS; ++i) {
        vec3 toLight = u_pointPosRange[i].xyz - v_worldPos;
        float d = length(toLight);
        float lambert = max(dot(normal, toLight / max(d, 1e-4)), 0.0);
        light += u_pointColor[i].rgb * lambert * falloff(d, u_pointPosRange[i].w);
    }
    gl_FragColor = vec4(u_baseColor.rgb * (u_ambient.rgb + light), 1.0);
}
