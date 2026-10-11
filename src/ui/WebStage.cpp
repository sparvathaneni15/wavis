#include "WebStage.h"

#include "../core/VisualFrame.h"

#include <BinaryData.h>

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
std::optional<juce::WebBrowserComponent::Resource> serveBundledPage(const juce::String& path)
{
    const char* data = nullptr;
    int size = 0;
    const char* mime = "text/plain";

    if (path.isEmpty() || path == "/" || path == "/index.html")
    {
        data = BinaryData::index_html;
        size = BinaryData::index_htmlSize;
        mime = "text/html";
    }
    else if (path == "/app.js")
    {
        data = BinaryData::app_js;
        size = BinaryData::app_jsSize;
        mime = "text/javascript";
    }
    else
        return std::nullopt;

    juce::WebBrowserComponent::Resource resource;
    resource.mimeType = mime;
    resource.data.resize(static_cast<size_t>(size));
    if (size > 0)
        std::memcpy(resource.data.data(), data, static_cast<size_t>(size));
    return resource;
}

void writePngCapture(const juce::File& file, const juce::String& dataUrl)
{
    const auto payload = dataUrl.fromFirstOccurrenceOf(",", false, false);
    juce::MemoryOutputStream decoded;
    if (payload.isEmpty() || !juce::Base64::convertFromBase64(decoded, payload))
    {
        std::cout << "WAVIS_FRAME_VARIANCE 0 " << file.getFullPathName() << "\n";
        return;
    }

    file.getParentDirectory().createDirectory();
    file.replaceWithData(decoded.getData(), decoded.getDataSize());

    const auto image = juce::ImageFileFormat::loadFrom(file);
    double sum = 0.0;
    double sumSq = 0.0;
    int samples = 0;
    if (image.isValid())
    {
        for (int y = 0; y < image.getHeight(); y += 4)
        {
            for (int x = 0; x < image.getWidth(); x += 4)
            {
                const auto colour = image.getPixelAt(x, y);
                const double lum = static_cast<double>(colour.getRed() + colour.getGreen() + colour.getBlue());
                sum += lum;
                sumSq += lum * lum;
                ++samples;
            }
        }
    }

    const double variance = samples > 0 ? sumSq / samples - (sum / samples) * (sum / samples) : 0.0;
    std::cout << "WAVIS_FRAME_VARIANCE " << variance << " " << file.getFullPathName() << "\n";
}
} // namespace

juce::WebBrowserComponent::Options WebStage::makeOptions(WebStage& owner)
{
    auto options = juce::WebBrowserComponent::Options{}
                       .withNativeIntegrationEnabled()
                       .withKeepPageLoadedWhenBrowserIsHidden()
                       .withResourceProvider([](const juce::String& path) { return serveBundledPage(path); })
                       .withNativeFunction("exitFullscreen", [&owner](const juce::Array<juce::var>&, auto completion)
                                           {
                                               if (owner.exitFullscreen)
                                                   owner.exitFullscreen();
                                               completion({});
                                           })
                       .withNativeFunction("webglUnavailable", [&owner](const juce::Array<juce::var>&, auto completion)
                                           {
                                               if (owner.webglFailed)
                                                   owner.webglFailed();
                                               completion({});
                                           })
                       .withNativeFunction("placeSpeaker", [&owner](const juce::Array<juce::var>& args, auto completion)
                                           {
                                               if (args.size() >= 3)
                                               {
                                                   const auto id = args[0].toString().toStdString();
                                                   const float x = static_cast<float>(static_cast<double>(args[1]));
                                                   const float z = static_cast<float>(static_cast<double>(args[2]));
                                                   owner.audio.setSpeakerPosition(id, x, z);
                                               }
                                               completion({});
                                           });

#if JUCE_WINDOWS && JUCE_USE_WIN_WEBVIEW2
    options = options.withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
                  .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}
                                              .withBackgroundColour(juce::Colours::black)
                                              .withStatusBarDisabled());
#endif
    return options;
}

class WebStage::Browser : public juce::WebBrowserComponent
{
public:
    explicit Browser(WebStage& ownerIn)
        : juce::WebBrowserComponent(WebStage::makeOptions(ownerIn)), owner(ownerIn)
    {
    }

    void pageFinishedLoading(const juce::String& url) override
    {
        if (url.contains("juce.backend"))
            owner.pageReady = true;
    }

    bool pageLoadHadNetworkError(const juce::String& errorInfo) override
    {
        std::cerr << "Wavis page error " << errorInfo << "\n";
        return true;
    }

    void visibilityChanged() override
    {
        juce::WebBrowserComponent::visibilityChanged();
        owner.navigateOnce();
    }

private:
    WebStage& owner;
};

WebStage::WebStage(AudioEngine& engine, std::function<void()> onExit, std::function<void()> onWebgl)
    : audio(engine), exitFullscreen(std::move(onExit)), webglFailed(std::move(onWebgl))
{
    setOpaque(true);
    fft = std::make_unique<juce::dsp::FFT>(11);
    window.resize(static_cast<size_t>(fft->getSize()));
    for (size_t i = 0; i < window.size(); ++i)
        window[i] = 0.5f * (1.0f - std::cos(6.2831853f * static_cast<float>(i) / static_cast<float>(window.size())));
    fftBuffer.assign(static_cast<size_t>(fft->getSize() * 2), 0.0f);

    browser = std::make_unique<Browser>(*this);
    addAndMakeVisible(browser.get());
    std::cerr << "Wavis visual backend " << backendName() << "\n";
    startTimerHz(60);
}

WebStage::~WebStage()
{
    stopTimer();
}

std::string WebStage::backendName()
{
#if JUCE_WINDOWS
    return "WebView2";
#elif JUCE_MAC
    return "WKWebView";
#else
    return "WebKitGTK";
#endif
}

void WebStage::setMode(const std::string& next)
{
    mode = next == "spectrum" || next == "radial" ? next : "oscilloscope";
}

void WebStage::setPictureFullscreen(bool enabled)
{
    pictureFullscreen = enabled;
}

void WebStage::resized()
{
    if (browser != nullptr)
        browser->setBounds(getLocalBounds());
    navigateOnce();
}

void WebStage::navigateOnce()
{
    if (navigated || browser == nullptr || !isShowing())
        return;
    navigated = true;
    browser->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
}

void WebStage::timerCallback()
{
    navigateOnce();
    emitFrame();
    const double now = juce::Time::getMillisecondCounterHiRes();
    ++frameCounter;
    if (fpsStamp <= 0.0)
        fpsStamp = now;
    if (now - fpsStamp >= 500.0)
    {
        fps.store(juce::roundToInt(frameCounter * 1000.0 / (now - fpsStamp)));
        frameCounter = 0;
        fpsStamp = now;
    }
}

void WebStage::emitFrame()
{
    float block[1024];
    const int got = audio.popVisual(block, 1024);
    if (got > 0)
        history.insert(history.end(), block, block + got);
    if (static_cast<int>(history.size()) > 2048)
        history.erase(history.begin(), history.end() - 2048);

    if (!pageReady || browser == nullptr)
        return;

    float wave[kWavePoints];
    const int available = static_cast<int>(history.size());
    const int tail = std::min(available, 512);
    decimatePeaks(tail > 0 ? history.data() + (available - tail) : nullptr, tail, wave, kWavePoints);

    float bins[kSpectrumBins];
    std::fill(std::begin(bins), std::end(bins), 0.0f);
    const int fftSize = fft != nullptr ? fft->getSize() : 0;
    if (fftSize > 0 && available >= fftSize)
    {
        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        const int offset = available - fftSize;
        for (int i = 0; i < fftSize; ++i)
            fftBuffer[static_cast<size_t>(i)] = history[static_cast<size_t>(offset + i)] * window[static_cast<size_t>(i)];
        fft->performFrequencyOnlyForwardTransform(fftBuffer.data(), true);
        reduceLogBins(fftBuffer.data(), fftSize / 2, bins, kSpectrumBins);
        for (float& bin : bins)
            bin = std::max(0.0f, bin * 12.0f);
    }

    const auto bands = (fftSize > 0 && available >= fftSize)
                           ? measureBandsOpen(fftBuffer.data(), fftSize / 2, audio.getVisualSampleRate(), fftSize)
                           : SpectralBands {};

    auto* object = new juce::DynamicObject();
    object->setProperty("mode", juce::String(mode));
    object->setProperty("level", static_cast<double>(audio.getInputLevel()));

    juce::Array<juce::var> waveValues;
    juce::Array<juce::var> binValues;
    waveValues.ensureStorageAllocated(kWavePoints);
    binValues.ensureStorageAllocated(kSpectrumBins);
    for (float sample : wave)
        waveValues.add(static_cast<double>(sample));
    for (float bin : bins)
        binValues.add(static_cast<double>(bin));
    object->setProperty("wave", waveValues);
    object->setProperty("bins", binValues);
    object->setProperty("bass", static_cast<double>(bands.bass));
    object->setProperty("mid", static_cast<double>(bands.mid));
    object->setProperty("treble", static_cast<double>(bands.treble));
    object->setProperty("fullscreen", pictureFullscreen);

    juce::Array<juce::var> speakers;
    for (const auto& spot : audio.getSpeakers())
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("id", juce::String(spot.id));
        item->setProperty("label", juce::String(spot.label));
        item->setProperty("x", static_cast<double>(spot.x));
        item->setProperty("z", static_cast<double>(spot.z));
        speakers.add(juce::var(item));
    }
    object->setProperty("speakers", speakers);

    browser->emitEventIfBrowserIsVisible("wavisFrame", juce::var(object));
    framesSent.store(framesSent.load() + 1);
}

void WebStage::evaluate(const juce::String& script, std::function<void(juce::String)> done)
{
    if (browser == nullptr || !pageReady)
    {
        if (done)
            done({});
        return;
    }

    juce::Component::SafePointer<WebStage> safe(this);
    browser->evaluateJavascript(script, [safe, done = std::move(done)](juce::WebBrowserComponent::EvaluationResult result)
                                {
                                    if (safe == nullptr || !done)
                                        return;
                                    if (result.getError() != nullptr)
                                    {
                                        done({});
                                        return;
                                    }
                                    const auto* value = result.getResult();
                                    done(value != nullptr ? value->toString() : juce::String());
                                });
}

void WebStage::requestCapture(const juce::File& file)
{
    if (browser == nullptr || !pageReady)
        return;

    juce::Component::SafePointer<WebStage> safe(this);
    browser->evaluateJavascript("window.wavisCapture ? window.wavisCapture() : ''",
                                [safe, file](juce::WebBrowserComponent::EvaluationResult result)
                                {
                                    if (safe == nullptr)
                                        return;
                                    if (const auto* error = result.getError())
                                        std::cerr << "Wavis capture error: " << error->message << "\n";
                                    const auto* value = result.getResult();
                                    const auto text = value != nullptr ? value->toString() : juce::String();
                                    writePngCapture(file, text);
                                });
}
