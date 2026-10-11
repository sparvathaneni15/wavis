#pragma once

#include <string>
#include <string_view>
#include <vector>

// A monitor the control panel can send the borderless stage to.
// `id` is the physical rectangle, so a hot-plug that removes that screen is detectable
// even when the remaining list is reindexed.
struct MonitorDesc
{
    std::string id;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool main = false;
};

inline std::string monitorId(int x, int y, int width, int height)
{
    return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(width) + "," + std::to_string(height);
}

inline std::string monitorLabel(int oneBasedIndex, int width, int height, bool main)
{
    std::string label = "Display " + std::to_string(oneBasedIndex) + " — " + std::to_string(width) + "×"
                        + std::to_string(height);
    if (main)
        label += " (main)";
    return label;
}

inline std::string fullscreenToggleLabel(int oneBasedIndex)
{
    return "Fullscreen on display " + std::to_string(oneBasedIndex);
}

inline int indexOfMonitor(const std::vector<MonitorDesc>& monitors, std::string_view id)
{
    for (int i = 0; i < static_cast<int>(monitors.size()); ++i)
        if (monitors[static_cast<size_t>(i)].id == id)
            return i;
    return -1;
}

inline int defaultMonitorIndex(const std::vector<MonitorDesc>& monitors)
{
    for (int i = 0; i < static_cast<int>(monitors.size()); ++i)
        if (monitors[static_cast<size_t>(i)].main)
            return i;
    return monitors.empty() ? -1 : 0;
}

enum class StageDecision
{
    unchanged,
    exitToMainWindow
};

// The stage stays up only while its display is still attached.
// A missing id means the projector (or second screen) was unplugged.
inline StageDecision stageAfterHotplug(bool stageOpen, std::string_view selectedId, const std::vector<MonitorDesc>& monitors)
{
    if (!stageOpen)
        return StageDecision::unchanged;
    if (selectedId.empty() || indexOfMonitor(monitors, selectedId) < 0)
        return StageDecision::exitToMainWindow;
    return StageDecision::unchanged;
}
