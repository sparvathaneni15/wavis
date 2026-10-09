#include "MainComponent.h"

#include <algorithm>

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
                         : next.status == "Live"                                 ? juce::Colour(0xffd6ff4a)
                                                                                  : juce::Colour(0xff9b978e));
    }

    const std::string& getId() const { return choice.id; }

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
    }

private:
    AudioEngine::OutputChoice choice;
    juce::ToggleButton toggle;
    juce::Label name;
    juce::Slider gain { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::TextButton mute;
    juce::Label detail;
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
                                    static_cast<juce::Component*>(&modes), static_cast<juce::Component*>(&fullscreenButton),
                                    static_cast<juce::Component*>(&outputLabel), static_cast<juce::Component*>(&outputCount),
                                    static_cast<juce::Component*>(&hint), static_cast<juce::Component*>(&feedback),
                                    static_cast<juce::Component*>(&muteFeedback), static_cast<juce::Component*>(&status),
                                    static_cast<juce::Component*>(&viewport) })
        addAndMakeVisible(child);

    title.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    title.setColour(juce::Label::textColourId, juce::Colour(0xffd6ff4a));
    subtitle.setColour(juce::Label::textColourId, juce::Colour(0xff9b978e));
    hint.setColour(juce::Label::textColourId, juce::Colour(0xffc8c2b4));
    feedback.setColour(juce::Label::textColourId, juce::Colour(0xffffc1c6));
    status.setColour(juce::Label::textColourId, juce::Colour(0xfff4f1ea));
    fileName.setColour(juce::Label::textColourId, juce::Colour(0xff9b978e));
    for (auto* label : { &driverLabel, &inputLabel, &meterLabel, &modeLabel, &outputLabel })
        label->setColour(juce::Label::textColourId, juce::Colour(0xff9b978e));

    startButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffd6ff4a));
    startButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff14160c));
    muteFeedback.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffff5d6c));

    modes.addItem("Oscilloscope", 1);
    modes.addItem("Spectrum", 2);
    modes.addItem("Radial", 3);
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
    fullscreenButton.onClick = [this] { setProjector(!projector); };
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
    engine.addListener(this);
    rebuildInputs();
    rebuildOutputs();
    refreshStats();
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
    outputCount.setText(juce::String(selected) + " selected", juce::dontSendNotification);
    status.setText(engine.getStatusLine(), juce::dontSendNotification);
    fileName.setText(engine.getFileName(), juce::dontSendNotification);
    fpsLabel.setText(engine.isRunning() && visual.getFramesPerSecond() > 0 ? juce::String(visual.getFramesPerSecond()) + " fps"
                                                                            : juce::String("--"),
                     juce::dontSendNotification);
    syncFeedback();
    const bool busy = engine.isRunning();
    startButton.setEnabled(!busy);
    stopButton.setEnabled(busy);
}

void MainComponent::syncFeedback()
{
    const auto hits = engine.getFeedbackOutputIds();
    const bool show = !hits.empty();
    feedback.setVisible(show && !projector);
    muteFeedback.setVisible(show && !projector);
    if (show)
        feedback.setText("Feedback loop: this system/browser input is routed back to the device being captured.",
                         juce::dontSendNotification);
}

void MainComponent::timerCallback()
{
    const float target = engine.getInputLevel();
    displayedLevel = std::max(target, displayedLevel * 0.85f);
    levelReadout.setText(juce::String(juce::roundToInt(displayedLevel * 100.0f)) + "%", juce::dontSendNotification);
    refreshStats();
    repaint();

    if (!demo)
        return;
    ++demoTicks;
    if (demoTicks == 20)
    {
        demoDir.getChildFile("wavis_oscilloscope.png").deleteFile();
        visual.requestCapture(demoDir.getChildFile("wavis_oscilloscope.png"));
    }
    else if (demoTicks == 45)
    {
        visual.setMode("spectrum");
        modes.setSelectedId(2, juce::dontSendNotification);
        engine.setVisualMode("spectrum");
    }
    else if (demoTicks == 70)
        visual.requestCapture(demoDir.getChildFile("wavis_spectrum.png"));
    else if (demoTicks == 95)
    {
        visual.setMode("radial");
        modes.setSelectedId(3, juce::dontSendNotification);
        engine.setVisualMode("radial");
    }
    else if (demoTicks == 120)
        visual.requestCapture(demoDir.getChildFile("wavis_radial.png"));
    else if (demoTicks >= demoSeconds * 15)
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainComponent::setProjector(bool enabled)
{
    projector = enabled;
    fullscreenButton.setButtonText(projector ? "Exit" : "Full screen");
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setFullScreen(projector);
    resized();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101218));
    if (!projector)
    {
        auto meter = meterBounds;
        g.setColour(juce::Colour(0xff23262f));
        g.fillRoundedRectangle(meter.toFloat(), 5.0f);
        g.setColour(juce::Colour(0xffd6ff4a));
        auto filled = meter.withWidth(juce::roundToInt(meter.getWidth() * displayedLevel));
        g.fillRoundedRectangle(filled.toFloat(), 5.0f);
    }
}

void MainComponent::resized()
{
    visual.setBounds(projector ? getLocalBounds() : getLocalBounds().withTrimmedLeft(400));
    layoutControls();
}

void MainComponent::layoutControls()
{
    const bool show = !projector;
    for (auto* child : getChildren())
        if (child != &visual)
            child->setVisible(show);
    if (!show)
        return;
    syncFeedback();

    auto area = juce::Rectangle<int>(16, 14, 368, getHeight() - 28);
    title.setBounds(area.removeFromTop(24));
    subtitle.setBounds(area.removeFromTop(18));
    area.removeFromTop(8);
    auto header = area.removeFromTop(28);
    fpsLabel.setBounds(header.removeFromRight(70));
    fullscreenButton.setBounds(header.removeFromRight(110).reduced(0, 2));
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
    area.removeFromTop(8);
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
    viewport.setBounds(area);
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
    if (key.getTextCharacter() == 'f' || key.getTextCharacter() == 'F')
    {
        setProjector(!projector);
        return true;
    }
    if (key == juce::KeyPress::escapeKey && projector)
    {
        setProjector(false);
        return true;
    }
    return false;
}
