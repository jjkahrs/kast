// Shared by the SSAO passes (fs_ssao_*.sc; post_process.h PostProcess).

#include "screen_space.sh"

uniform vec4 u_ssao; // x: radius (m); y: slice count; z: intensity; w: power
