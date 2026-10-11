#include "SystemInputs.h"

#include "../core/SourceClassifier.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#if WAVIS_HAS_PULSE
 #include <pulse/pulseaudio.h>
 #include <pulse/simple.h>
#endif

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <mmdeviceapi.h>
 #include <audioclient.h>
 #include <functiondiscoverykeys_devpkey.h>
 #include <propsys.h>
 #include <objbase.h>
#endif

namespace
{
void pushStereo(OutputFanout& fanout, SpscRingBuffer<float>& visual, std::atomic<float>& level,
                const float* interleaved, int frames)
{
    fanout.push(interleaved, frames);
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        const float l = interleaved[i * 2];
        const float r = interleaved[i * 2 + 1];
        const float mono = 0.5f * (l + r);
        visual.push(&mono, 1);
        peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
    }
    level.store(peak, std::memory_order_relaxed);
}

#if WAVIS_HAS_PULSE
struct PulseItem
{
    std::string name;
    std::string description;
};

struct PulseScan
{
    std::vector<PulseItem>* items = nullptr;
    bool listDone = false;
    pa_context_state_t state = PA_CONTEXT_UNCONNECTED;
};

std::vector<PulseItem> listPulseSources()
{
    std::vector<PulseItem> items;
    pa_mainloop* loop = pa_mainloop_new();
    if (loop == nullptr)
        return items;

    pa_context* context = pa_context_new(pa_mainloop_get_api(loop), "Wavis");
    if (context == nullptr)
    {
        pa_mainloop_free(loop);
        return items;
    }

    PulseScan scan;
    scan.items = &items;
    pa_context_set_state_callback(context, [](pa_context* ctx, void* userdata)
    {
        static_cast<PulseScan*>(userdata)->state = pa_context_get_state(ctx);
    }, &scan);

    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
    {
        pa_context_unref(context);
        pa_mainloop_free(loop);
        return items;
    }

    const auto began = std::chrono::steady_clock::now();
    while (scan.state != PA_CONTEXT_READY && scan.state != PA_CONTEXT_FAILED && scan.state != PA_CONTEXT_TERMINATED)
    {
        pa_mainloop_iterate(loop, 0, nullptr);
        if (std::chrono::steady_clock::now() - began > std::chrono::milliseconds(400))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (scan.state == PA_CONTEXT_READY)
    {
        pa_operation* op = pa_context_get_source_info_list(context,
            [](pa_context*, const pa_source_info* info, int eol, void* userdata)
            {
                auto* scanState = static_cast<PulseScan*>(userdata);
                if (eol != 0)
                {
                    scanState->listDone = true;
                    return;
                }
                if (info != nullptr && info->name != nullptr)
                    scanState->items->push_back({ info->name, info->description != nullptr ? info->description : "" });
            }, &scan);

        const auto listed = std::chrono::steady_clock::now();
        while (op != nullptr && !scan.listDone)
        {
            pa_mainloop_iterate(loop, 0, nullptr);
            if (std::chrono::steady_clock::now() - listed > std::chrono::milliseconds(400))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (op != nullptr)
            pa_operation_unref(op);
    }

    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(loop);
    return items;
}

class PulseInput final : public ExtraInputStream
{
public:
    PulseInput(pa_simple* opened, OutputFanout& outputs, SpscRingBuffer<float>& visualRing, std::atomic<float>& peak)
        : stream(opened), fanout(outputs), visual(visualRing), level(peak)
    {
        worker = std::thread([this] { run(); });
    }

    ~PulseInput() override
    {
        stop.store(true);
        if (worker.joinable())
            worker.join();
        if (stream != nullptr)
            pa_simple_free(stream);
    }

private:
    void run()
    {
        std::vector<float> block(256 * 2);
        while (!stop.load())
        {
            int error = 0;
            if (pa_simple_read(stream, block.data(), block.size() * sizeof(float), &error) < 0)
                break;
            pushStereo(fanout, visual, level, block.data(), 256);
        }
    }

    pa_simple* stream = nullptr;
    OutputFanout& fanout;
    SpscRingBuffer<float>& visual;
    std::atomic<float>& level;
    std::atomic<bool> stop { false };
    std::thread worker;
};
#endif

#if defined(_WIN32)
std::string wideToUtf8(LPCWSTR text)
{
    if (text == nullptr)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& text)
{
    if (text.empty())
        return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (chars <= 1)
        return {};
    std::wstring out(static_cast<size_t>(chars - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), chars);
    return out;
}

bool isFloatFormat(const WAVEFORMATEX* format)
{
    if (format == nullptr)
        return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        const GUID ieee = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
        return IsEqualGUID(ext->SubFormat, ieee) != 0;
    }
    return false;
}

class WasapiLoopback final : public ExtraInputStream
{
public:
    WasapiLoopback(const std::wstring& endpointId, OutputFanout& fanout, SpscRingBuffer<float>& visual,
                   std::atomic<float>& level, std::string& error)
        : endpointId(endpointId), fanout(fanout), visual(visual), level(level)
    {
        worker = std::thread([this] { run(); });
        for (int i = 0; i < 50 && !started.load(std::memory_order_acquire) && !failed.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (failed.load(std::memory_order_acquire))
            error = failure;
        else if (!started.load(std::memory_order_acquire))
            error = "Timed out opening WASAPI loopback.";
    }

    ~WasapiLoopback() override
    {
        stop.store(true);
        if (worker.joinable())
            worker.join();
    }

    bool ok() const noexcept { return started.load() && !failed.load(); }
    int sampleRateHz() const noexcept { return rateHz.load(); }

private:
    void run()
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* format = nullptr;

        auto fail = [&](const char* message)
        {
            failure = message;
            failed.store(true, std::memory_order_release);
        };

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator))))
            fail("WASAPI enumerator was not created.");
        else if (FAILED(enumerator->GetDevice(endpointId.c_str(), &device)))
            fail("That loopback endpoint is gone.");
        else if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))))
            fail("Could not activate the loopback client.");
        else if (FAILED(client->GetMixFormat(&format)))
            fail("Could not read the mix format.");
        else
        {
            const REFERENCE_TIME buffer = 200000;
            const HRESULT opened = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, buffer, 0,
                                                      format, nullptr);
            if (FAILED(opened))
                fail("WASAPI refused loopback capture on that device.");
            else if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))))
                fail("WASAPI capture service is unavailable.");
            else if (FAILED(client->Start()))
                fail("WASAPI loopback did not start.");
            else
            {
                if (format->nSamplesPerSec > 0)
                    rateHz.store(static_cast<int>(format->nSamplesPerSec));
                started.store(true, std::memory_order_release);
            }
        }

        const bool useFloat = isFloatFormat(format);
        const int channels = format != nullptr ? static_cast<int>(format->nChannels) : 0;
        std::vector<float> block(256 * 2);

        while (started.load() && !stop.load() && !failed.load())
        {
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet)))
                break;
            if (packet == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;

            UINT32 offset = 0;
            while (offset < frames)
            {
                const UINT32 count = std::min<UINT32>(256, frames - offset);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    std::fill(block.begin(), block.begin() + count * 2, 0.0f);
                }
                else if (useFloat)
                {
                    const auto* src = reinterpret_cast<const float*>(data) + offset * static_cast<UINT32>(channels);
                    for (UINT32 i = 0; i < count; ++i)
                    {
                        const float l = channels > 0 ? src[i * channels] : 0.0f;
                        const float r = channels > 1 ? src[i * channels + 1] : l;
                        block[i * 2] = l;
                        block[i * 2 + 1] = r;
                    }
                }
                else
                {
                    const auto* src = reinterpret_cast<const int16_t*>(data) + offset * static_cast<UINT32>(channels);
                    for (UINT32 i = 0; i < count; ++i)
                    {
                        const float l = channels > 0 ? src[i * channels] / 32768.0f : 0.0f;
                        const float r = channels > 1 ? src[i * channels + 1] / 32768.0f : l;
                        block[i * 2] = l;
                        block[i * 2 + 1] = r;
                    }
                }
                pushStereo(fanout, visual, level, block.data(), static_cast<int>(count));
                offset += count;
            }

            capture->ReleaseBuffer(frames);
        }

        if (client != nullptr && started.load())
            client->Stop();
        if (format != nullptr)
            CoTaskMemFree(format);
        if (capture != nullptr)
            capture->Release();
        if (client != nullptr)
            client->Release();
        if (device != nullptr)
            device->Release();
        if (enumerator != nullptr)
            enumerator->Release();
        if (SUCCEEDED(init))
            CoUninitialize();
    }

    std::wstring endpointId;
    OutputFanout& fanout;
    SpscRingBuffer<float>& visual;
    std::atomic<float>& level;
    std::string failure;
    std::atomic<bool> stop { false };
    std::atomic<bool> started { false };
    std::atomic<bool> failed { false };
    std::atomic<int> rateHz { 48000 };
    std::thread worker;
};

std::vector<ExtraInput> enumerateWasapiLoopback()
{
    std::vector<ExtraInput> inputs;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
    {
        if (SUCCEEDED(init))
            CoUninitialize();
        return inputs;
    }

    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
    {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)))
                continue;
            LPWSTR id = nullptr;
            device->GetId(&id);
            IPropertyStore* store = nullptr;
            std::string friendly;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)))
            {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                    friendly = wideToUtf8(value.pwszVal);
                PropVariantClear(&value);
                store->Release();
            }

            ExtraInput extra;
            extra.driver = "WASAPI Loopback";
            extra.deviceName = wideToUtf8(id);
            extra.label = "System/Browser audio — " + (friendly.empty() ? extra.deviceName : friendly) + " (loopback)";
            if (!extra.deviceName.empty())
                inputs.push_back(extra);
            if (id != nullptr)
                CoTaskMemFree(id);
            device->Release();
        }
        collection->Release();
    }

    enumerator->Release();
    if (SUCCEEDED(init))
        CoUninitialize();
    return inputs;
}
#endif
} // namespace

std::vector<ExtraInput> enumerateExtraInputs()
{
    std::vector<ExtraInput> inputs;

#if WAVIS_HAS_PULSE
    for (const auto& source : listPulseSources())
    {
        if (!isPulseMonitor(source.name, source.description))
            continue;
        ExtraInput extra;
        extra.driver = "PulseAudio";
        extra.deviceName = source.name;
        extra.label = labelPulseSource(source.name, source.description);
        inputs.push_back(std::move(extra));
    }
#endif

#if defined(_WIN32)
    auto loopback = enumerateWasapiLoopback();
    inputs.insert(inputs.end(), loopback.begin(), loopback.end());
#endif

    return inputs;
}

bool isExtraInputDriver(const std::string& driver)
{
    return driver == "PulseAudio" || driver == "WASAPI Loopback";
}

std::unique_ptr<ExtraInputStream> openExtraInput(const std::string& driver, const std::string& deviceName,
                                                 OutputFanout& fanout, SpscRingBuffer<float>& visualMono,
                                                 std::atomic<float>& level, double& sampleRate, std::string& error)
{
#if WAVIS_HAS_PULSE
    if (driver == "PulseAudio")
    {
        pa_sample_spec spec;
        spec.format = PA_SAMPLE_FLOAT32LE;
        spec.rate = 48000;
        spec.channels = 2;
        int pulseError = 0;
        pa_simple* stream = pa_simple_new(nullptr, "Wavis", PA_STREAM_RECORD, deviceName.c_str(), "capture", &spec,
                                          nullptr, nullptr, &pulseError);
        if (stream == nullptr)
        {
            error = "PulseAudio did not open that monitor (" + std::string(pa_strerror(pulseError)) + ").";
            return nullptr;
        }
        sampleRate = 48000.0;
        return std::make_unique<PulseInput>(stream, fanout, visualMono, level);
    }
#else
    (void) driver;
#endif

#if defined(_WIN32)
    if (driver == "WASAPI Loopback")
    {
        auto stream = std::make_unique<WasapiLoopback>(utf8ToWide(deviceName), fanout, visualMono, level, error);
        if (!stream->ok())
            return nullptr;
        sampleRate = static_cast<double>(stream->sampleRateHz());
        return stream;
    }
#endif

    error = "This build cannot open that system-audio source.";
    (void) fanout;
    (void) visualMono;
    (void) level;
    (void) sampleRate;
    (void) deviceName;
    return nullptr;
}
