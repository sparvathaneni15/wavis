#include "VisualHost.h"

#include "VisualView.h"
#include "WebStage.h"

#if JUCE_LINUX
 #include <dlfcn.h>
#endif

namespace
{
bool webViewLibraryPresent()
{
#if JUCE_LINUX
    const char* libraries[] = { "libwebkit2gtk-4.1.so", "libwebkit2gtk-4.1.so.0", "libwebkit2gtk-4.0.so.0",
                                "libwebkit2gtk-4.0.so" };
    for (const char* name : libraries)
    {
        if (void* handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL))
        {
            dlclose(handle);
            return true;
        }
    }
    return false;
#elif JUCE_MAC
    return true;
#elif JUCE_WINDOWS
 #if JUCE_USE_WIN_WEBVIEW2
    const auto options = juce::WebBrowserComponent::Options{}.withBackend(juce::WebBrowserComponent::Options::Backend::webview2);
    return juce::WebBrowserComponent::areOptionsSupported(options);
 #else
    return false;
 #endif
#else
    return false;
#endif
}
} // namespace

VisualHost::VisualHost(AudioEngine& engineIn) : audio(engineIn)
{
    if (webViewLibraryPresent())
        installWeb();
    else
        installFallback();
}

VisualHost::~VisualHost() = default;

void VisualHost::installWeb()
{
    gl.reset();
    web = std::make_unique<WebStage>(audio, [this] { if (exitFullscreen) exitFullscreen(); }, [this] {
        if (fallingBack)
            return;
        fallingBack = true;
        juce::MessageManager::callAsync([this] { installFallback(); });
    });
    web->setMode(mode);
    web->setPictureFullscreen(pictureFullscreen);
    addAndMakeVisible(web.get());
    resized();
}

void VisualHost::installFallback()
{
    web.reset();
    gl = std::make_unique<VisualView>(audio);
    gl->setMode(mode);
    addAndMakeVisible(gl.get());
    resized();
}

void VisualHost::setPictureFullscreen(bool enabled)
{
    pictureFullscreen = enabled;
    if (web != nullptr)
        web->setPictureFullscreen(enabled);
}

void VisualHost::setMode(const std::string& next)
{
    mode = next == "spectrum" || next == "radial" ? next : "oscilloscope";
    if (web != nullptr)
        web->setMode(mode);
    if (gl != nullptr)
        gl->setMode(mode);
}

void VisualHost::requestCapture(const juce::File& file)
{
    if (web != nullptr)
        web->requestCapture(file);
    else if (gl != nullptr)
        gl->requestCapture(file);
}

void VisualHost::evaluate(const juce::String& script, std::function<void(juce::String)> done)
{
    if (web != nullptr)
        web->evaluate(script, std::move(done));
    else if (done)
        done({});
}

int VisualHost::getFramesPerSecond() const noexcept
{
    if (web != nullptr)
        return web->getFramesPerSecond();
    if (gl != nullptr)
        return gl->getFramesPerSecond();
    return 0;
}

bool VisualHost::hasLiveFrames() const noexcept
{
    if (web != nullptr)
        return web->hasLiveFrames();
    return gl != nullptr;
}

std::string VisualHost::backendLabel() const
{
    if (web != nullptr)
        return "Three.js · " + WebStage::backendName();
    return "OpenGL fallback";
}

void VisualHost::setExitFullscreen(std::function<void()> callback)
{
    exitFullscreen = std::move(callback);
}

void VisualHost::resized()
{
    if (web != nullptr)
        web->setBounds(getLocalBounds());
    if (gl != nullptr)
        gl->setBounds(getLocalBounds());
}
