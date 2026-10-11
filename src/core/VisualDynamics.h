#pragma once

#include <algorithm>
#include <cmath>

// Visual-only dynamics. This does not run on the audio outputs.
// A slow peak reference sets makeup gain (capped, so quiet material is not lifted
// to the target). A soft knee then bends anything above `knee` toward `ceiling`
// without ever crossing it. The page applies the same formulas; see web/src/main.js.

struct VisualDynamics
{
    float target = 0.46f;
    float knee = 0.62f;
    float ceiling = 1.0f;
    float maxMakeup = 2.2f;
    float peakAttack = 10.0f;
    float peakRelease = 0.28f;
    float reference = 0.2f;
};

inline float visualSoftKnee(float x, float knee, float ceiling)
{
    if (!(x > 0.0f))
        return 0.0f;
    if (!(ceiling > knee))
        return std::min(x, std::max(ceiling, 0.0f));
    if (x <= knee)
        return x;
    const float span = ceiling - knee;
    const float bent = knee + span * std::tanh((x - knee) / span);
    return std::min(bent, std::nextafter(ceiling, 0.0f));
}

// One sample of auto-gain plus the soft knee. `dt` is seconds.
inline float visualCompress(float input, VisualDynamics& state, float dt)
{
    const float x = std::max(0.0f, input);
    const float step = std::max(0.0f, dt);
    const float rate = x > state.reference ? state.peakAttack : state.peakRelease;
    const float mix = 1.0f - std::exp(-rate * step);
    state.reference += (x - state.reference) * mix;
    state.reference = std::max(state.reference, 1.0e-4f);
    const float makeup = std::min(state.maxMakeup, state.target / state.reference);
    const float bent = visualSoftKnee(x * makeup, state.knee, state.ceiling);
    if (state.ceiling > 0.0f)
        return std::min(bent, std::nextafter(state.ceiling, 0.0f));
    return 0.0f;
}
