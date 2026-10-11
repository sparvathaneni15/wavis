#include "OutputFanout.h"

#include <algorithm>
#include <cmath>
#include <cstring>

OutputFanout::Slot::Slot() : ring(131072) {}

OutputFanout::OutputFanout() = default;

void OutputFanout::setInputSampleRate(double sampleRate) noexcept
{
    if (sampleRate > 0.0)
        inputSampleRate.store(static_cast<float>(sampleRate), std::memory_order_relaxed);
}

double OutputFanout::getInputSampleRate() const noexcept
{
    return inputSampleRate.load(std::memory_order_relaxed);
}

void OutputFanout::setDriftEnabled(bool enabled) noexcept
{
    driftEnabled.store(enabled, std::memory_order_relaxed);
}

void OutputFanout::setSmoothingEnabled(bool enabled) noexcept
{
    smoothingEnabled.store(enabled, std::memory_order_relaxed);
}

void OutputFanout::setTargetLatencyMs(double ms) noexcept
{
    targetLatencyMs.store(static_cast<float>(std::clamp(ms, 10.0, 250.0)), std::memory_order_relaxed);
}

int OutputFanout::targetFramesFor(double outputRate) const noexcept
{
    const double rate = outputRate > 0.0 ? outputRate : static_cast<double>(getInputSampleRate());
    const double ms = targetLatencyMs.load(std::memory_order_relaxed);
    return std::max(32, static_cast<int>(rate * (ms / 1000.0)));
}

int OutputFanout::activateOutput(double outputSampleRate) noexcept
{
    for (int i = 0; i < kMaxOutputs; ++i)
    {
        auto& slot = slots[static_cast<size_t>(i)];
        if (slot.active.load(std::memory_order_acquire))
            continue;

        slot.ring.reset();
        slot.outputSampleRate = outputSampleRate > 0.0 ? outputSampleRate : getInputSampleRate();
        slot.targetFrames = targetFramesFor(slot.outputSampleRate);
        slot.smoothed = slot.muted.load(std::memory_order_relaxed) ? 0.0f : slot.gain.load(std::memory_order_relaxed);
        slot.hasLast = false;
        slot.lastLeft = 0.0f;
        slot.lastRight = 0.0f;
        slot.underruns.store(0, std::memory_order_relaxed);
        slot.overflows.store(0, std::memory_order_relaxed);
        slot.latencyMs.store(0.0f, std::memory_order_relaxed);
        slot.driftMs.store(0.0f, std::memory_order_relaxed);
        slot.outputLevel.store(0.0f, std::memory_order_relaxed);
        slot.active.store(true, std::memory_order_release);
        return i;
    }

    return -1;
}

void OutputFanout::deactivateOutput(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return;
    auto& s = slots[static_cast<size_t>(slot)];
    s.active.store(false, std::memory_order_release);
    s.outputLevel.store(0.0f, std::memory_order_relaxed);
    s.ring.reset();
}

void OutputFanout::deactivateAll() noexcept
{
    for (int i = 0; i < kMaxOutputs; ++i)
        deactivateOutput(i);
}

void OutputFanout::setGain(int slot, float gain) noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return;
    slots[static_cast<size_t>(slot)].gain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed);
}

void OutputFanout::setMuted(int slot, bool muted) noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return;
    slots[static_cast<size_t>(slot)].muted.store(muted, std::memory_order_relaxed);
}

int OutputFanout::push(const float* interleavedStereo, int numFrames) noexcept
{
    if (interleavedStereo == nullptr || numFrames <= 0)
        return 0;

    const int samples = numFrames * kChannels;
    int shortest = numFrames;
    bool any = false;

    for (auto& slot : slots)
    {
        if (!slot.active.load(std::memory_order_acquire))
            continue;

        any = true;
        const int evenSpace = slot.ring.freeSpace() & ~1;
        const int n = std::min(samples, std::max(0, evenSpace));
        const int wroteSamples = slot.ring.push(interleavedStereo, n);
        const int wrote = wroteSamples / kChannels;
        if (wrote < numFrames)
            slot.overflows.fetch_add(1, std::memory_order_relaxed);
        shortest = std::min(shortest, wrote);
    }

    if (!any)
        return numFrames;

    return shortest;
}

int OutputFanout::discardFrames(Slot& slot, int frames) noexcept
{
    if (frames <= 0)
        return 0;
    return slot.ring.discard(frames * kChannels) / kChannels;
}

int OutputFanout::popFrames(Slot& slot, float* interleaved, int frames) noexcept
{
    if (frames <= 0)
        return 0;
    return slot.ring.pop(interleaved, frames * kChannels) / kChannels;
}

void OutputFanout::applyGain(Slot& slot, float* interleaved, int frames) noexcept
{
    const float target = slot.muted.load(std::memory_order_relaxed) ? 0.0f
                                                                    : slot.gain.load(std::memory_order_relaxed);
    const bool smooth = smoothingEnabled.load(std::memory_order_relaxed);
    float gain = slot.smoothed;

    if (!smooth)
    {
        gain = target;
        for (int i = 0; i < frames * kChannels; ++i)
            interleaved[i] *= gain;
        slot.smoothed = gain;
        noteOutputLevel(slot, interleaved, frames);
        return;
    }

    for (int i = 0; i < frames; ++i)
    {
        gain += (target - gain) * 0.01f;
        interleaved[i * 2] *= gain;
        interleaved[i * 2 + 1] *= gain;
    }
    slot.smoothed = gain;
    noteOutputLevel(slot, interleaved, frames);
}

void OutputFanout::noteOutputLevel(Slot& slot, const float* interleaved, int frames) noexcept
{
    float peak = 0.0f;
    if (interleaved != nullptr)
    {
        for (int i = 0; i < frames * kChannels; ++i)
            peak = std::max(peak, std::abs(interleaved[i]));
    }
    const float previous = slot.outputLevel.load(std::memory_order_relaxed);
    slot.outputLevel.store(followOutputLevel(previous, peak), std::memory_order_relaxed);
}

int OutputFanout::pull(int slotIndex, float* interleavedStereo, int numFrames) noexcept
{
    if (interleavedStereo == nullptr || numFrames <= 0 || slotIndex < 0 || slotIndex >= kMaxOutputs)
        return 0;

    auto& slot = slots[static_cast<size_t>(slotIndex)];
    if (!slot.active.load(std::memory_order_acquire))
    {
        std::fill(interleavedStereo, interleavedStereo + numFrames * kChannels, 0.0f);
        slot.outputLevel.store(0.0f, std::memory_order_relaxed);
        return 0;
    }

    const double inRate = getInputSampleRate();
    const int fill = slot.ring.available() / kChannels;
    const auto plan = planDrift(fill, slot.targetFrames, driftEnabled.load(std::memory_order_relaxed), inRate,
                                slot.outputSampleRate);

    if (plan.skipFrames > 0)
        discardFrames(slot, plan.skipFrames);

    const float driftMs = static_cast<float>((fill - slot.targetFrames) * 1000.0 / std::max(1.0, inRate));
    slot.driftMs.store(driftMs, std::memory_order_relaxed);

    int produced = 0;
    const bool identity = std::abs(plan.resampleRatio - 1.0f) < 0.0001f;

    if (plan.duplicateFrames > 0 && slot.hasLast)
    {
        const int dups = std::min(plan.duplicateFrames, numFrames);
        for (int i = 0; i < dups; ++i)
        {
            interleavedStereo[i * 2] = slot.lastLeft;
            interleavedStereo[i * 2 + 1] = slot.lastRight;
        }
        produced = dups;
    }

    const int need = numFrames - produced;
    if (identity)
    {
        const int got = need > 0 ? popFrames(slot, interleavedStereo + produced * kChannels, need) : 0;
        produced += got;
    }
    else if (need > 0)
    {
        const int wantIn = std::min(maxPullFrames - 2, std::max(2, static_cast<int>(std::ceil(need * plan.resampleRatio)) + 2));
        constexpr int kStackFrames = 2048;
        float stack[kStackFrames * kChannels];
        const int chunkIn = std::min(wantIn, kStackFrames);
        const int gotIn = popFrames(slot, stack, chunkIn);
        if (gotIn >= 2)
        {
            double pos = 0.0;
            const int outRoom = need;
            int made = 0;
            for (; made < outRoom; ++made)
            {
                const int i0 = static_cast<int>(pos);
                if (i0 + 1 >= gotIn)
                    break;
                const float t = static_cast<float>(pos - i0);
                const float* a = stack + i0 * kChannels;
                const float* b = a + kChannels;
                float* d = interleavedStereo + (produced + made) * kChannels;
                d[0] = a[0] + (b[0] - a[0]) * t;
                d[1] = a[1] + (b[1] - a[1]) * t;
                pos += plan.resampleRatio;
            }
            produced += made;
        }
    }

    if (produced > 0)
    {
        slot.lastLeft = interleavedStereo[(produced - 1) * kChannels];
        slot.lastRight = interleavedStereo[(produced - 1) * kChannels + 1];
        slot.hasLast = true;
    }

    if (produced < numFrames)
    {
        std::fill(interleavedStereo + produced * kChannels, interleavedStereo + numFrames * kChannels, 0.0f);
        slot.underruns.fetch_add(1, std::memory_order_relaxed);
    }

    applyGain(slot, interleavedStereo, numFrames);

    const int filled = slot.ring.available() / kChannels;
    const float latency = static_cast<float>(filled * 1000.0 / std::max(1.0, inRate));
    slot.latencyMs.store(latency, std::memory_order_relaxed);
    return produced;
}

void OutputFanout::pullPlanar(int slot, float* const* outputs, int numChannels, int numFrames) noexcept
{
    if (outputs == nullptr || numFrames <= 0)
        return;

    int done = 0;
    while (done < numFrames)
    {
        const int n = std::min(maxPullFrames, numFrames - done);
        // planarScratch is only safe for one consumer. Each output callback has its own slot,
        // but they would share this buffer. Pull into a stack block instead.
        constexpr int kChunk = 512;
        const int count = std::min(n, kChunk);
        float interleaved[kChunk * kChannels];
        pull(slot, interleaved, count);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (outputs[ch] == nullptr)
                continue;
            const int srcCh = std::min(ch, kChannels - 1);
            for (int i = 0; i < count; ++i)
                outputs[ch][done + i] = interleaved[i * kChannels + srcCh];
        }

        done += count;
    }
}

int OutputFanout::getFillFrames(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return 0;
    return slots[static_cast<size_t>(slot)].ring.available() / kChannels;
}

float OutputFanout::getLatencyMs(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return 0.0f;
    return slots[static_cast<size_t>(slot)].latencyMs.load(std::memory_order_relaxed);
}

float OutputFanout::getDriftMs(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return 0.0f;
    return slots[static_cast<size_t>(slot)].driftMs.load(std::memory_order_relaxed);
}

float OutputFanout::getOutputLevel(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return 0.0f;
    return slots[static_cast<size_t>(slot)].outputLevel.load(std::memory_order_relaxed);
}

uint32_t OutputFanout::getUnderrunCount(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return 0;
    return slots[static_cast<size_t>(slot)].underruns.load(std::memory_order_relaxed);
}

uint32_t OutputFanout::getOverflowCount(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return 0;
    return slots[static_cast<size_t>(slot)].overflows.load(std::memory_order_relaxed);
}

bool OutputFanout::isActive(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxOutputs)
        return false;
    return slots[static_cast<size_t>(slot)].active.load(std::memory_order_acquire);
}
