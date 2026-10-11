#pragma once

#include "../engine/AudioEngine.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_opengl/juce_opengl.h>

#include <atomic>
#include <vector>

class VisualView : public juce::Component, private juce::OpenGLRenderer
{
public:
    explicit VisualView(AudioEngine& engine);
    ~VisualView() override;

    void setMode(const std::string& mode);
    std::string getMode() const;
    int getFramesPerSecond() const noexcept { return fps.load(); }

    void requestCapture(const juce::File& file);

    void paint(juce::Graphics& g) override;
    void resized() override {}

private:
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    void drawOscilloscope();
    void drawSpectrum();
    void drawRadial();
    void drawVertices(GLenum mode, const std::vector<float>& vertices);
    bool ensureProgram();
    void captureIfRequested(int width, int height);

    AudioEngine& audio;
    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> program;
    unsigned int vertexArray = 0;
    std::vector<float> history;
    std::vector<float> magnitudes;
    std::vector<float> peaks;
    std::vector<float> window;
    std::vector<float> fftBuffer;
    std::vector<float> vertices;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::string mode = "oscilloscope";
    float rotation = 0.0f;
    std::atomic<int> fps { 0 };
    int frameCounter = 0;
    double fpsStamp = 0.0;
    juce::File captureFile;
    std::atomic<bool> capturePending { false };
    double lastVariance = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VisualView)
};
