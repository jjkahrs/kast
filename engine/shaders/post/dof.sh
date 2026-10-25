// Shared by the depth of field passes (fs_dof_*.sc; post_process.h PostProcess).

#include "screen_space.sh"

uniform vec4 u_dof; // x: CoC scale (px, dofCocScale); y: focus distance (m); z: max CoC (px, full resolution)

// The thin lens' signed circle of confusion radius in full-resolution pixels at view depth z (m): < 0 nearer than the
// focus, > 0 farther, clamped to max CoC (circleOfConfusion in post_process.cpp).
float kastCoc(float z)
{
    z = max(abs(z), 1e-4);
    return clamp(u_dof.x * (z - u_dof.y) / z, -u_dof.z, u_dof.z);
}
