#pragma once

#include "../core/DisplayLayout.h"
#include "../engine/AudioEngine.h"
#include "EscapeGrab.h"
#include "VisualHost.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <string>
#include <utility>
#include <vector>

class MainComponent : public juce::Component, private juce::Timer, private AudioEngine::Listener
{
public:
    MainComponent();
    ~MainComponent() override;

    void startDemo(const juce::File& directory, int seconds);

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    class OutputRow;
    class StageWindow;

    void timerCallback() override;
    void engineChanged() override;
    void rebuildInputs();
    void rebuildOutputs();
    void refreshStats();
    void refreshDisplays();
    void layoutControls();
    void setStage(bool enabled);
    void syncFeedback();
    void sampleLuma(const char* phase);
    void printLumaSummary();
    std::vector<MonitorDesc> collectMonitors() const;

    AudioEngine engine;
    VisualHost visual;
    juce::Label title { {}, "WAVIS" };
    juce::Label subtitle { {}, "One input, every selected output" };
    juce::Label driverLabel { {}, "Driver" };
    juce::ComboBox drivers;
    juce::Label inputLabel { {}, "Input" };
    juce::ComboBox inputs;
    juce::TextButton fileButton { "Audio file" };
    juce::Label fileName;
    juce::TextButton startButton { "Start" };
    juce::TextButton stopButton { "Stop" };
    juce::Label meterLabel { {}, "Level" };
    juce::Label levelReadout;
    juce::Label fpsLabel;
    juce::Label modeLabel { {}, "Visual" };
    juce::ComboBox modes;
    juce::Label displayLabel { {}, "Display" };
    juce::ComboBox displayBox;
    juce::ToggleButton stageButton { "Fullscreen on display 1" };
    juce::Label backendLabel;
    juce::Label outputLabel { {}, "Outputs" };
    juce::Label outputCount;
    juce::Label hint;
    juce::Label feedback;
    juce::TextButton muteFeedback { "Mute that output" };
    juce::Label status;
    juce::Viewport viewport;
    juce::Component outputList;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<StageWindow> stage;
    EscapeGrab escapeGrab;

    std::vector<std::string> inputIds;
    std::vector<MonitorDesc> monitors;
    std::string selectedMonitorId;
    juce::Rectangle<int> meterBounds;
    float displayedLevel = 0.0f;
    bool stageOpen = false;
    bool demo = false;
    int demoTicks = 0;
    int demoSeconds = 7;
    int demoShots = 0;
    int lumaPhaseTick = 0;
    bool lumaBusy = false;
    bool demoRoomShown = false;
    bool demoRoomMoved = false;
    struct LumaSample
    {
        std::string phase;
        double value = 0.0;
    };
    std::vector<LumaSample> lumaSamples;
    juce::File demoDir;
    bool buildingUi = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
