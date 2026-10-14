$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex0, 0); // HDR scene
SAMPLER2D(s_tex1, 1); // bloom (upsampled level 0), when u_post.z > 0
uniform vec4 u_post;   // x: exposure scale 2^EV; y: 1 = ACES, else clamp; z: bloom intensity (0 = off); w: 1 = grading
uniform vec4 u_grade;  // rgb: color filter * white balance; w: contrast
uniform vec4 u_grade2; // x: saturation; y: vignette intensity (0 = off); z: vignette smoothness

#define MID_GREY 0.18

// ACES filmic fit (Narkowicz 2015), as acesTonemap in post_process.cpp.
vec3 aces(vec3 x)
{
    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}

// Linear to sRGB (piecewise IEC 61966-2-1), as srgbToLinear's inverse in post_process.cpp: targets are plain UNORM.
vec3 toSrgb(vec3 c)
{
    c = saturate(c);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3_splat(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3_splat(0.0031308), c));
}

// bloom -> exposure -> tonemap -> color grading -> vignette -> sRGB. With every effect off: toSrgb(saturate(scene)),
// what fs_mesh.sc wrote before post-processing.
void main()
{
    vec4 scene = texture2D(s_tex0, v_texcoord0);
    vec3 c = scene.rgb;
    if (u_post.z > 0.0)
        c += texture2D(s_tex1, v_texcoord0).rgb * u_post.z;
    c *= u_post.x;
    c = u_post.y > 0.5 ? aces(c) : saturate(c);
    if (u_post.w > 0.5) {
        // Filter and white balance multiply; contrast is a power curve pivoting on mid grey; saturation mixes
        // with the Rec. 709 luminance.
        c *= u_grade.rgb;
        c = pow(max(c, vec3_splat(1e-6)) / MID_GREY, vec3_splat(u_grade.w)) * MID_GREY;
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = saturate(mix(vec3_splat(l), c, u_grade2.x));
    }
    if (u_grade2.y > 0.0) {
        // 0 at the center, 1 at the corners; darkening ramps in over the outer `smoothness` of that.
        float d = length(v_texcoord0 - 0.5) * 1.41421356;
        c *= 1.0 - u_grade2.y * smoothstep(1.0 - u_grade2.z, 1.0, d);
    }
    gl_FragColor = vec4(toSrgb(c), scene.a);
}
