#include "hexcaster/param_smoother.h"
#include <cmath>

namespace hexcaster {

void ParamSmoother::prepare(float sampleRate, float smoothingMs)
{
    // EMA coefficient: coeff_ close to 1.0 = very slow smoothing
    // tau = smoothingMs / 1000, alpha = 1 - exp(-1 / (tau * sampleRate))
    // coeff_ = 1 - alpha = exp(-1 / (tau * sampleRate))
    if (sampleRate > 0.f && smoothingMs > 0.f) {
        const float tau = smoothingMs / 1000.f;
        coeff_ = std::exp(-1.f / (tau * sampleRate));
    } else {
        coeff_ = 0.f; // instant snap
    }
}

void ParamSmoother::setTarget(float target)
{
    target_ = target;
}

float ParamSmoother::next()
{
    current_ = coeff_ * current_ + (1.f - coeff_) * target_;
    return current_;
}

void ParamSmoother::snap(float value)
{
    current_ = value;
    target_  = value;
}

} // namespace hexcaster
