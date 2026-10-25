$input v_texcoord0

#include <bgfx_shader.sh>
#include "dof.sh"

SAMPLER2D(s_tex0, 0); // the scene's HDR color
SAMPLER2D(s_tex1, 1); // the scene's depth
SAMPLER2D(s_tex2, 2); // fs_dof_gather.sc's far layer (half resolution)
SAMPLER2D(s_tex3, 3); // fs_dof_gather.sc's near layer (half resolution: a coverage)

// Full resolution: the sharp color blends to the (bilinear upsampled) far layer as this pixel's own far CoC grows past
// half a pixel, then the near layer goes over it by its coverage.
void main()
{
    vec2 uv = v_texcoord0;
    vec3 color = texture2DLod(s_tex0, uv, 0.0).rgb;
    float coc = kastCoc(kastViewPos(uv, texture2DLod(s_tex1, uv, 0.0).x).z);
    color = mix(color, texture2DLod(s_tex2, uv, 0.0).rgb, saturate((coc - 0.5) / 1.5));
    vec4 near = texture2DLod(s_tex3, uv, 0.0);
    gl_FragColor = vec4(mix(color, near.rgb, near.a), 1.0);
}
