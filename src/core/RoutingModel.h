#pragma once

#include "DeviceList.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

enum class LinkState
{
    idle,
    live,
    disconnected,
    error
};

struct OutputRoute
{
    std::string id;
    std::string driver;
    std::string deviceName;
    std::string label;
    float gain = 0.8f;
    bool muted = false;
    bool selected = false;
    bool available = false;
    bool systemAudio = false;
    bool bluetooth = false;
    LinkState state = LinkState::idle;
};

struct PresenceUpdate
{
    std::string id;
    LinkState newState = LinkState::idle;
    bool shouldOpen = false;
    bool shouldClose = false;
};

inline float clampGain(float gain) noexcept
{
    if (!std::isfinite(gain))
        return 0.0f;
    return std::clamp(gain, 0.0f, 1.0f);
}

inline float effectiveGain(const OutputRoute& route) noexcept
{
    if (route.muted)
        return 0.0f;
    return clampGain(route.gain);
}

inline std::vector<OutputRoute> withSelection(std::vector<OutputRoute> routes, const std::string& id, bool selected)
{
    for (auto& route : routes)
        if (route.id == id)
            route.selected = selected;
    return routes;
}

inline std::vector<OutputRoute> withGain(std::vector<OutputRoute> routes, const std::string& id, float gain)
{
    for (auto& route : routes)
        if (route.id == id)
            route.gain = clampGain(gain);
    return routes;
}

inline std::vector<OutputRoute> withMute(std::vector<OutputRoute> routes, const std::string& id, bool muted)
{
    for (auto& route : routes)
        if (route.id == id)
            route.muted = muted;
    return routes;
}

// Selected devices that disappear stay in the list so they can reconnect.
// Unselected devices that disappear are dropped.
inline std::vector<OutputRoute> mergeOutputRoutes(const std::vector<OutputRoute>& previous,
                                                  const std::vector<AudioEndpoint>& discovered)
{
    std::vector<OutputRoute> next;
    std::set<std::string> seen;

    for (const auto& ep : discovered)
    {
        if (!ep.output)
            continue;

        seen.insert(ep.id);
        OutputRoute route;
        route.id = ep.id;
        route.driver = ep.driver;
        route.deviceName = ep.deviceName;
        route.label = ep.label.empty() ? ep.deviceName : ep.label;
        route.available = true;
        route.bluetooth = ep.bluetooth;

        for (const auto& old : previous)
        {
            if (old.id != ep.id)
                continue;
            route.gain = old.gain;
            route.muted = old.muted;
            route.selected = old.selected;
            route.state = old.state;
            break;
        }

        next.push_back(route);
    }

    for (const auto& old : previous)
    {
        if (!old.selected || seen.count(old.id) != 0)
            continue;
        auto ghost = old;
        ghost.available = false;
        next.push_back(ghost);
    }

    return next;
}

inline bool linkIsOpen(LinkState state) noexcept
{
    return state == LinkState::live || state == LinkState::error;
}

// sessionRunning: devices are only opened while a session is running.
// An error state is not retried until the device disappears and comes back,
// which clears the error to disconnected.
inline std::vector<PresenceUpdate> planPresence(const std::vector<OutputRoute>& routes, bool sessionRunning)
{
    std::vector<PresenceUpdate> updates;
    updates.reserve(routes.size());

    for (const auto& route : routes)
    {
        PresenceUpdate update;
        update.id = route.id;
        const bool openish = linkIsOpen(route.state);

        if (!sessionRunning)
        {
            update.shouldClose = openish;
            update.shouldOpen = false;
            if (!route.selected)
                update.newState = LinkState::idle;
            else if (!route.available)
                update.newState = LinkState::disconnected;
            else
                update.newState = LinkState::idle;
            updates.push_back(update);
            continue;
        }

        if (!route.selected)
        {
            update.shouldClose = openish;
            update.shouldOpen = false;
            update.newState = LinkState::idle;
            updates.push_back(update);
            continue;
        }

        if (!route.available)
        {
            update.shouldClose = openish;
            update.shouldOpen = false;
            update.newState = LinkState::disconnected;
            updates.push_back(update);
            continue;
        }

        if (route.state == LinkState::error)
        {
            update.shouldOpen = false;
            update.shouldClose = false;
            update.newState = LinkState::error;
            updates.push_back(update);
            continue;
        }

        if (route.state != LinkState::live)
            update.shouldOpen = true;

        update.newState = route.state == LinkState::live ? LinkState::live : route.state;
        updates.push_back(update);
    }

    return updates;
}
