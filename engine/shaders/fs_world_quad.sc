$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_worldQuad, 0); // the World UI atlas: a GUI's premultiplied alpha, sRGB values

// Unlit, in linear HDR, premultiplied (blended ONE, INV_SRC_ALPHA).
void main()
{
    vec4 color = texture2D(s_worldQuad, v_texcoord0);
    if (color.a < 1.0 / 255.0)
        discard; // nothing to show: no depth written there either
    vec3 c = color.rgb / max(color.a, 0.0001);
    vec3 lin = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3_splat(2.4)), step(vec3_splat(0.04045), c)); // sRGB decode
    gl_FragColor = vec4(lin * color.a, color.a);
}
