#include "AudioEngine.h"

#include "../core/SourceClassifier.h"
#include "SystemInputs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace
{
double pickClosest(const juce::Array<double>& values, double preferred)
{
    if (values.isEmpty())
        return preferred;
    double best = values.getFirst();
    for (auto value : values)
        if (std::abs(value - preferred) < std::abs(best - preferred))
            best = value;
    return best;
}

int pickClosest(const juce::Array<int>& values, int preferred)
{
    if (values.isEmpty())
        return preferred;
    int best = values.getFirst();
    for (auto value : values)
        if (std::abs(value - preferred) < std::abs(best - preferred))
            best = value;
    return best;
}

std::string platformTag()
{
#if JUCE_MAC
    return "macos";
#elif JUCE_WINDOWS
    return "windows";
#else
    return "linux";
#endif
}

bool isVirtualInput(const std::string& id)
{
    return id == "wavis:test-tone" || id == "wavis:file" || id.empty();
}
} // namespace

class AudioEngine::TypeListener : public juce::AudioIODeviceType::Listener
{
public:
    explicit TypeListener(AudioEngine& owner) : engine(owner) {}
    void audioDeviceListChanged() override { engine.requestScan(); }
    AudioEngine& engine;
};

class AudioEngine::InputCallback : public juce::AudioIODeviceCallback
{
public:
    explicit InputCallback(AudioEngine& owner) : engine(owner) {}

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

        int done = 0;
        while (done < numSamples)
        {
            constexpr int kChunk = 256;
            const int count = std::min(kChunk, numSamples - done);
            float interleaved[kChunk * 2];
            float peak = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                const float left = (numInputChannels > 0 && inputChannelData != nullptr && inputChannelData[0] != nullptr)
                                       ? inputChannelData[0][done + i]
                                       : 0.0f;
                const float right = (numInputChannels > 1 && inputChannelData[1] != nullptr) ? inputChannelData[1][done + i]
                                                                                              : left;
                interleaved[i * 2] = left;
                interleaved[i * 2 + 1] = right;
                peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
            }
            engine.pushProducerBlock(interleaved, count);
            engine.level.store(peak, std::memory_order_relaxed);
            done += count;
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        if (device != nullptr)
        {
            const double rate = device->getCurrentSampleRate();
            engine.fanout.setInputSampleRate(rate);
            if (rate > 0.0)
                engine.visualRate.store(rate, std::memory_order_relaxed);
        }
    }

    void audioDeviceStopped() override {}

private:
    AudioEngine& engine;
};

class AudioEngine::OutputCallback : public juce::AudioIODeviceCallback
{
public:
    OutputCallback(AudioEngine& owner, int outputSlot) : engine(owner), slot(outputSlot) {}

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputChannelData, int numOutputChannels,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        juce::ScopedNoDenormals noDenormals;
        engine.fanout.pullPlanar(slot, outputChannelData, numOutputChannels, numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

private:
    AudioEngine& engine;
    int slot = -1;
};

AudioEngine::AudioEngine()
{
    formats.registerBasicFormats();
    juce::PropertiesFile::Options options;
    options.applicationName = "Wavis";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "Wavis";
    properties.setStorageParameters(options);

    fanout.setDriftEnabled(true);
    fanout.setSmoothingEnabled(true);
    fanout.setTargetLatencyMs(50.0);

    std::ignore = deviceManager.getAvailableDeviceTypes();
    typeListener = std::make_unique<TypeListener>(*this);
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        type->addListener(typeListener.get());
    deviceManager.addChangeListener(this);

    if (auto* settingsFile = properties.getUserSettings())
    {
        if (auto parsed = parseSettings(settingsFile->getValue("state").toStdString()))
        {
            driver = parsed->driver;
            inputId = parsed->inputId;
            visualMode = parsed->visual;
            filePath = parsed->filePath;
            savedOutputs = parsed->outputs;
        }
    }

    startTimerHz(10);
    rescan();
}

AudioEngine::~AudioEngine()
{
    alive->store(false);
    stopTimer();
    deviceManager.removeChangeListener(this);
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        type->removeListener(typeListener.get());
    stop();
}

void AudioEngine::addListener(Listener* listener) { listeners.add(listener); }
void AudioEngine::removeListener(Listener* listener) { listeners.remove(listener); }

void AudioEngine::requestScan() { scanRequested = true; }

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*) { requestScan(); }

void AudioEngine::timerCallback()
{
    ++scanTicks;
    if (!scanRequested && scanTicks < 15)
        return;
    scanTicks = 0;
    scanRequested = false;
    rescan();
    if (scanRequested)
    {
        scanRequested = false;
        rescan();
    }
}

std::vector<std::string> AudioEngine::getDriverNames() const { return drivers; }
std::string AudioEngine::getDriver() const { return driver; }

void AudioEngine::setDriver(const std::string& next)
{
    driver = next;
    saveSettings();
    notifyChanged();
}

bool AudioEngine::driverVisible(const std::string& driverName) const
{
    return driver == "All drivers" || driver == driverName;
}

std::vector<AudioEngine::InputChoice> AudioEngine::getInputs() const
{
    std::vector<InputChoice> choices;
    choices.push_back({ "wavis:test-tone", "Test tone", false });
    choices.push_back({ "wavis:file", "Audio file", false });
    std::vector<AudioEndpoint> visibleInputs;
    for (const auto& ep : endpoints)
        if (ep.input && driverVisible(ep.driver))
            visibleInputs.push_back(ep);
    if (driver == "All drivers")
        visibleInputs = collapseDuplicateDevices(visibleInputs, std::set<std::string> { inputId });

    for (const auto& ep : visibleInputs)
    {
        choices.push_back({ ep.id, ep.label.empty() ? ep.deviceName : ep.label, ep.systemAudio });
    }

    bool found = false;
    for (const auto& choice : choices)
        found = found || choice.id == inputId;
    if (!found && !inputId.empty())
        choices.push_back({ inputId, "Disconnected â€” " + inputId, false });
    return choices;
}

std::string AudioEngine::getInputId() const { return inputId; }

void AudioEngine::setInputId(const std::string& id)
{
    inputId = id.empty() ? "wavis:test-tone" : id;
    saveSettings();
    if (running.load())
    {
        stopSource();
        startSource();
    }
    notifyChanged();
}

std::vector<AudioEngine::OutputChoice> AudioEngine::getOutputs() const
{
    std::vector<OutputChoice> choices;
    std::vector<OutputRoute> visibleRoutes;
    for (const auto& route : routes)
    {
        if (route.available && !driverVisible(route.driver))
            continue;
        if (!route.available && !route.selected)
            continue;
        visibleRoutes.push_back(route);
    }
    if (driver == "All drivers")
    {
        std::set<std::string> selectedIds;
        std::vector<OutputRoute> available;
        std::vector<OutputRoute> unavailable;
        for (const auto& route : visibleRoutes)
        {
            if (route.selected)
                selectedIds.insert(route.id);
            (route.available ? available : unavailable).push_back(route);
        }
        visibleRoutes = collapseDuplicateDevices(available, selectedIds);
        visibleRoutes.insert(visibleRoutes.end(), unavailable.begin(), unavailable.end());
    }

    for (const auto& route : visibleRoutes)
    {

        OutputChoice choice;
        choice.id = route.id;
        choice.label = route.label.empty() ? route.deviceName : route.label;
        choice.selected = route.selected;
        choice.gain = route.gain;
        choice.muted = route.muted;
        choice.available = route.available;

        const auto live = liveOutputs.find(route.id);
        const int slot = live == liveOutputs.end() ? -1 : live->second.slot;
        const auto underruns = slot >= 0 ? fanout.getUnderrunCount(slot) : 0u;
        if (!route.available || route.state == LinkState::disconnected)
            choice.status = "Disconnected";
        else if (route.state == LinkState::error)
            choice.status = "Error";
        else if (route.muted && route.state == LinkState::live)
            choice.status = "Muted";
        else if (route.state == LinkState::live && underruns > 0 && fanout.getFillFrames(slot) < 32)
            choice.status = "Underrun";
        else if (route.state == LinkState::live)
            choice.status = "Live";
        else
            choice.status = route.selected ? "Ready" : "Off";

        if (slot >= 0)
        {
            choice.latencyMs = fanout.getLatencyMs(slot);
            choice.driftMs = fanout.getDriftMs(slot);
        }
        choices.push_back(choice);
    }
    return choices;
}

void AudioEngine::setOutputSelected(const std::string& id, bool selected)
{
    routes = withSelection(std::move(routes), id, selected);
    saveSettings();
    if (running.load())
        applyPresence();
    notifyChanged();
}

void AudioEngine::setOutputGain(const std::string& id, float gain)
{
    routes = withGain(std::move(routes), id, gain);
    if (const auto live = liveOutputs.find(id); live != liveOutputs.end())
        fanout.setGain(live->second.slot, clampGain(gain));
    saveSettings();
}

void AudioEngine::setOutputMuted(const std::string& id, bool muted)
{
    routes = withMute(std::move(routes), id, muted);
    if (const auto live = liveOutputs.find(id); live != liveOutputs.end())
        fanout.setMuted(live->second.slot, muted);
    saveSettings();
    notifyChanged();
}

void AudioEngine::setFile(const juce::File& file)
{
    filePath = file.getFullPathName().toStdString();
    inputId = "wavis:file";
    saveSettings();
    if (running.load())
    {
        stopSource();
        startSource();
    }
    notifyChanged();
}

juce::String AudioEngine::getFileName() const
{
    if (filePath.empty())
        return "No file chosen";
    return juce::File(filePath).getFileName();
}

std::string AudioEngine::getVisualMode() const { return visualMode; }

void AudioEngine::setVisualMode(const std::string& mode)
{
    visualMode = isVisualModeName(mode) ? mode : "oscilloscope";
    saveSettings();
}

bool AudioEngine::openOutput(OutputRoute& route)
{
    if (liveOutputs.count(route.id) != 0)
        return true;

    auto device = createDevice(route.driver, route.deviceName, false);
    if (device == nullptr)
    {
        status = "Could not create output " + route.label;
        return false;
    }

    const double preferred = fanout.getInputSampleRate();
    const double rate = pickClosest(device->getAvailableSampleRates(), preferred > 0.0 ? preferred : 48000.0);
    const int buffer = pickClosest(device->getAvailableBufferSizes(), 512);
    juce::BigInteger inputs;
    juce::BigInteger outputs;
    int channels = device->getOutputChannelNames().size();
    if (channels <= 0)
        channels = 2;
    channels = std::min(channels, 2);
    outputs.setRange(0, channels, true);
    const auto error = device->open(inputs, outputs, rate, buffer);
    if (error.isNotEmpty())
    {
        status = error.toStdString();
        return false;
    }

    const double actual = device->getCurrentSampleRate();
    const int slot = fanout.activateOutput(actual > 0.0 ? actual : rate);
    if (slot < 0)
    {
        device->close();
        status = "Wavis routes at most 16 outputs at once.";
        return false;
    }

    fanout.setGain(slot, route.gain);
    fanout.setMuted(slot, route.muted);
    auto callback = std::make_unique<OutputCallback>(*this, slot);
    device->start(callback.get());

    LiveOutput live;
    live.slot = slot;
    live.callback = std::move(callback);
    live.device = std::move(device);
    liveOutputs.emplace(route.id, std::move(live));
    return true;
}

void AudioEngine::closeOutput(const std::string& id)
{
    const auto found = liveOutputs.find(id);
    if (found == liveOutputs.end())
        return;
    if (found->second.device != nullptr)
    {
        found->second.device->stop();
        found->second.device->close();
    }
    fanout.deactivateOutput(found->second.slot);
    liveOutputs.erase(found);
}

void AudioEngine::closeAllOutputs()
{
    std::vector<std::string> ids;
    ids.reserve(liveOutputs.size());
    for (const auto& entry : liveOutputs)
        ids.push_back(entry.first);
    for (const auto& id : ids)
        closeOutput(id);
}

void AudioEngine::applyPresence()
{
    const auto updates = planPresence(routes, running.load());
    for (const auto& update : updates)
    {
        OutputRoute* route = nullptr;
        for (auto& candidate : routes)
            if (candidate.id == update.id)
                route = &candidate;
        if (route == nullptr)
            continue;

        if (update.shouldClose)
            closeOutput(route->id);

        route->state = update.newState;
        if (!update.shouldOpen)
            continue;

        if (openOutput(*route))
            route->state = LinkState::live;
        else
            route->state = LinkState::error;
    }

    if (!running.load() || isVirtualInput(inputId))
        return;

    const AudioEndpoint* endpoint = findEndpoint(endpoints, inputId);
    const bool available = endpoint != nullptr;
    if (!available)
    {
        if (inputDevice != nullptr || extraInput != nullptr)
            stopSource();
        inputState = LinkState::disconnected;
        status = "Input disconnected. It will reconnect when it returns.";
        return;
    }

    if (inputState == LinkState::disconnected)
    {
        startSource();
        if (inputState == LinkState::live)
            status = "Reconnected input " + endpoint->label;
    }
}

void AudioEngine::pushProducerBlock(const float* interleavedStereo, int frames) noexcept
{
    fanout.push(interleavedStereo, frames);
    for (int i = 0; i < frames; ++i)
    {
        const float mono = 0.5f * (interleavedStereo[i * 2] + interleavedStereo[i * 2 + 1]);
        visual.push(&mono, 1);
    }
}

void AudioEngine::runTone()
{
    constexpr int frames = 256;
    float block[frames * 2];
    float mono[frames];
    double p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0;
    constexpr double twoPi = 6.283185307179586;
    const double rate = 48000.0;
    auto next = std::chrono::steady_clock::now();

    while (!toneStop.load(std::memory_order_relaxed))
    {
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i)
        {
            const float sample = 0.40f * static_cast<float>(std::sin(p1)) + 0.22f * static_cast<float>(std::sin(p2))
                                 + 0.14f * static_cast<float>(std::sin(p3)) + 0.08f * static_cast<float>(std::sin(p4));
            p1 += twoPi * 110.0 / rate;
            p2 += twoPi * 220.0 / rate;
            p3 += twoPi * 277.18 / rate;
            p4 += twoPi * 440.0 / rate;
            if (p1 > twoPi) p1 -= twoPi;
            if (p2 > twoPi) p2 -= twoPi;
            if (p3 > twoPi) p3 -= twoPi;
            if (p4 > twoPi) p4 -= twoPi;
            block[i * 2] = sample;
            block[i * 2 + 1] = sample;
            mono[i] = sample;
            peak = std::max(peak, std::abs(sample));
        }

        fanout.push(block, frames);
        visual.push(mono, frames);
        level.store(peak, std::memory_order_relaxed);

        next += std::chrono::microseconds(static_cast<int>(frames * 1000000.0 / rate));
        const auto now = std::chrono::steady_clock::now();
        if (now + std::chrono::milliseconds(5) < next)
            std::this_thread::sleep_until(next);
        else if (now > next + std::chrono::milliseconds(30))
            next = now;
    }
}

void AudioEngine::runFile()
{
    if (reader == nullptr)
        return;

    constexpr int frames = 256;
    juce::AudioBuffer<float> buffer(2, frames);
    float interleaved[frames * 2];
    float mono[frames];
    juce::int64 position = 0;
    const double rate = reader->sampleRate > 0.0 ? reader->sampleRate : 48000.0;
    auto next = std::chrono::steady_clock::now();

    while (!fileStop.load(std::memory_order_relaxed))
    {
        if (position + frames >= reader->lengthInSamples)
            position = 0;
        reader->read(&buffer, 0, frames, position, true, reader->numChannels > 1);
        position += frames;

        float peak = 0.0f;
        for (int i = 0; i < frames; ++i)
        {
            const float left = buffer.getSample(0, i);
            const float right = buffer.getNumChannels() > 1 ? buffer.getSample(1, i) : left;
            interleaved[i * 2] = left;
            interleaved[i * 2 + 1] = right;
            mono[i] = 0.5f * (left + right);
            peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
        }

        fanout.push(interleaved, frames);
        visual.push(mono, frames);
        level.store(peak, std::memory_order_relaxed);

        next += std::chrono::microseconds(static_cast<int>(frames * 1000000.0 / rate));
        const auto now = std::chrono::steady_clock::now();
        if (now + std::chrono::milliseconds(5) < next)
            std::this_thread::sleep_until(next);
        else if (now > next + std::chrono::milliseconds(30))
            next = now;
    }
}

void AudioEngine::stopSource()
{
    toneStop.store(true);
    fileStop.store(true);
    if (toneThread.joinable())
        toneThread.join();
    if (fileThread.joinable())
        fileThread.join();

    if (inputDevice != nullptr)
    {
        inputDevice->stop();
        inputDevice->close();
        inputDevice.reset();
    }
    inputCallback.reset();
    extraInput.reset();
    reader.reset();
    inputState = LinkState::idle;
}

void AudioEngine::startSource()
{
    stopSource();
    level.store(0.0f);

    if (inputId == "wavis:file")
    {
        juce::File file(filePath);
        if (!file.existsAsFile())
        {
            inputState = LinkState::error;
            status = "Choose an audio file, then start.";
            return;
        }
        reader.reset(formats.createReaderFor(file));
        if (reader == nullptr)
        {
            inputState = LinkState::error;
            status = "Could not read that audio file.";
            return;
        }
        fanout.setInputSampleRate(reader->sampleRate);
        visualRate.store(reader->sampleRate > 0.0 ? reader->sampleRate : 48000.0, std::memory_order_relaxed);
        fileStop.store(false);
        fileThread = std::thread([this] { runFile(); });
        inputState = LinkState::live;
        status = "Playing " + file.getFileName().toStdString();
        return;
    }

    if (isVirtualInput(inputId))
    {
        inputId = "wavis:test-tone";
        fanout.setInputSampleRate(48000.0);
        visualRate.store(48000.0, std::memory_order_relaxed);
        toneStop.store(false);
        toneThread = std::thread([this] { runTone(); });
        inputState = LinkState::live;
        status = "Test tone running.";
        return;
    }

    const auto* endpoint = findEndpoint(endpoints, inputId);
    if (endpoint == nullptr)
    {
        inputState = LinkState::disconnected;
        status = "Input disconnected. It will reconnect when it returns.";
        return;
    }

    if (isExtraInputDriver(endpoint->driver))
    {
        double rate = 48000.0;
        std::string error;
        extraInput = openExtraInput(endpoint->driver, endpoint->deviceName, fanout, visual, level, rate, error);
        if (extraInput == nullptr)
        {
            inputState = LinkState::error;
            status = error.empty() ? "Could not open the system-audio input." : error;
            return;
        }
        fanout.setInputSampleRate(rate);
        visualRate.store(rate > 0.0 ? rate : 48000.0, std::memory_order_relaxed);
        inputState = LinkState::live;
        status = "System/browser audio running.";
        return;
    }

    inputDevice = createDevice(endpoint->driver, endpoint->deviceName, true);
    if (inputDevice == nullptr)
    {
        inputState = LinkState::error;
        status = "Could not create input " + endpoint->label;
        return;
    }

    const double rate = pickClosest(inputDevice->getAvailableSampleRates(), 48000.0);
    const int buffer = pickClosest(inputDevice->getAvailableBufferSizes(), 512);
    juce::BigInteger inputs;
    juce::BigInteger outputs;
    int channels = inputDevice->getInputChannelNames().size();
    if (channels <= 0)
        channels = 2;
    channels = std::min(channels, 2);
    inputs.setRange(0, channels, true);
    const auto error = inputDevice->open(inputs, outputs, rate, buffer);
    if (error.isNotEmpty())
    {
        inputDevice.reset();
        inputState = LinkState::error;
        status = error.toStdString();
        return;
    }

    fanout.setInputSampleRate(inputDevice->getCurrentSampleRate());
    if (inputDevice->getCurrentSampleRate() > 0.0)
        visualRate.store(inputDevice->getCurrentSampleRate(), std::memory_order_relaxed);
    inputCallback = std::make_unique<InputCallback>(*this);
    inputDevice->start(inputCallback.get());
    inputState = LinkState::live;
    status = "Input running: " + endpoint->label;
}

std::unique_ptr<juce::AudioIODevice> AudioEngine::createDevice(const std::string& driverName, const std::string& name, bool input)
{
    const juce::String wantedName(name);
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName().toStdString() != driverName)
            continue;
        if (!type->getDeviceNames(input).contains(wantedName))
            continue;

        juce::AudioIODevice* raw = nullptr;
        if (input)
            raw = type->hasSeparateInputsAndOutputs() ? type->createDevice({}, wantedName)
                                                      : type->createDevice(wantedName, wantedName);
        else
            raw = type->hasSeparateInputsAndOutputs() ? type->createDevice(wantedName, {})
                                                      : type->createDevice(wantedName, wantedName);
        if (raw != nullptr)
            return std::unique_ptr<juce::AudioIODevice>(raw);
    }
    return nullptr;
}

void AudioEngine::start()
{
    if (running.load())
        stop();

    running.store(true);
    startSource();
    if (inputState == LinkState::error)
    {
        running.store(false);
        notifyChanged();
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    applyPresence();

    int liveCount = 0;
    for (const auto& route : routes)
        if (route.state == LinkState::live)
            ++liveCount;

    if (inputState == LinkState::live)
    {
        if (liveCount == 0)
            status += " Video only â€” no outputs selected.";
        else
            status += " Routing to " + std::to_string(liveCount) + (liveCount == 1 ? " output." : " outputs.");
    }

    saveSettings();
    notifyChanged();
}

void AudioEngine::stop()
{
    running.store(false);
    stopSource();
    closeAllOutputs();
    for (auto& route : routes)
    {
        if (!route.selected)
            route.state = LinkState::idle;
        else if (!route.available)
            route.state = LinkState::disconnected;
        else
            route.state = LinkState::idle;
    }
    level.store(0.0f);
    inputState = LinkState::idle;
    status = "Stopped.";
    notifyChanged();
}

float AudioEngine::getInputLevel() const noexcept
{
    if (!running.load())
        return 0.0f;
    return std::clamp(level.load(std::memory_order_relaxed), 0.0f, 1.0f);
}

double AudioEngine::getVisualSampleRate() const noexcept
{
    const double rate = visualRate.load(std::memory_order_relaxed);
    return rate > 0.0 ? rate : 48000.0;
}

int AudioEngine::popVisual(float* destination, int frames) noexcept
{
    if (destination == nullptr || frames <= 0)
        return 0;
    return visual.pop(destination, frames);
}

std::string AudioEngine::getStatusLine() const { return status; }

std::string AudioEngine::getLoopbackHint() const
{
    bool anySystem = false;
    for (const auto& ep : endpoints)
        anySystem = anySystem || (ep.input && ep.systemAudio);
    return platformLoopbackHint(platformTag(), anySystem);
}

std::vector<std::string> AudioEngine::getFeedbackOutputIds() const
{
    std::vector<std::string> ids;
    if (isVirtualInput(inputId))
        return ids;

    const auto* endpoint = findEndpoint(endpoints, inputId);
    if (endpoint == nullptr || !endpoint->systemAudio)
        return ids;

    for (const auto& hit : findFeedbackLoops(endpoint->label, true, routes))
        ids.push_back(hit.outputId);
    return ids;
}

void AudioEngine::muteFeedbackOutputs()
{
    for (const auto& id : getFeedbackOutputIds())
        setOutputMuted(id, true);
}

void AudioEngine::rescan()
{
    if (scanning)
    {
        scanRequested = true;
        return;
    }
    scanning = true;

    std::vector<AudioEndpoint> next;
    std::vector<std::string> nextDrivers { "All drivers" };
    auto addDriver = [&nextDrivers](const std::string& name)
    {
        if (std::find(nextDrivers.begin(), nextDrivers.end(), name) == nextDrivers.end())
            nextDrivers.push_back(name);
    };

    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        const auto typeName = type->getTypeName().toStdString();
        addDriver(typeName);
        for (const auto& name : type->getDeviceNames(true))
        {
            auto ep = makeEndpoint(typeName, name.toStdString(), true, false);
            ep.systemAudio = isSystemAudioName(typeName, ep.deviceName);
            ep.label = ep.systemAudio ? systemAudioLabel(ep.deviceName) : ep.deviceName;
            if (findEndpoint(next, ep.id) == nullptr)
                next.push_back(ep);
        }
        for (const auto& name : type->getDeviceNames(false))
        {
            auto ep = makeEndpoint(typeName, name.toStdString(), false, true, false);
            ep.label = ep.deviceName;
            if (findEndpoint(next, ep.id) == nullptr)
                next.push_back(std::move(ep));
        }
    }

    for (const auto& extra : enumerateExtraInputs())
    {
        auto ep = makeEndpoint(extra.driver, extra.deviceName, true, false, true);
        ep.label = extra.label;
        addDriver(extra.driver);
        if (findEndpoint(next, ep.id) == nullptr)
            next.push_back(std::move(ep));
    }

    const auto diff = diffEndpoints(endpoints, next);
    const bool firstScan = !settingsApplied;
    auto merged = mergeOutputRoutes(routes, next);
    if (!settingsApplied)
    {
        applyOutputSettings(merged, savedOutputs);
        settingsApplied = true;
    }

    endpoints = std::move(next);
    drivers = std::move(nextDrivers);
    if (std::find(drivers.begin(), drivers.end(), driver) == drivers.end())
        driver = "All drivers";

    routes = std::move(merged);
    if (running.load())
        applyPresence();

    scanning = false;
    if (diff.any() || firstScan)
        notifyChanged();
}

void AudioEngine::notifyChanged()
{
    auto stillAlive = alive;
    juce::MessageManager::callAsync([this, stillAlive]
    {
        if (!stillAlive->load())
            return;
        listeners.call(&Listener::engineChanged);
    });
}

void AudioEngine::saveSettings()
{
    AppSettings settings;
    settings.driver = driver;
    settings.inputId = inputId;
    settings.visual = visualMode;
    settings.filePath = filePath;
    for (const auto& route : routes)
    {
        if (!route.selected)
            continue;
        OutputSetting setting;
        setting.id = route.id;
        setting.gain = route.gain;
        setting.muted = route.muted;
        setting.selected = true;
        settings.outputs.push_back(setting);
    }

    if (auto* file = properties.getUserSettings())
    {
        file->setValue("state", juce::String(serialiseSettings(settings)));
        file->saveIfNeeded();
    }
}
