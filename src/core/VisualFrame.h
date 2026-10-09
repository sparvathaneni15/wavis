#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// Native-side analysis that is shipped to the web view. The browser only draws these numbers.

inline constexpr int kWavePoints = 128;
inline constexpr int kSpectrumBins = 64;

// Keep the loudest sample in each bucket so a short peak is not averaged away.
inline void decimatePeaks(const float* source, int sourceCount, float* destination, int destinationCount)
{
    if (destinationCount <= 0)
        return;
    if (source == nullptr || sourceCount <= 0)
    {
        std::fill(destination, destination + destinationCount, 0.0f);
        return;
    }

    for (int i = 0; i < destinationCount; ++i)
    {
        int from = (i * sourceCount) / destinationCount;
        int to = ((i + 1) * sourceCount) / destinationCount;
        if (to <= from)
            to = from + 1;
        if (to > sourceCount)
            to = sourceCount;

        float peak = source[from];
        for (int j = from + 1; j < to; ++j)
            if (std::abs(source[j]) > std::abs(peak))
                peak = source[j];
        destination[i] = peak;
    }
}

// Peak energy in three bands of a linear FFT. Bin i is i * sampleRate / fftSize Hz.
// Bass is 20–150 Hz, mid is 150–2000 Hz, treble is 2000 Hz up to 16 kHz or Nyquist.
struct SpectralBands
{
    float bass = 0.0f;
    float mid = 0.0f;
    float treble = 0.0f;
};

inline float peakInHzRange(const float* magnitudes, int count, double sampleRate, int fftSize, double lowHz, double highHz)
{
    if (magnitudes == nullptr || count <= 0 || sampleRate <= 0.0 || fftSize <= 0 || highHz <= lowHz)
        return 0.0f;

    const double binHz = sampleRate / static_cast<double>(fftSize);
    int from = std::clamp(static_cast<int>(std::floor(lowHz / binHz)), 0, count);
    int to = std::clamp(static_cast<int>(std::ceil(highHz / binHz)), from, count);
    float peak = 0.0f;
    for (int i = from; i < to; ++i)
        peak = std::max(peak, magnitudes[i]);
    return std::clamp(peak * 12.0f, 0.0f, 1.0f);
}

inline SpectralBands measureBands(const float* magnitudes, int count, double sampleRate, int fftSize)
{
    const double nyquist = sampleRate * 0.5;
    SpectralBands bands;
    bands.bass = peakInHzRange(magnitudes, count, sampleRate, fftSize, 20.0, 150.0);
    bands.mid = peakInHzRange(magnitudes, count, sampleRate, fftSize, 150.0, 2000.0);
    bands.treble = peakInHzRange(magnitudes, count, sampleRate, fftSize, 2000.0, std::min(16000.0, nyquist));
    return bands;
}

// Logarithmic grouping of an FFT magnitude array. Earlier bins are narrower.
inline void reduceLogBins(const float* magnitudes, int count, float* bins, int binCount)
{
    if (binCount <= 0)
        return;
    std::fill(bins, bins + binCount, 0.0f);
    if (magnitudes == nullptr || count <= 1)
        return;

    int previous = 0;
    for (int i = 0; i < binCount; ++i)
    {
        int edge = count;
        if (i + 1 < binCount)
        {
            const float t = static_cast<float>(i + 1) / static_cast<float>(binCount);
            const float curved = (std::pow(static_cast<float>(count), t) - 1.0f) / static_cast<float>(count - 1)
                                 * static_cast<float>(count);
            edge = std::clamp(static_cast<int>(std::lround(curved)), previous + 1, count);
        }

        float peak = 0.0f;
        for (int j = previous; j < edge; ++j)
            peak = std::max(peak, magnitudes[j]);
        bins[i] = peak;
        previous = edge;
    }
}
