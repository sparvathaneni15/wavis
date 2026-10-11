#pragma once

#include "../engine/AudioEngine.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Three.js page. Native code pushes waveform samples and FFT bins; the page does not analyse audio.
class WebStage : public juce::Component, private juce::Timer
{
public:
    WebStage(AudioEngine& engine, std::function<void()> exitFullscreen, std::function<void()> webglFailed);
    ~WebStage() override;

    void setMode(const std::string& mode);
    void setPictureFullscreen(bool enabled);
    void requestCapture(const juce::File& file);
    void evaluate(const juce::String& script, std::function<void(juce::String)> done);
    int getFramesPerSecond() const noexcept { return fps.load(); }
    bool hasLiveFrames() const noexcept { return framesSent.load() > 0; }
    static std::string backendName();

    void resized() override;

private:
    class Browser;

    void timerCallback() override;
    void navigateOnce();
    void emitFrame();
    static juce::WebBrowserComponent::Options makeOptions(WebStage& owner);

    AudioEngine& audio;
    std::function<void()> exitFullscreen;
    std::function<void()> webglFailed;
    std::unique_ptr<Browser> browser;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> history;
    std::vector<float> window;
    std::vector<float> fftBuffer;
    std::string mode = "oscilloscope";
    bool pictureFullscreen = false;
    bool navigated = false;
    bool pageReady = false;
    std::atomic<int> fps { 0 };
    std::atomic<int> framesSent { 0 };
    int frameCounter = 0;
    double fpsStamp = 0.0;

    friend class Browser;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebStage)
};
