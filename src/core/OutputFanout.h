#pragma once

#include "DriftCorrector.h"
#include "SpscRingBuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// Per-output meter. A louder block jumps immediately; silence falls off so the picture does not stick.
inline float followOutputLevel(float previous, float blockPeak) noexcept
{
    const float peak = std::clamp(blockPeak, 0.0f, 1.0f);
    const float next = peak >= previous ? peak : previous * 0.94f;
    return std::clamp(next, 0.0f, 1.0f);
}

// One input block is copied into a private SPSC ring for every active output.
// Each output callback pulls from its own ring, so the devices run at once
// without sharing a lock. Slots are allocated up front (no growth on the audio threads).
class OutputFanout
{
public:
    static constexpr int kChannels = 2;
    static constexpr int kMaxOutputs = 16;

    OutputFanout();

    void setInputSampleRate(double sampleRate) noexcept;
    double getInputSampleRate() const noexcept;

    void setDriftEnabled(bool enabled) noexcept;
    void setSmoothingEnabled(bool enabled) noexcept;
    void setTargetLatencyMs(double ms) noexcept;

    // Message thread, while the slot is inactive.
    int activateOutput(double outputSampleRate) noexcept;
    void deactivateOutput(int slot) noexcept;
    void deactivateAll() noexcept;

    void setGain(int slot, float gain) noexcept;
    void setMuted(int slot, bool muted) noexcept;

    // Producer thread only. Interleaved stereo. Returns the shortest write across slots.
    int push(const float* interleavedStereo, int numFrames) noexcept;

    // Consumer thread for this slot only. Interleaved stereo.
    int pull(int slot, float* interleavedStereo, int numFrames) noexcept;

    // Consumer thread. Writes planar channel pointers. Missing channels are skipped.
    void pullPlanar(int slot, float* const* outputs, int numChannels, int numFrames) noexcept;

    int getFillFrames(int slot) const noexcept;
    float getLatencyMs(int slot) const noexcept;
    float getDriftMs(int slot) const noexcept;
    uint32_t getUnderrunCount(int slot) const noexcept;
    uint32_t getOverflowCount(int slot) const noexcept;
    float getOutputLevel(int slot) const noexcept;
    bool isActive(int slot) const noexcept;

private:
    struct Slot
    {
        Slot();
        SpscRingBuffer<float> ring;
        std::atomic<bool> active { false };
        std::atomic<float> gain { 1.0f };
        std::atomic<bool> muted { false };
        std::atomic<float> latencyMs { 0.0f };
        std::atomic<float> driftMs { 0.0f };
        std::atomic<float> outputLevel { 0.0f };
        std::atomic<uint32_t> underruns { 0 };
        std::atomic<uint32_t> overflows { 0 };
        double outputSampleRate = 48000.0;
        int targetFrames = 2400;
        bool smooth = false;
        float smoothed = 1.0f;
        float lastLeft = 0.0f;
        float lastRight = 0.0f;
        bool hasLast = false;
    };

    int targetFramesFor(double outputRate) const noexcept;
    void applyGain(Slot& slot, float* interleaved, int frames) noexcept;
    void noteOutputLevel(Slot& slot, const float* interleaved, int frames) noexcept;
    int discardFrames(Slot& slot, int frames) noexcept;
    int popFrames(Slot& slot, float* interleaved, int frames) noexcept;

    std::array<Slot, kMaxOutputs> slots;
    std::atomic<float> inputSampleRate { 48000.0f };
    std::atomic<bool> driftEnabled { true };
    std::atomic<bool> smoothingEnabled { true };
    std::atomic<float> targetLatencyMs { 50.0f };
    int maxPullFrames = 8192;
};
