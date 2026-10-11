#pragma once

#include "RoutingModel.h"

#include <string>
#include <string_view>
#include <vector>

enum class SourceKind
{
    hardware,
    systemAudio,
    testTone,
    file
};

inline char asciiLowerChar(char c) noexcept
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return c;
}

inline std::string asciiLower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = asciiLowerChar(c);
    return out;
}

inline bool contains(std::string_view haystack, std::string_view needle) noexcept
{
    return haystack.find(needle) != std::string_view::npos;
}

// Best effort: the name or id the OS gave us mentions a Bluetooth transport.
// Product nicknames alone (no "bluetooth" / "bluez" / "BTHENUM") are not treated as Bluetooth.
inline bool isBluetoothTransport(std::string_view driver, std::string_view name, std::string_view id)
{
    const auto mentions = [](const std::string& text)
    {
        return text.find("bluetooth") != std::string::npos || text.find("bluez") != std::string::npos
               || text.find("bthenum") != std::string::npos || text.find("bthhfenum") != std::string::npos;
    };
    return mentions(asciiLower(driver)) || mentions(asciiLower(name)) || mentions(asciiLower(id));
}

inline bool endsWith(std::string_view text, std::string_view suffix) noexcept
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline void trimInPlace(std::string& text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    if (i != 0)
        text.erase(0, i);
}

// Pulse/PipeWire monitors, WASAPI loopback names, and macOS virtual loopback drivers.
inline bool isSystemAudioName(std::string_view driver, std::string_view deviceName)
{
    const auto name = asciiLower(deviceName);
    (void) driver;

    if (contains(name, "loopback"))
        return true;
    if (contains(name, "blackhole"))
        return true;
    if (contains(name, "soundflower"))
        return true;
    if (contains(name, "background music"))
        return true;
    if (contains(name, "monitor of "))
        return true;
    if (contains(name, ".monitor"))
        return true;
    if (contains(name, "monitor_"))
        return true;
    if (endsWith(name, "monitor"))
        return true;
    return false;
}

inline bool isPulseMonitor(std::string_view sourceName, std::string_view description)
{
    return isSystemAudioName("PulseAudio", sourceName) || isSystemAudioName("PulseAudio", description);
}

inline std::string systemAudioLabel(std::string_view rawName)
{
    const auto lower = asciiLower(rawName);
    if (lower.rfind("system/browser audio", 0) == 0)
        return std::string(rawName);

    std::string pretty(rawName);
    if (isSystemAudioName("", rawName) && !contains(lower, "monitor of ") && contains(lower, "monitor"))
        pretty = "Monitor of " + pretty;

    return "System/Browser audio — " + pretty;
}

inline std::string labelPulseSource(std::string_view sourceName, std::string_view description)
{
    if (!isPulseMonitor(sourceName, description))
        return std::string(description.empty() ? sourceName : description);

    std::string pretty(description.empty() ? sourceName : description);
    if (!contains(asciiLower(pretty), "monitor of "))
        pretty = "Monitor of " + pretty;
    return "System/Browser audio — " + pretty;
}

inline std::string normalizeEndpointName(std::string_view raw)
{
    auto text = asciiLower(raw);
    auto stripPrefix = [&text](std::string_view prefix)
    {
        if (text.rfind(prefix, 0) == 0)
            text.erase(0, prefix.size());
    };
    auto stripSuffix = [&text](std::string_view suffix)
    {
        if (endsWith(text, suffix))
            text.erase(text.size() - suffix.size());
    };

    stripPrefix("system/browser audio — ");
    stripPrefix("system/browser audio - ");
    stripPrefix("monitor of ");
    stripSuffix(" [loopback]");
    stripSuffix(" (loopback)");
    stripSuffix(".monitor");
    trimInPlace(text);
    return text;
}

inline bool endpointNamesMatch(std::string_view a, std::string_view b)
{
    const auto left = normalizeEndpointName(a);
    const auto right = normalizeEndpointName(b);
    if (left.empty() || right.empty())
        return false;
    if (left == right)
        return true;
    if (left.size() >= 6 && right.find(left) != std::string::npos)
        return true;
    if (right.size() >= 6 && left.find(right) != std::string::npos)
        return true;
    return false;
}

struct FeedbackHit
{
    std::string outputId;
    std::string message;
};

// System/browser capture routed back to the device being captured.
// Hardware mics are not flagged here; this is a loop inside the machine.
inline std::vector<FeedbackHit> findFeedbackLoops(std::string_view inputLabel, bool inputIsSystemAudio,
                                                  const std::vector<OutputRoute>& outputs)
{
    std::vector<FeedbackHit> hits;
    if (!inputIsSystemAudio)
        return hits;

    for (const auto& output : outputs)
    {
        if (!output.selected || output.muted)
            continue;
        const auto& outputName = output.label.empty() ? output.deviceName : output.label;
        if (!endpointNamesMatch(inputLabel, outputName) && !endpointNamesMatch(inputLabel, output.deviceName))
            continue;

        FeedbackHit hit;
        hit.outputId = output.id;
        hit.message = "System/browser audio from \"" + std::string(inputLabel) + "\" is routed back to \""
                      + outputName + "\". Mute that output to avoid a feedback loop.";
        hits.push_back(std::move(hit));
    }

    return hits;
}

inline std::string platformLoopbackHint(std::string_view platform, bool systemInputPresent)
{
    if (platform == "macos")
    {
        if (systemInputPresent)
            return "System/browser audio inputs (BlackHole, Loopback, or Soundflower) are labeled System/Browser audio.";
        return "macOS has no native system-audio loopback. Install BlackHole or Loopback, then select that input. "
               "It will be labeled System/Browser audio.";
    }

    if (platform == "windows")
        return "System/browser audio is a WASAPI loopback of an output device, labeled System/Browser audio.";

    return "PulseAudio and PipeWire monitor sources are labeled System/Browser audio — Monitor of …";
}
