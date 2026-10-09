#pragma once

#include "../core/DeviceList.h"
#include "../core/OutputFanout.h"
#include "../core/SettingsModel.h"
#include "../core/SpscRingBuffer.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class AudioEngine : private juce::ChangeListener, private juce::Timer
{
public:
    struct InputChoice
    {
        std::string id;
        std::string label;
        bool systemAudio = false;
    };

    struct OutputChoice
    {
        std::string id;
        std::string label;
        bool selected = false;
        float gain = 0.8f;
        bool muted = false;
        bool available = false;
        std::string status;
        float latencyMs = 0.0f;
        float driftMs = 0.0f;
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void engineChanged() = 0;
    };

    AudioEngine();
    ~AudioEngine() override;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);
    void requestScan();

    std::vector<std::string> getDriverNames() const;
    std::string getDriver() const;
    void setDriver(const std::string& driver);

    std::vector<InputChoice> getInputs() const;
    std::string getInputId() const;
    void setInputId(const std::string& id);

    std::vector<OutputChoice> getOutputs() const;
    void setOutputSelected(const std::string& id, bool selected);
    void setOutputGain(const std::string& id, float gain);
    void setOutputMuted(const std::string& id, bool muted);

    void setFile(const juce::File& file);
    juce::String getFileName() const;

    std::string getVisualMode() const;
    void setVisualMode(const std::string& mode);

    bool isRunning() const noexcept { return running.load(); }
    void start();
    void stop();

    float getInputLevel() const noexcept;
    int popVisual(float* destination, int frames) noexcept;

    std::string getStatusLine() const;
    std::string getLoopbackHint() const;
    std::vector<std::string> getFeedbackOutputIds() const;
    void muteFeedbackOutputs();

private:
    class InputCallback;
    class OutputCallback;
    class TypeListener;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void rescan();
    void applyPresence();
    void startSource();
    void stopSource();
    void runTone();
    void runFile();
    bool openOutput(OutputRoute& route);
    void closeOutput(const std::string& id);
    void closeAllOutputs();
    void pushProducerBlock(const float* interleavedStereo, int frames) noexcept;
    void notifyChanged();
    void saveSettings();
    bool driverVisible(const std::string& driverName) const;
    std::unique_ptr<juce::AudioIODevice> createDevice(const std::string& driver, const std::string& name, bool input);

    juce::AudioDeviceManager deviceManager;
    juce::ApplicationProperties properties;
    juce::AudioFormatManager formats;
    std::unique_ptr<TypeListener> typeListener;

    OutputFanout fanout;
    SpscRingBuffer<float> visual { 32768 };
    std::atomic<float> level { 0.0f };
    std::atomic<bool> running { false };
    std::atomic<bool> toneStop { true };
    std::atomic<bool> fileStop { true };
    std::thread toneThread;
    std::thread fileThread;

    std::unique_ptr<juce::AudioIODevice> inputDevice;
    std::unique_ptr<InputCallback> inputCallback;
    std::unique_ptr<class ExtraInputStream> extraInput;
    std::unique_ptr<juce::AudioFormatReader> reader;

    struct LiveOutput
    {
        int slot = -1;
        std::unique_ptr<juce::AudioIODevice> device;
        std::unique_ptr<OutputCallback> callback;
    };

    std::map<std::string, LiveOutput> liveOutputs;
    std::vector<AudioEndpoint> endpoints;
    std::vector<OutputRoute> routes;
    std::vector<std::string> drivers { "All drivers" };
    std::string driver = "All drivers";
    std::string inputId = "wavis:test-tone";
    std::string visualMode = "oscilloscope";
    std::string filePath;
    std::string status = "Stopped. The test tone needs no hardware.";
    LinkState inputState = LinkState::idle;
    bool settingsApplied = false;
    bool scanning = false;
    bool scanRequested = false;
    int scanTicks = 0;
    std::vector<OutputSetting> savedOutputs;

    juce::ListenerList<Listener> listeners;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
