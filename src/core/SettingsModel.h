#pragma once

#include "RoutingModel.h"

#include <optional>
#include <sstream>
#include <string>
#include <vector>

struct OutputSetting
{
    std::string id;
    float gain = 0.8f;
    bool muted = false;
    bool selected = true;
};

struct AppSettings
{
    int version = 1;
    std::string driver = "All drivers";
    std::string inputId = "wavis:test-tone";
    std::string visual = "oscilloscope";
    std::string filePath;
    std::vector<OutputSetting> outputs;
};

inline bool isVisualModeName(std::string_view name) noexcept
{
    return name == "oscilloscope" || name == "spectrum" || name == "radial";
}

inline std::string sanitiseField(std::string value)
{
    for (char& c : value)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return value;
}

inline std::string serialiseSettings(const AppSettings& settings)
{
    std::ostringstream out;
    out << "WAVIS1\n";
    out << "driver=" << sanitiseField(settings.driver) << "\n";
    out << "input=" << sanitiseField(settings.inputId) << "\n";
    out << "visual=" << (isVisualModeName(settings.visual) ? settings.visual : "oscilloscope") << "\n";
    out << "file=" << sanitiseField(settings.filePath) << "\n";

    std::vector<std::string> seen;
    for (const auto& route : settings.outputs)
    {
        if (route.id.empty())
            continue;
        bool duplicate = false;
        for (const auto& id : seen)
            duplicate = duplicate || id == route.id;
        if (duplicate)
            continue;
        seen.push_back(route.id);
        out << "route=" << sanitiseField(route.id) << "\t" << clampGain(route.gain) << "\t" << (route.muted ? 1 : 0)
            << "\t" << (route.selected ? 1 : 0) << "\n";
    }

    return out.str();
}

inline std::optional<AppSettings> parseSettings(const std::string& text)
{
    const bool header = text.rfind("WAVIS1\n", 0) == 0 || text.rfind("WAVIS1\r\n", 0) == 0 || text == "WAVIS1";
    if (!header)
        return std::nullopt;

    AppSettings settings;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line))
        return std::nullopt;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != "WAVIS1")
        return std::nullopt;

    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("driver=", 0) == 0)
            settings.driver = line.substr(7);
        else if (line.rfind("input=", 0) == 0)
            settings.inputId = line.substr(6);
        else if (line.rfind("visual=", 0) == 0)
            settings.visual = isVisualModeName(line.substr(7)) ? line.substr(7) : "oscilloscope";
        else if (line.rfind("file=", 0) == 0)
            settings.filePath = line.substr(5);
        else if (line.rfind("route=", 0) == 0)
        {
            auto body = line.substr(6);
            std::string fields[4];
            int index = 0;
            std::string current;
            for (char c : body)
            {
                if (c == '\t' && index < 3)
                {
                    fields[index++] = current;
                    current.clear();
                }
                else
                    current.push_back(c);
            }
            fields[index] = current;
            if (fields[0].empty())
                continue;

            bool duplicate = false;
            for (const auto& existing : settings.outputs)
                duplicate = duplicate || existing.id == fields[0];
            if (duplicate)
                continue;

            OutputSetting route;
            route.id = fields[0];
            try
            {
                route.gain = fields[1].empty() ? 0.8f : clampGain(std::stof(fields[1]));
            }
            catch (...)
            {
                route.gain = 0.8f;
            }
            route.muted = fields[2] == "1";
            route.selected = fields[3] != "0";
            settings.outputs.push_back(route);
        }
    }

    if (settings.inputId.empty())
        settings.inputId = "wavis:test-tone";
    if (settings.driver.empty())
        settings.driver = "All drivers";
    return settings;
}

inline void applyOutputSettings(std::vector<OutputRoute>& routes, const std::vector<OutputSetting>& saved)
{
    for (const auto& setting : saved)
    {
        bool found = false;
        for (auto& route : routes)
        {
            if (route.id != setting.id)
                continue;
            route.gain = clampGain(setting.gain);
            route.muted = setting.muted;
            route.selected = setting.selected;
            found = true;
            break;
        }

        if (found || !setting.selected)
            continue;

        OutputRoute ghost;
        ghost.id = setting.id;
        const auto split = setting.id.find("::");
        if (split != std::string::npos)
        {
            ghost.driver = setting.id.substr(0, split);
            ghost.deviceName = setting.id.substr(split + 2);
        }
        else
            ghost.deviceName = setting.id;
        ghost.label = ghost.deviceName;
        ghost.gain = clampGain(setting.gain);
        ghost.muted = setting.muted;
        ghost.selected = true;
        ghost.available = false;
        ghost.state = LinkState::disconnected;
        routes.push_back(ghost);
    }
}
