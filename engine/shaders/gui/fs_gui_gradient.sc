$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

#define MAX_STOPS 16
uniform vec4 u_gradient;              // x: 0 linear, 1 radial, 2 conic; y: 1 repeating; z: stop count
uniform vec4 u_shape;                 // xy: start point / center; zw: linear: vector to the end point, radial: 1 / radius,
                                      // conic: unit vector of the start angle
uniform vec4 u_stopColor[MAX_STOPS];  // premultiplied
uniform vec4 u_stopPos[MAX_STOPS];    // x: 0 at the start, 1 at the end

// RmlUi's linear-, radial- and conic-gradient decorators; v_texcoord0 is the element-local pixel position.
void main()
{
    vec2 v = v_texcoord0 - u_shape.xy;
    float t;
    if (u_gradient.x < 0.5)
        t = dot(u_shape.zw, v) / dot(u_shape.zw, u_shape.zw);
    else if (u_gradient.x < 1.5)
        t = length(u_shape.zw * v);
    else {
        vec2 r = vec2(u_shape.z * v.x + u_shape.w * v.y, u_shape.z * v.y - u_shape.w * v.x); // rotated to the start angle
        t = 0.5 + atan2(-r.x, r.y) / (2.0 * 3.14159265);
    }
    int count = int(u_gradient.z);
    if (u_gradient.y > 0.5) {
        float t0 = u_stopPos[0].x;
        t = t0 + mod(t - t0, max(u_stopPos[count - 1].x - t0, 0.00001));
    }
    vec4 color = u_stopColor[0]; // linear between stops, as in CSS
    for (int i = 1; i < MAX_STOPS; ++i) {
        if (i >= count)
            break;
        float p0 = u_stopPos[i - 1].x;
        color = mix(color, u_stopColor[i], clamp((t - p0) / max(u_stopPos[i].x - p0, 0.00001), 0.0, 1.0));
    }
    gl_FragColor = color * v_color0;
}
