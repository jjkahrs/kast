#pragma once

#include <cmath>

namespace kast {

// u_time.x (fs_mesh.sc) wraps to 0 every this many seconds, so it keeps sub-millisecond float precision.
constexpr double kShaderTimePeriod = 3600.0;

// The u_time clock (Renderer::setTime). The editor advances it every frame in edit mode and Play (by --fixed-dt when
// given) but not while Play is paused; the player every frame.
class ShaderClock {
public:
    void advance(double dt) { seconds_ = std::fmod(seconds_ + dt, kShaderTimePeriod); }
    float seconds() const { return static_cast<float>(seconds_); }

private:
    double seconds_ = 0.0; // double: whole-period sums of float steps stay exact enough to wrap where expected
};

} // namespace kast
