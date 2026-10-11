#pragma once

#include "../core/OutputFanout.h"
#include "../core/SpscRingBuffer.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct ExtraInput
{
    std::string driver;
    std::string deviceName;
    std::string label;
};

std::vector<ExtraInput> enumerateExtraInputs();

// Friendly names of render endpoints whose OS enumerator is a Bluetooth transport.
// Empty on platforms where that query is not available.
std::vector<std::string> bluetoothRenderNames();

bool isExtraInputDriver(const std::string& driver);

class ExtraInputStream
{
public:
    virtual ~ExtraInputStream() = default;
};

// nullptr means the device could not be opened. `error` explains why.
// Only call this for drivers that isExtraInputDriver() accepts.
std::unique_ptr<ExtraInputStream> openExtraInput(const std::string& driver,
                                                 const std::string& deviceName,
                                                 OutputFanout& fanout,
                                                 SpscRingBuffer<float>& visualMono,
                                                 std::atomic<float>& level,
                                                 double& sampleRate,
                                                 std::string& error);
