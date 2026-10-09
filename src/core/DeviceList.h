#pragma once

#include <string>
#include <string_view>
#include <vector>

struct AudioEndpoint
{
    std::string driver;
    std::string deviceName;
    std::string label;
    bool input = false;
    bool output = false;
    bool systemAudio = false;
    std::string id;
};

inline std::string makeEndpointId(std::string_view driver, std::string_view deviceName)
{
    return std::string(driver) + "::" + std::string(deviceName);
}

inline AudioEndpoint makeEndpoint(std::string driver, std::string deviceName, bool input, bool output, bool systemAudio = false)
{
    AudioEndpoint ep;
    ep.driver = std::move(driver);
    ep.deviceName = std::move(deviceName);
    ep.input = input;
    ep.output = output;
    ep.systemAudio = systemAudio;
    ep.label = ep.deviceName;
    ep.id = makeEndpointId(ep.driver, ep.deviceName);
    return ep;
}

struct ListDiff
{
    std::vector<std::string> addedIds;
    std::vector<std::string> removedIds;
    bool labelsChanged = false;

    bool any() const noexcept
    {
        return !addedIds.empty() || !removedIds.empty() || labelsChanged;
    }
};

inline const AudioEndpoint* findEndpoint(const std::vector<AudioEndpoint>& list, const std::string& id)
{
    for (const auto& ep : list)
        if (ep.id == id)
            return &ep;
    return nullptr;
}

// Identity is the endpoint id. Order does not matter. A label or flag change is reported
// separately so the UI can refresh without treating the device as new.
inline ListDiff diffEndpoints(const std::vector<AudioEndpoint>& previous, const std::vector<AudioEndpoint>& next)
{
    ListDiff diff;

    for (const auto& ep : next)
    {
        const auto* old = findEndpoint(previous, ep.id);
        if (old == nullptr)
            diff.addedIds.push_back(ep.id);
        else if (old->label != ep.label || old->input != ep.input || old->output != ep.output
                 || old->systemAudio != ep.systemAudio || old->driver != ep.driver)
            diff.labelsChanged = true;
    }

    for (const auto& ep : previous)
        if (findEndpoint(next, ep.id) == nullptr)
            diff.removedIds.push_back(ep.id);

    return diff;
}
