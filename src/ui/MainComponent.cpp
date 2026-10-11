#include "MainComponent.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
juce::String statusLineFor(const AudioEngine::OutputChoice& output)
{
    juce::String text(output.status);
    if (output.status == "Live" || output.status == "Muted" || output.status == "Underrun")
    {
        text << "  ·  " << juce::String(output.latencyMs, 0) << " ms";
        text << "  ·  drift " << (output.driftMs >= 0.0f ? "+" : "") << juce::String(output.driftMs, 0) << " ms";
    }
    return text;
}

juce::String probeScript(const char* mode, double bass, double mid, double treble, double level, double lowBins, double highBins)
{
    juce::String bins = "[";
    juce::String wave = "[";
    for (int i = 0; i < 64; ++i)
    {
        if (i > 0)
            bins << ",";
        const double t = i / 63.0;
        bins << juce::String(lowBins * (1.0 - t) + highBins * t, 3);
    }
    bins << "]";
    for (int i = 0; i < 128; ++i)
    {
        if (i > 0)
            wave << ",";
        wave << juce::String(std::sin(i * 0.19) * std::min(1.0, level), 3);
    }
    wave << "]";
    return "window.wavisProbe&&window.wavisProbe({mode:'" + juce::String(mode) + "',bass:" + juce::String(bass, 3)
           + ",mid:" + juce::String(mid, 3) + ",treble:" + juce::String(treble, 3) + ",level:" + juce::String(level, 3)
           + ",bins:" + bins + ",wave:" + wave + "})";
}
} // namespace

class MainComponent::OutputRow : public juce::Component
{
public:
    explicit OutputRow(AudioEngine::OutputChoice choiceIn) : choice(std::move(choiceIn))
    {
        addAndMakeVisible(toggle);
        addAndMakeVisible(name);
        addAndMakeVisible(gain);
        addAndMakeVisible(mute);
        addAndMakeVisible(detail);
        toggle.setButtonText({});
        toggle.setToggleState(choice.selected, juce::dontSendNotification);
        name.setText(choice.label, juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centredLeft);
        gain.setRange(0.0, 1.0, 0.01);
        gain.setValue(choice.gain, juce::dontSendNotification);
        gain.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        gain.setTooltip("Gain for " + juce::String(choice.label));
        mute.setClickingTogglesState(true);
        mute.setToggleState(choice.muted, juce::dontSendNotification);
        mute.setButtonText(choice.muted ? "Muted" : "Mute");
        detail.setJustificationType(juce::Justification::centredLeft);
        detail.setText(statusLineFor(choice), juce::dontSendNotification);

        toggle.onClick = [this]
        {
            if (onSelected)
                onSelected(toggle.getToggleState());
        };
        gain.onValueChange = [this]
        {
            if (onGain)
                onGain(static_cast<float>(gain.getValue()));
        };
        mute.onClick = [this]
        {
            mute.setButtonText(mute.getToggleState() ? "Muted" : "Mute");
            if (onMute)
                onMute(mute.getToggleState());
        };
    }

    void setStats(const AudioEngine::OutputChoice& next)
    {
        choice = next;
        if (!gain.isMouseButtonDown())
            gain.setValue(next.gain, juce::dontSendNotification);
        if (!mute.isMouseButtonDown())
        {
            mute.setToggleState(next.muted, juce::dontSendNotification);
            mute.setButtonText(next.muted ? "Muted" : "Mute");
        }
        toggle.setToggleState(next.selected, juce::dontSendNotification);
        detail.setText(statusLineFor(next), juce::dontSendNotification);
        detail.setColour(juce::Label::textColourId,
                         next.status == "Disconnected" || next.status == "Error" ? juce::Colour(0xffff8d96)
                         : next.status == "Live"                                 ? juce::Colour(0xff5cefff)
                                                                                  : juce::Colour(0xff9b978e));
    }

    const std::string& getId() const { return choice.id; }

    void setKeyFocus(bool focused)
    {
        if (keyFocus == focused)
            return;
        keyFocus = focused;
        repaint();
    }

    std::function<void(bool)> onSelected;
    std::function<void(float)> onGain;
    std::function<void(bool)> onMute;

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 6);
        auto top = area.removeFromTop(22);
        toggle.setBounds(top.removeFromLeft(22));
        name.setBounds(top);
        auto bottom = area.removeFromTop(24);
        mute.setBounds(bottom.removeFromRight(72));
        gain.setBounds(bottom);
        detail.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0xff1a1d25));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 8.0f);
        if (keyFocus)
        {
            g.setColour(juce::Colour(0xff5cefff));
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 8.0f, 1.5f);
        }
    }

private:
    AudioEngine::OutputChoice choice;
    juce::ToggleButton toggle;
    juce::Label name;
    juce::Slider gain { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::TextButton mute;
    juce::Label detail;
    bool keyFocus = false;
};

class MainComponent::StageWindow : public juce::DocumentWindow
{
public:
    StageWindow() : juce::DocumentWindow({}, juce::Colours::black, 0)
    {
        setUsingNativeTitleBar(false);
        setTitleBarHeight(0);
        setDropShadowEnabled(false);
        setResizable(false, false);
        setAlwaysOnTop(true);
        setWantsKeyboardFocus(true);
    }

    int getDesktopWindowStyleFlags() const override
    {
        return juce::ComponentPeer::windowIsTemporary;
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey && onExit)
        {
            onExit();
            return true;
        }
        return juce::DocumentWindow::keyPressed(key);
    }

    void closeButtonPressed() override
    {
        if (onExit)
            onExit();
    }

    std::function<void()> onExit;
};

MainComponent::MainComponent() : visual(engine)
{
    setWantsKeyboardFocus(true);
    addAndMakeVisible(visual);
    for (juce::Component* child : { static_cast<juce::Component*>(&title), static_cast<juce::Component*>(&subtitle),
                                    static_cast<juce::Component*>(&driverLabel), static_cast<juce::Component*>(&drivers),
                                    static_cast<juce::Component*>(&inputLabel), static_cast<juce::Component*>(&inputs),
                                    static_cast<juce::Component*>(&fileButton), static_cast<juce::Component*>(&fileName),
                                    static_cast<juce::Component*>(&startButton), static_cast<juce::Component*>(&stopButton),
                                    static_cast<juce::Component*>(&meterLabel), static_cast<juce::Component*>(&levelReadout),
                                    static_cast<juce::Component*>(&fpsLabel), static_cast<juce::Component*>(&modeLabel),
                                    static_cast<juce::Component*>(&modes), static_cast<juce::Component*>(&displayLabel),
                                    static_cast<juce::Component*>(&displayBox), static_cast<juce::Component*>(&stageButton),
                                    static_cast<juce::Component*>(&backendLabel), static_cast<juce::Component*>(&outputLabel),
                                    static_cast<juce::Component*>(&outputCount),
                                    static_cast<juce::Component*>(&hint), static_cast<juce::Component*>(&feedback),
                                    static_cast<juce::Component*>(&muteFeedback), static_cast<juce::Component*>(&status),
                                    static_cast<juce::Component*>(&viewport) })
        addAndMakeVisible(child);

    title.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    title.setColour(juce::Label::textColourId, juce::Colour(0xff5cefff));
    subtitle.setColour(juce::Label::textColourId, juce::Colour(0xff9b978e));
    hint.setColour(juce::Label::textColourId, juce::Colour(0xffc8c2b4));
    feedback.setColour(juce::Label::textColourId, juce::Colour(0xffffc1c6));
    status.setColour(juce::Label::textColourId, juce::Colour(0xfff4f1ea));
    fileName.setColour(juce::Label::textColourId, juce::Colour(0xff9b978e));
    for (auto* label : { &driverLabel, &inputLabel, &meterLabel, &modeLabel, &displayLabel, &backendLabel, &outputLabel })
        label->setColour(juce::Label::textColourId, juce::Colour(0xff9b978e));

    startButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff5cefff));
    startButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff14160c));
    muteFeedback.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffff5d6c));

    modes.addItem("Waveform", 1);
    modes.addItem("Spectrum", 2);
    modes.addItem("3D scene", 3);
    const auto visualMode = engine.getVisualMode();
    modes.setSelectedId(visualMode == "spectrum" ? 2 : visualMode == "radial" ? 3 : 1, juce::dontSendNotification);
    visual.setMode(visualMode);

    drivers.onChange = [this]
    {
        if (buildingUi)
            return;
        engine.setDriver(drivers.getText().toStdString());
    };
    inputs.onChange = [this]
    {
        if (buildingUi)
            return;
        const int index = inputs.getSelectedItemIndex();
        if (index >= 0 && index < static_cast<int>(inputIds.size()))
            engine.setInputId(inputIds[static_cast<size_t>(index)]);
    };
    modes.onChange = [this]
    {
        const char* names[] = { "oscilloscope", "spectrum", "radial" };
        const int index = std::clamp(modes.getSelectedId() - 1, 0, 2);
        engine.setVisualMode(names[index]);
        visual.setMode(names[index]);
    };
    startButton.onClick = [this] { engine.start(); grabKeyboardFocus(); };
    stopButton.onClick = [this] { engine.stop(); };
    stageButton.onClick = [this] { setStage(stageButton.getToggleState()); };
    stageButton.setTooltip("Borderless on the whole chosen display. Esc leaves it.");
    displayBox.onChange = [this]
    {
        if (buildingUi)
            return;
        const int index = displayBox.getSelectedItemIndex();
        if (index >= 0 && index < static_cast<int>(monitors.size()))
        {
            selectedMonitorId = monitors[static_cast<size_t>(index)].id;
            stageButton.setButtonText(fullscreenToggleLabel(index + 1));
            if (stageOpen)
                setStage(true);
        }
    };
    visual.setExitFullscreen([this] { setStage(false); });
    muteFeedback.onClick = [this] { engine.muteFeedbackOutputs(); };
    fileButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("Choose an audio file", juce::File(),
                                                      "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& chosen)
                             {
                                 const auto file = chosen.getResult();
                                 if (file.existsAsFile())
                                     engine.setFile(file);
                             });
    };

    viewport.setViewedComponent(&outputList, false);
    viewport.setScrollBarsShown(true, false);
    visual.setBackendChanged([this] { updateOutputChrome(); });
    engine.addListener(this);
    rebuildInputs();
    rebuildOutputs();
    refreshDisplays();
    refreshStats();
    updateOutputChrome();
    startTimerHz(15);
    setSize(1200, 740);
}

MainComponent::~MainComponent()
{
    engine.removeListener(this);
}

void MainComponent::startDemo(const juce::File& directory, int seconds)
{
    demo = true;
    demoDir = directory;
    demoSeconds = std::max(4, seconds);
    demoTicks = 0;
    engine.setInputId("wavis:test-tone");
    engine.setVisualMode("oscilloscope");
    visual.setMode("oscilloscope");
    modes.setSelectedId(1, juce::dontSendNotification);
    engine.start();
}

void MainComponent::engineChanged()
{
    rebuildInputs();
    rebuildOutputs();
    refreshStats();
}

void MainComponent::rebuildInputs()
{
    buildingUi = true;
    inputIds.clear();
    drivers.clear(juce::dontSendNotification);
    auto names = engine.getDriverNames();
    int driverId = 1;
    for (const auto& name : names)
    {
        drivers.addItem(name, driverId);
        if (name == engine.getDriver())
            drivers.setSelectedId(driverId, juce::dontSendNotification);
        ++driverId;
    }
    if (drivers.getSelectedId() == 0 && drivers.getNumItems() > 0)
        drivers.setSelectedId(1, juce::dontSendNotification);

    inputs.clear(juce::dontSendNotification);
    const auto choices = engine.getInputs();
    int id = 1;
    int selected = 1;
    for (const auto& choice : choices)
    {
        inputs.addItem(choice.label, id);
        inputIds.push_back(choice.id);
        if (choice.id == engine.getInputId())
            selected = id;
        ++id;
    }
    inputs.setSelectedId(selected, juce::dontSendNotification);
    fileName.setText(engine.getFileName(), juce::dontSendNotification);
    hint.setText(engine.getLoopbackHint(), juce::dontSendNotification);
    buildingUi = false;
}

void MainComponent::rebuildOutputs()
{
    outputList.deleteAllChildren();
    const auto outputs = engine.getOutputs();
    int y = 0;
    int selected = 0;
    for (const auto& output : outputs)
    {
        if (output.selected)
            ++selected;
        auto* row = new OutputRow(output);
        row->setBounds(0, y, 360, 78);
        row->onSelected = [this, id = output.id](bool on) { engine.setOutputSelected(id, on); };
        row->onGain = [this, id = output.id](float gain) { engine.setOutputGain(id, gain); };
        row->onMute = [this, id = output.id](bool muted) { engine.setOutputMuted(id, muted); };
        outputList.addAndMakeVisible(row);
        y += 82;
    }
    if (outputs.empty())
    {
        auto* empty = new juce::Label({}, "No output devices yet. Video still runs from the test tone.");
        empty->setColour(juce::Label::textColourId, juce::Colour(0xff9b978e));
        empty->setBounds(0, 0, 360, 48);
        outputList.addAndMakeVisible(empty);
        y = 48;
    }
    outputList.setSize(360, std::max(48, y));
    outputCount.setText(juce::String(selected) + " selected", juce::dontSendNotification);
}

void MainComponent::refreshStats()
{
    const auto outputs = engine.getOutputs();
    int selected = 0;
    for (auto* child : outputList.getChildren())
    {
        auto* row = dynamic_cast<OutputRow*>(child);
        if (row == nullptr)
            continue;
        for (const auto& output : outputs)
        {
            if (output.id != row->getId())
                continue;
            row->setStats(output);
            if (output.selected)
                ++selected;
        }
    }
    if (visual.isWebView())
        outputCount.setText("in the picture", juce::dontSendNotification);
    else
        outputCount.setText(juce::String(selected) + " selected", juce::dontSendNotification);
    status.setText(engine.getStatusLine(), juce::dontSendNotification);
    backendLabel.setText(visual.backendLabel(), juce::dontSendNotification);
    fileName.setText(engine.getFileName(), juce::dontSendNotification);
    fpsLabel.setText(engine.isRunning() && visual.getFramesPerSecond() > 0 ? juce::String(visual.getFramesPerSecond()) + " fps"
                                                                            : juce::String("--"),
                     juce::dontSendNotification);
    syncFeedback();
    const bool busy = engine.isRunning();
    startButton.setEnabled(!busy);
    stopButton.setEnabled(busy);
}

void MainComponent::updateOutputChrome()
{
    const bool web = visual.isWebView();
    viewport.setVisible(!web);
    outputLabel.setText(web ? "Speakers" : "Outputs", juce::dontSendNotification);
    if (web)
        outputCount.setText("in the picture", juce::dontSendNotification);
    layoutControls();
}

void MainComponent::moveOutputFocus(int delta)
{
    const int count = outputList.getNumChildComponents();
    if (count <= 0 || visual.isWebView())
        return;
    outputFocus = (outputFocus + delta + count) % count;
    int index = 0;
    for (auto* child : outputList.getChildren())
    {
        if (auto* row = dynamic_cast<OutputRow*>(child))
            row->setKeyFocus(index == outputFocus);
        ++index;
    }
}

void MainComponent::toggleFocusedOutput()
{
    if (visual.isWebView())
    {
        visual.evaluate("window.wavisToggleFocused ? window.wavisToggleFocused() : ''", {});
        return;
    }
    int index = 0;
    for (auto* child : outputList.getChildren())
    {
        auto* row = dynamic_cast<OutputRow*>(child);
        if (row == nullptr)
            continue;
        if (index == outputFocus)
        {
            const auto outputs = engine.getOutputs();
            for (const auto& output : outputs)
            {
                if (output.id == row->getId())
                {
                    engine.setOutputSelected(output.id, !output.selected);
                    return;
                }
            }
        }
        ++index;
    }
}

void MainComponent::syncFeedback()
{
    const auto hits = engine.getFeedbackOutputIds();
    const bool show = !hits.empty();
    feedback.setVisible(show);
    muteFeedback.setVisible(show);
    if (show)
        feedback.setText("Feedback loop: this system/browser input is routed back to the device being captured.",
                         juce::dontSendNotification);
}

void MainComponent::timerCallback()
{
    if (stageOpen && escapeGrab.consumeEscape())
        setStage(false);

    const float target = engine.getInputLevel();
    displayedLevel = std::max(target, displayedLevel * 0.85f);
    levelReadout.setText(juce::String(juce::roundToInt(displayedLevel * 100.0f)) + "%", juce::dontSendNotification);
    refreshDisplays();
    refreshStats();
    repaint();

    if (!demo)
        return;
    ++demoTicks;

    const bool ready = visual.hasLiveFrames();
    if (demoShots == 0 && ready && demoTicks >= 40)
    {
        visual.setMode("oscilloscope");
        modes.setSelectedId(1, juce::dontSendNotification);
        visual.requestCapture(demoDir.getChildFile("wavis_waveform.png"));
        demoShots = 1;
    }
    else if (demoShots == 1 && demoTicks >= 85)
    {
        visual.setMode("spectrum");
        modes.setSelectedId(2, juce::dontSendNotification);
        engine.setVisualMode("spectrum");
        demoShots = 2;
    }
    else if (demoShots == 2 && demoTicks >= 115)
    {
        visual.requestCapture(demoDir.getChildFile("wavis_spectrum.png"));
        demoShots = 3;
    }
    else if (demoShots == 3 && demoTicks >= 140)
    {
        visual.setMode("radial");
        modes.setSelectedId(3, juce::dontSendNotification);
        engine.setVisualMode("radial");
        demoShots = 4;
    }
    else if (demoShots == 4 && demoTicks >= 165 && !demoRoomShown)
    {
        visual.evaluate("window.wavisDemoSpeakers&&window.wavisDemoSpeakers(["
                        "{id:'demo-left',label:'Left',x:-1.7,z:-0.4,placed:true,selected:true,status:'Live',latencyMs:48,level:0.72,gain:0.8},"
                        "{id:'demo-right',label:'Right',x:1.6,z:-0.4,placed:true,selected:true,status:'Live',latencyMs:51,level:0.28,gain:0.8},"
                        "{id:'demo-rear',label:'Rear',x:0.15,z:1.35,placed:true,selected:false,status:'Off',latencyMs:0,level:0,gain:0.5},"
                        "{id:'demo-buds',label:'Buds',placed:false,selected:false,status:'Off',bluetooth:true,gain:0.8}])",
                        {});
        demoRoomShown = true;
    }
    else if (demoShots == 4 && demoTicks >= 185 && !demoRoomMoved)
    {
        visual.evaluate("window.wavisMoveSpeaker?window.wavisMoveSpeaker('demo-right',0.4,1.2):''",
                        [](juce::String text)
                        {
                            std::cout << "WAVIS_SPEAKER " << text << "\n";
                        });
        visual.evaluate("window.wavisRoomState?window.wavisRoomState():''",
                        [](juce::String text)
                        {
                            std::cout << "WAVIS_ROOM " << text << "\n";
                        });
        demoRoomMoved = true;
    }
    else if (demoShots == 4 && demoTicks >= 200)
    {
        visual.requestCapture(demoDir.getChildFile("wavis_scene.png"));
        demoShots = 5;
    }
    else if (demoShots == 5 && demoTicks >= 245)
    {
        visual.setMode("radial");
        modes.setSelectedId(3, juce::dontSendNotification);
        visual.evaluate("window.wavisDemoSpeakers&&window.wavisDemoSpeakers([])", {});
        visual.evaluate(probeScript("radial", 28.0, 16.0, 14.0, 1.0, 22.0, 18.0), {});
        demoShots = 6;
        lumaPhaseTick = demoTicks;
    }
    else if (demoShots == 6)
    {
        if (demoTicks >= lumaPhaseTick + 24 && ((demoTicks - lumaPhaseTick) % 8) == 0)
            sampleLuma("loud");
        if (demoTicks >= lumaPhaseTick + 64)
        {
            visual.requestCapture(demoDir.getChildFile("wavis_loud.png"));
            demoShots = 7;
            lumaPhaseTick = demoTicks;
        }
    }
    else if (demoShots == 7)
    {
        const double t = (demoTicks - lumaPhaseTick) / 15.0;
        const double hit = std::pow(std::max(0.0, std::sin(t * 2.4)), 6.0);
        const double bass = 0.3 + 18.0 * hit;
        const double mid = 0.2 + 2.2 * std::fabs(std::sin(t * 1.1));
        const double treble = 0.05 + 1.6 * std::fabs(std::sin(t * 6.5));
        const double level = 0.12 + 0.85 * hit;
        visual.evaluate(probeScript("radial", bass, mid, treble, level, bass, treble), {});
        if (((demoTicks - lumaPhaseTick) % 8) == 0)
            sampleLuma("music");
        if (demoTicks >= lumaPhaseTick + 75)
        {
            visual.requestCapture(demoDir.getChildFile("wavis_music.png"));
            demoShots = 8;
            lumaPhaseTick = demoTicks;
        }
    }
    else if ((demoShots >= 8 && demoTicks >= lumaPhaseTick + 10) || demoTicks >= std::max(demoSeconds * 15, 640))
    {
        printLumaSummary();
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
}

void MainComponent::sampleLuma(const char* phase)
{
    if (lumaBusy)
        return;
    lumaBusy = true;
    const std::string label = phase;
    visual.evaluate("window.wavisLuminance ? String(window.wavisLuminance()) : ''",
                    [this, label](juce::String text)
                    {
                        lumaBusy = false;
                        if (text.isEmpty())
                            return;
                        const double value = text.getDoubleValue();
                        if (value < 0.0)
                            return;
                        lumaSamples.push_back({ label, value });
                        std::cout << "WAVIS_LUMA " << label << " " << value << "\n";
                    });
}

void MainComponent::printLumaSummary()
{
    const auto range = [this](const std::string& phase, double& lo, double& hi, int& count)
    {
        lo = 1.0e9;
        hi = -1.0;
        count = 0;
        for (const auto& sample : lumaSamples)
        {
            if (sample.phase != phase)
                continue;
            lo = std::min(lo, sample.value);
            hi = std::max(hi, sample.value);
            ++count;
        }
    };

    double loudLo = 0.0, loudHi = 0.0, musicLo = 0.0, musicHi = 0.0;
    int loudCount = 0, musicCount = 0;
    range("loud", loudLo, loudHi, loudCount);
    range("music", musicLo, musicHi, musicCount);
    constexpr double ceiling = 0.62;
    const bool loudOk = loudCount > 0 && loudHi < ceiling;
    const bool musicOk = musicCount > 1 && musicHi < ceiling && (musicHi - musicLo) > 0.015;
    std::cout << "WAVIS_LUMA_SUMMARY loud n=" << loudCount << " min=" << loudLo << " max=" << loudHi
              << " music n=" << musicCount << " min=" << musicLo << " max=" << musicHi
              << " ceiling=" << ceiling << "\n";
    std::cout << (loudOk && musicOk ? "WAVIS_LUMA_OK\n" : "WAVIS_LUMA_FAIL\n");
}

std::vector<MonitorDesc> MainComponent::collectMonitors() const
{
    std::vector<MonitorDesc> found;
    const auto& displays = juce::Desktop::getInstance().getDisplays().displays;
    for (const auto& display : displays)
    {
        const auto physical = display.physicalBounds;
        const auto logical = display.logicalBounds.getSmallestIntegerContainer();
        MonitorDesc monitor;
        monitor.id = monitorId(physical.getX(), physical.getY(), physical.getWidth(), physical.getHeight());
        monitor.x = logical.getX();
        monitor.y = logical.getY();
        monitor.width = std::max(1, logical.getWidth());
        monitor.height = std::max(1, logical.getHeight());
        monitor.main = display.isMain;
        found.push_back(std::move(monitor));
    }
    return found;
}

void MainComponent::refreshDisplays()
{
    const auto next = collectMonitors();
    const bool same = next.size() == monitors.size()
                      && std::equal(next.begin(), next.end(), monitors.begin(),
                                    [](const MonitorDesc& a, const MonitorDesc& b) { return a.id == b.id && a.main == b.main; });
    monitors = next;

    if (stageAfterHotplug(stageOpen, selectedMonitorId, monitors) == StageDecision::exitToMainWindow)
        setStage(false);

    if (indexOfMonitor(monitors, selectedMonitorId) < 0)
    {
        const int fallback = defaultMonitorIndex(monitors);
        selectedMonitorId = fallback >= 0 ? monitors[static_cast<size_t>(fallback)].id : std::string();
    }

    if (!same)
    {
        buildingUi = true;
        displayBox.clear(juce::dontSendNotification);
        int id = 1;
        int selected = 1;
        for (const auto& monitor : monitors)
        {
            displayBox.addItem(monitorLabel(id, monitor.width, monitor.height, monitor.main), id);
            if (monitor.id == selectedMonitorId)
                selected = id;
            ++id;
        }
        if (displayBox.getNumItems() > 0)
            displayBox.setSelectedId(selected, juce::dontSendNotification);
        buildingUi = false;
    }

    const int index = indexOfMonitor(monitors, selectedMonitorId);
    stageButton.setButtonText(fullscreenToggleLabel(index >= 0 ? index + 1 : 1));
    stageButton.setEnabled(!monitors.empty());

    if (stageOpen && stage != nullptr && index >= 0)
    {
        const auto& monitor = monitors[static_cast<size_t>(index)];
        stage->setBounds(monitor.x, monitor.y, monitor.width, monitor.height);
    }
}

void MainComponent::setStage(bool enabled)
{
    if (enabled)
    {
        const int index = indexOfMonitor(monitors, selectedMonitorId);
        if (index < 0)
        {
            stageButton.setToggleState(false, juce::dontSendNotification);
            stageOpen = false;
            visual.setPictureFullscreen(false);
            escapeGrab.setArmed(false);
            return;
        }

        const auto& monitor = monitors[static_cast<size_t>(index)];
        if (stage == nullptr)
        {
            stage = std::make_unique<StageWindow>();
            stage->onExit = [this] { setStage(false); };
        }

        stage->setContentNonOwned(&visual, false);
        stage->setBounds(monitor.x, monitor.y, std::max(1, monitor.width), std::max(1, monitor.height));
        stage->setVisible(true);
        stage->toFront(true);
        stage->grabKeyboardFocus();
        visual.grabKeyboardFocus();
        stageOpen = true;
    }
    else
    {
        if (stage != nullptr)
        {
            stage->clearContentComponent();
            stage->setVisible(false);
        }
        addAndMakeVisible(visual);
        stageOpen = false;
        resized();
    }

    stageButton.setToggleState(stageOpen, juce::dontSendNotification);
    visual.setPictureFullscreen(stageOpen);
    escapeGrab.setArmed(stageOpen);
    const int index = indexOfMonitor(monitors, selectedMonitorId);
    stageButton.setButtonText(fullscreenToggleLabel(index >= 0 ? index + 1 : 1));
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101218));
    auto meter = meterBounds;
    g.setColour(juce::Colour(0xff23262f));
    g.fillRoundedRectangle(meter.toFloat(), 5.0f);
    g.setColour(juce::Colour(0xff5cefff));
    auto filled = meter.withWidth(juce::roundToInt(meter.getWidth() * displayedLevel));
    g.fillRoundedRectangle(filled.toFloat(), 5.0f);
    if (stageOpen)
    {
        g.setColour(juce::Colour(0xff9b978e));
        g.drawFittedText("Picture is on " + stageButton.getButtonText() + ". Esc returns it here.",
                         getLocalBounds().withTrimmedLeft(420).reduced(24), juce::Justification::centred, 3);
    }
}

void MainComponent::resized()
{
    if (!stageOpen)
        visual.setBounds(getLocalBounds().withTrimmedLeft(400));
    layoutControls();
}

void MainComponent::layoutControls()
{
    syncFeedback();

    auto area = juce::Rectangle<int>(16, 14, 368, getHeight() - 28);
    title.setBounds(area.removeFromTop(24));
    subtitle.setBounds(area.removeFromTop(18));
    area.removeFromTop(8);
    auto header = area.removeFromTop(28);
    fpsLabel.setBounds(header.removeFromRight(70));
    driverLabel.setBounds(area.removeFromTop(16));
    drivers.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);
    inputLabel.setBounds(area.removeFromTop(16));
    inputs.setBounds(area.removeFromTop(28));
    auto fileRow = area.removeFromTop(28);
    fileButton.setBounds(fileRow.removeFromLeft(110).reduced(0, 2));
    fileName.setBounds(fileRow);
    area.removeFromTop(8);
    auto transport = area.removeFromTop(32);
    startButton.setBounds(transport.removeFromLeft(120).reduced(0, 1));
    transport.removeFromLeft(8);
    stopButton.setBounds(transport.removeFromLeft(90).reduced(0, 1));
    area.removeFromTop(8);
    auto meterRow = area.removeFromTop(16);
    meterLabel.setBounds(meterRow.removeFromLeft(50));
    levelReadout.setBounds(meterRow.removeFromRight(42));
    meterBounds = meterRow.reduced(0, 3);
    area.removeFromTop(8);
    modeLabel.setBounds(area.removeFromTop(16));
    modes.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    displayLabel.setBounds(area.removeFromTop(16));
    displayBox.setBounds(area.removeFromTop(28));
    stageButton.setBounds(area.removeFromTop(28));
    backendLabel.setBounds(area.removeFromTop(18));
    area.removeFromTop(6);
    auto outHead = area.removeFromTop(16);
    outputLabel.setBounds(outHead.removeFromLeft(80));
    outputCount.setBounds(outHead);
    hint.setBounds(area.removeFromTop(48));
    if (feedback.isVisible())
    {
        feedback.setBounds(area.removeFromTop(36));
        muteFeedback.setBounds(area.removeFromTop(28).removeFromLeft(160));
    }
    status.setBounds(area.removeFromBottom(40));
    area.removeFromTop(4);
    if (viewport.isVisible())
        viewport.setBounds(area);
    else
        viewport.setBounds(0, 0, 0, 0);
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getTextCharacter() == '1')
    {
        modes.setSelectedId(1);
        return true;
    }
    if (key.getTextCharacter() == '2')
    {
        modes.setSelectedId(2);
        return true;
    }
    if (key.getTextCharacter() == '3')
    {
        modes.setSelectedId(3);
        return true;
    }
    if (visual.isWebView() && (key == juce::KeyPress::leftKey || key == juce::KeyPress::upKey))
    {
        visual.evaluate("window.wavisFocusStep?window.wavisFocusStep(-1):''", {});
        return true;
    }
    if (visual.isWebView() && (key == juce::KeyPress::rightKey || key == juce::KeyPress::downKey))
    {
        visual.evaluate("window.wavisFocusStep?window.wavisFocusStep(1):''", {});
        return true;
    }
    if (!visual.isWebView() && key == juce::KeyPress::upKey)
    {
        moveOutputFocus(-1);
        return true;
    }
    if (!visual.isWebView() && key == juce::KeyPress::downKey)
    {
        moveOutputFocus(1);
        return true;
    }
    if (key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey)
    {
        toggleFocusedOutput();
        return true;
    }
    if (key.getTextCharacter() == 'f' || key.getTextCharacter() == 'F')
    {
        setStage(!stageOpen);
        return true;
    }
    if (key == juce::KeyPress::escapeKey && stageOpen)
    {
        setStage(false);
        return true;
    }
    return false;
}
