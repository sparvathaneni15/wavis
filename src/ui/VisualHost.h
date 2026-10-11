#pragma once

#include "../engine/AudioEngine.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>

// The picture the user sees. Three.js in an embedded web view, or the OpenGL view when that
// web view cannot be created.
class VisualHost : public juce::Component
{
public:
    explicit VisualHost(AudioEngine& engine);
    ~VisualHost() override;

    void setMode(const std::string& mode);
    void setPictureFullscreen(bool enabled);
    void requestCapture(const juce::File& file);
    void evaluate(const juce::String& script, std::function<void(juce::String)> done);
    int getFramesPerSecond() const noexcept;
    bool hasLiveFrames() const noexcept;
    bool isWebView() const noexcept { return web != nullptr; }
    std::string backendLabel() const;

    void setExitFullscreen(std::function<void()> callback);
    void setBackendChanged(std::function<void()> callback);
    void resized() override;

private:
    void installWeb();
    void installFallback();

    AudioEngine& audio;
    std::function<void()> exitFullscreen;
    std::function<void()> backendChanged;
    std::unique_ptr<class WebStage> web;
    std::unique_ptr<class VisualView> gl;
    std::string mode = "oscilloscope";
    bool pictureFullscreen = false;
    bool fallingBack = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VisualHost)
};
