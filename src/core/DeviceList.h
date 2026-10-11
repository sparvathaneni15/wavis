#pragma once

#include <cctype>
#include <map>
#include <set>
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
    bool bluetooth = false;
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
                 || old->systemAudio != ep.systemAudio || old->bluetooth != ep.bluetooth || old->driver != ep.driver)
            diff.labelsChanged = true;
    }

    for (const auto& ep : previous)
        if (findEndpoint(next, ep.id) == nullptr)
            diff.removedIds.push_back(ep.id);

    return diff;
}

// The same physical device is often exposed by several drivers (e.g. Windows Audio, DirectSound, ASIO).
// In the "All drivers" view that makes one channel selectable repeatedly, so keep one entry per
// device name and direction. Ids in `preferredIds` (current selection) win; otherwise the first
// entry in driver order is kept. System-audio sources are distinct by id and never merged.
template <typename T>
std::vector<T> collapseDuplicateDevices(const std::vector<T>& items, const std::set<std::string>& preferredIds)
{
    auto keyOf = [](const T& item)
    {
        if (item.systemAudio)
            return std::string("sys|") + item.id;
        std::string name = item.deviceName;
        const auto begin = name.find_first_not_of(" \t");
        const auto end = name.find_last_not_of(" \t");
        name = begin == std::string::npos ? std::string() : name.substr(begin, end - begin + 1);
        for (auto& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return name;
    };

    std::vector<T> result;
    std::map<std::string, size_t> slotByKey;
    for (const auto& item : items)
    {
        const auto key = keyOf(item);
        const auto it = slotByKey.find(key);
        if (it == slotByKey.end())
        {
            slotByKey[key] = result.size();
            result.push_back(item);
        }
        else if (preferredIds.count(item.id) != 0 && preferredIds.count(result[it->second].id) == 0)
            result[it->second] = item;
    }
    return result;
}
