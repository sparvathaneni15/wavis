#pragma once

#include <algorithm>
#include <cmath>

// Buffer-level drift plan for one output clock against the input clock.
// skipFrames / duplicateFrames correct fill level without a continuous pitch shift.
// resampleRatio converts input rate to output rate (1 when the rates match).
struct DriftPlan
{
    int skipFrames = 0;
    int duplicateFrames = 0;
    float resampleRatio = 1.0f;
};

inline DriftPlan planDrift(int fillFrames,
                           int targetFrames,
                           bool enabled,
                           double inputRate,
                           double outputRate) noexcept
{
    DriftPlan plan;
    if (inputRate > 0.0 && outputRate > 0.0)
        plan.resampleRatio = static_cast<float>(inputRate / outputRate);

    if (!enabled || targetFrames <= 0)
        return plan;

    const int deadband = std::max(1, targetFrames / 5);
    const int error = fillFrames - targetFrames;

    if (error > deadband)
        plan.skipFrames = std::clamp((error - deadband) / 32, 1, 64);
    else if (-error > deadband)
        plan.duplicateFrames = std::clamp(((-error) - deadband) / 64, 1, 8);

    return plan;
}
