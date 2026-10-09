#include "core/DeviceList.h"
#include "core/DriftCorrector.h"
#include "core/OutputFanout.h"
#include "core/RoutingModel.h"
#include "core/SettingsModel.h"
#include "core/SourceClassifier.h"
#include "core/SpscRingBuffer.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
std::vector<float> stereoTone(int frames, float gain = 1.0f)
{
    std::vector<float> samples(static_cast<size_t>(frames * 2));
    for (int i = 0; i < frames; ++i)
    {
        const float left = gain * std::sin(0.07f * static_cast<float>(i));
        samples[static_cast<size_t>(i * 2)] = left;
        samples[static_cast<size_t>(i * 2 + 1)] = left * 0.5f;
    }
    return samples;
}

bool near(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}

OutputRoute selectedRoute(std::string id, bool available, LinkState state, float gain = 0.8f, bool muted = false)
{
    OutputRoute route;
    route.id = std::move(id);
    route.driver = "ALSA";
    route.deviceName = route.id;
    route.label = route.id;
    route.available = available;
    route.state = state;
    route.selected = true;
    route.gain = gain;
    route.muted = muted;
    return route;
}
} // namespace

class RingBufferTest : public juce::UnitTest
{
public:
    RingBufferTest() : juce::UnitTest("SPSC ring buffer", "Wavis") {}

    void runTest() override
    {
        beginTest("capacity is a power of two and the buffer keeps order across a wrap");
        SpscRingBuffer<int> ring(6);
        expect(ring.getCapacity() == 8);
        int first[5] = { 1, 2, 3, 4, 5 };
        expectEquals(ring.push(first, 5), 5);
        int out[3] = {};
        expectEquals(ring.pop(out, 3), 3);
        expectEquals(out[0], 1);
        expectEquals(out[2], 3);
        int more[4] = { 6, 7, 8, 9 };
        expectEquals(ring.push(more, 4), 4);
        int all[8] = {};
        expectEquals(ring.pop(all, 8), 6);
        expectEquals(all[0], 4);
        expectEquals(all[5], 9);

        beginTest("a full buffer refuses extra samples and an empty buffer returns nothing");
        ring.reset();
        int block[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        expectEquals(ring.push(block, 8), 8);
        int extra = 9;
        expectEquals(ring.push(&extra, 1), 0);
        int drained[8] = {};
        expectEquals(ring.pop(drained, 8), 8);
        expectEquals(ring.pop(drained, 1), 0);

        beginTest("two threads move a sequence without loss or reordering");
        SpscRingBuffer<int> concurrent(1024);
        constexpr int count = 50000;
        std::atomic<int> mismatches { 0 };
        std::atomic<bool> go { false };
        std::thread producer([&]
        {
            while (!go.load())
                ;
            for (int i = 0; i < count;)
            {
                const int value = i;
                if (concurrent.push(&value, 1) == 1)
                    ++i;
            }
        });
        std::thread consumer([&]
        {
            while (!go.load())
                ;
            for (int expect = 0; expect < count;)
            {
                int value = -1;
                if (concurrent.pop(&value, 1) == 1)
                {
                    if (value != expect)
                        mismatches.fetch_add(1);
                    ++expect;
                }
            }
        });
        go.store(true);
        producer.join();
        consumer.join();
        expectEquals(mismatches.load(), 0);
    }
};

class DriftTest : public juce::UnitTest
{
public:
    DriftTest() : juce::UnitTest("Drift correction", "Wavis") {}

    void runTest() override
    {
        beginTest("matched clocks inside the deadband do not skip or duplicate");
        const auto steady = planDrift(2400, 2400, true, 48000.0, 48000.0);
        expectEquals(steady.skipFrames, 0);
        expectEquals(steady.duplicateFrames, 0);
        expect(near(steady.resampleRatio, 1.0f));

        beginTest("a high buffer skips input and a low buffer duplicates");
        const auto high = planDrift(20000, 2400, true, 48000.0, 48000.0);
        expect(high.skipFrames > 0);
        const auto low = planDrift(100, 2400, true, 48000.0, 48000.0);
        expect(low.duplicateFrames > 0);

        beginTest("different device rates set the resample ratio even when drift correction is off");
        const auto rates = planDrift(2400, 2400, false, 48000.0, 44100.0);
        expect(near(rates.resampleRatio, 48000.0f / 44100.0f, 1.0e-4f));
        expectEquals(rates.skipFrames, 0);
    }
};

class FanoutTest : public juce::UnitTest
{
public:
    FanoutTest() : juce::UnitTest("Simultaneous fan-out", "Wavis") {}

    void runTest() override
    {
        beginTest("one signal is copied to every simulated output with its own gain and mute");
        OutputFanout fanout;
        fanout.setInputSampleRate(48000.0);
        fanout.setDriftEnabled(false);
        fanout.setSmoothingEnabled(false);
        const int a = fanout.activateOutput(48000.0);
        const int b = fanout.activateOutput(48000.0);
        const int c = fanout.activateOutput(48000.0);
        expectEquals(a, 0);
        expectEquals(b, 1);
        expectEquals(c, 2);
        fanout.setGain(a, 1.0f);
        fanout.setGain(b, 0.5f);
        fanout.setGain(c, 1.0f);
        fanout.setMuted(c, true);

        constexpr int frames = 256;
        const auto input = stereoTone(frames);
        expectEquals(fanout.push(input.data(), frames), frames);

        std::vector<float> outA(static_cast<size_t>(frames * 2));
        std::vector<float> outB(static_cast<size_t>(frames * 2));
        std::vector<float> outC(static_cast<size_t>(frames * 2));
        expectEquals(fanout.pull(a, outA.data(), frames), frames);
        expectEquals(fanout.pull(b, outB.data(), frames), frames);
        expectEquals(fanout.pull(c, outC.data(), frames), frames);

        for (int i = 0; i < frames * 2; ++i)
        {
            expect(near(outA[static_cast<size_t>(i)], input[static_cast<size_t>(i)]));
            expect(near(outB[static_cast<size_t>(i)], input[static_cast<size_t>(i)] * 0.5f));
            expect(near(outC[static_cast<size_t>(i)], 0.0f));
        }

        beginTest("with drift correction on, every output still receives the signal");
        OutputFanout drifting;
        drifting.setInputSampleRate(48000.0);
        drifting.setDriftEnabled(true);
        drifting.setSmoothingEnabled(false);
        drifting.setTargetLatencyMs(10.0);
        const int slots[3] = { drifting.activateOutput(48000.0), drifting.activateOutput(48000.0),
                               drifting.activateOutput(48000.0) };
        drifting.setGain(slots[1], 0.25f);
        std::vector<float> loud(4000 * 2, 0.5f);
        expect(drifting.push(loud.data(), 4000) > 0);

        bool allReceived = true;
        for (int slot : slots)
        {
            std::vector<float> block(128 * 2);
            const int got = drifting.pull(slot, block.data(), 128);
            expect(got == 128);
            float peak = 0.0f;
            for (float sample : block)
                peak = std::max(peak, std::abs(sample));
            const float expected = slot == slots[1] ? 0.125f : 0.5f;
            if (!near(peak, expected, 0.02f))
                allReceived = false;
            expect(drifting.getFillFrames(slot) < 4000 - 128);
        }
        expect(allReceived);
    }
};

class DeviceListTest : public juce::UnitTest
{
public:
    DeviceListTest() : juce::UnitTest("Device list diff and reconnect", "Wavis") {}

    void runTest() override
    {
        beginTest("order does not matter and a label change is not a new device");
        auto mic = makeEndpoint("ALSA", "hw:1,0", true, false);
        mic.label = "DJ controller";
        auto speakers = makeEndpoint("ALSA", "hw:0,0", false, true);
        speakers.label = "Speakers";
        auto booth = makeEndpoint("ALSA", "hw:2,0", false, true);
        booth.label = "Booth";

        std::vector<AudioEndpoint> before { mic, speakers };
        std::vector<AudioEndpoint> reordered { speakers, mic };
        const auto same = diffEndpoints(before, reordered);
        expect(!same.any());

        auto renamed = mic;
        renamed.label = "Controller";
        const auto labels = diffEndpoints(before, { renamed, speakers });
        expect(labels.addedIds.empty());
        expect(labels.removedIds.empty());
        expect(labels.labelsChanged);

        const auto added = diffEndpoints(before, { mic, speakers, booth });
        expectEquals(static_cast<int>(added.addedIds.size()), 1);
        expect(added.addedIds[0] == booth.id);
        const auto removed = diffEndpoints({ mic, speakers, booth }, { mic });
        expectEquals(static_cast<int>(removed.removedIds.size()), 2);

        beginTest("selected devices that disappear stay disconnected and reopen when they return");
        OutputRoute live = selectedRoute(speakers.id, true, LinkState::live);
        OutputRoute other = selectedRoute(booth.id, true, LinkState::idle);
        other.selected = false;
        auto merged = mergeOutputRoutes({ live, other }, { mic });
        expectEquals(static_cast<int>(merged.size()), 1);
        expect(merged[0].id == speakers.id);
        expect(!merged[0].available);
        expect(merged[0].selected);

        auto updates = planPresence(merged, true);
        expect(updates[0].shouldClose);
        expect(!updates[0].shouldOpen);
        expect(updates[0].newState == LinkState::disconnected);

        speakers.label = "Speakers";
        merged = mergeOutputRoutes(merged, { speakers });
        expect(merged[0].available);
        merged[0].state = LinkState::disconnected;
        updates = planPresence(merged, true);
        expect(updates[0].shouldOpen);
        expect(!updates[0].shouldClose);

        beginTest("a stopped session does not open devices, and an error is not retried until the device returns");
        OutputRoute ready = selectedRoute("out", true, LinkState::idle);
        auto stopped = planPresence({ ready }, false);
        expect(!stopped[0].shouldOpen);
        expect(stopped[0].newState == LinkState::idle);

        OutputRoute broken = selectedRoute("out", true, LinkState::error);
        auto retry = planPresence({ broken }, true);
        expect(!retry[0].shouldOpen);
        expect(retry[0].newState == LinkState::error);

        broken.available = false;
        auto dropped = planPresence({ broken }, true);
        expect(dropped[0].newState == LinkState::disconnected);
        broken.available = true;
        broken.state = LinkState::disconnected;
        auto back = planPresence({ broken }, true);
        expect(back[0].shouldOpen);

        beginTest("unselected live routes are closed");
        OutputRoute playing = selectedRoute("out", true, LinkState::live);
        playing.selected = false;
        auto closed = planPresence({ playing }, true);
        expect(closed[0].shouldClose);
        expect(closed[0].newState == LinkState::idle);
    }
};

class SourceAndFeedbackTest : public juce::UnitTest
{
public:
    SourceAndFeedbackTest() : juce::UnitTest("System audio classification and feedback", "Wavis") {}

    void runTest() override
    {
        beginTest("monitors, loopback endpoints, and virtual drivers are system/browser audio");
        expect(isSystemAudioName("ALSA", "Monitor of Built-in Audio Analog Stereo"));
        expect(isPulseMonitor("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor", "Built-in Audio"));
        expect(labelPulseSource("alsa_output.pci.monitor", "Built-in Audio Analog Stereo")
                   .find("System/Browser audio — Monitor of ") == 0);
        expect(isSystemAudioName("WASAPI", "Speakers (loopback)"));
        expect(systemAudioLabel("Speakers (loopback)").find("System/Browser audio — ") == 0);
        expect(isSystemAudioName("CoreAudio", "BlackHole 2ch"));
        expect(isSystemAudioName("CoreAudio", "Loopback Audio"));
        expect(isSystemAudioName("CoreAudio", "Soundflower (2ch)"));
        expect(!isSystemAudioName("ALSA", "hw:1,0"));
        expect(!isSystemAudioName("CoreAudio", "DJ Controller"));
        expect(!isPulseMonitor("alsa_input.usb-controller.analog-stereo", "DJ Controller"));

        beginTest("routing captured system audio back to that device is a feedback loop");
        OutputRoute speakers = selectedRoute("ALSA::hw:0", true, LinkState::live);
        speakers.label = "Built-in Audio Analog Stereo";
        speakers.deviceName = "Built-in Audio Analog Stereo";
        OutputRoute booth = selectedRoute("ALSA::hw:2", true, LinkState::live);
        booth.label = "Headphones";
        booth.deviceName = "Headphones";

        auto hits = findFeedbackLoops("System/Browser audio — Monitor of Built-in Audio Analog Stereo", true, { speakers, booth });
        expectEquals(static_cast<int>(hits.size()), 1);
        expect(hits[0].outputId == speakers.id);

        speakers.muted = true;
        hits = findFeedbackLoops("System/Browser audio — Monitor of Built-in Audio Analog Stereo", true, { speakers, booth });
        expect(hits.empty());

        speakers.muted = false;
        hits = findFeedbackLoops("DJ Controller", false, { speakers });
        expect(hits.empty());

        OutputRoute loop = selectedRoute("WASAPI::speakers", true, LinkState::live);
        loop.label = "Speakers (Realtek)";
        loop.deviceName = "Speakers (Realtek)";
        hits = findFeedbackLoops("System/Browser audio — Speakers (Realtek) (loopback)", true, { loop });
        expectEquals(static_cast<int>(hits.size()), 1);

        OutputRoute hole = selectedRoute("CoreAudio::BlackHole 2ch", true, LinkState::live);
        hole.label = "BlackHole 2ch";
        hole.deviceName = "BlackHole 2ch";
        hits = findFeedbackLoops("System/Browser audio — BlackHole 2ch", true, { hole });
        expectEquals(static_cast<int>(hits.size()), 1);

        beginTest("macOS explains the missing native loopback only when none is present");
        expect(platformLoopbackHint("macos", false).find("BlackHole") != std::string::npos);
        expect(platformLoopbackHint("macos", false).find("no native") != std::string::npos);
        expect(platformLoopbackHint("macos", true).find("no native") == std::string::npos);
        expect(platformLoopbackHint("windows", false).find("WASAPI") != std::string::npos);
        expect(platformLoopbackHint("linux", true).find("Monitor of") != std::string::npos);
    }
};

class SettingsTest : public juce::UnitTest
{
public:
    SettingsTest() : juce::UnitTest("Settings persistence", "Wavis") {}

    void runTest() override
    {
        beginTest("settings survive a file round trip and reject junk");
        AppSettings settings;
        settings.driver = "ALSA";
        settings.inputId = "ALSA::hw:1,0";
        settings.visual = "radial";
        settings.filePath = "/tmp/track.wav";
        settings.outputs.push_back({ "ALSA::hw:0,0", 1.4f, false, true });
        settings.outputs.push_back({ "ALSA::hw:0,0", 0.2f, true, true });
        settings.outputs.push_back({ "ALSA::hw:2,0", 0.25f, true, true });

        const auto text = serialiseSettings(settings);
        auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("wavis-settings-unit.txt");
        file.replaceWithText(text);
        const auto loaded = parseSettings(file.loadFileAsString().toStdString());
        file.deleteFile();

        expect(loaded.has_value());
        if (!loaded.has_value())
            return;
        expect(loaded->driver == "ALSA");
        expect(loaded->inputId == "ALSA::hw:1,0");
        expect(loaded->visual == "radial");
        expect(loaded->filePath == "/tmp/track.wav");
        expectEquals(static_cast<int>(loaded->outputs.size()), 2);
        expect(near(loaded->outputs[0].gain, 1.0f));
        expect(loaded->outputs[0].selected);
        expect(loaded->outputs[1].muted);
        expect(!parseSettings("nope").has_value());
        expect(!parseSettings("").has_value());

        auto badMode = parseSettings("WAVIS1\nvisual=nope\n");
        expect(badMode.has_value());
        expect(badMode->visual == "oscilloscope");

        beginTest("saved routes restore gain and a missing device stays selected for reconnect");
        std::vector<OutputRoute> routes;
        auto speakers = makeEndpoint("ALSA", "hw:0,0", false, true);
        routes.push_back({});
        routes.back().id = speakers.id;
        routes.back().available = true;
        applyOutputSettings(routes, loaded->outputs);
        expect(routes[0].selected);
        expect(near(routes[0].gain, 1.0f));
        expectEquals(static_cast<int>(routes.size()), 2);
        expect(!routes[1].available);
        expect(routes[1].state == LinkState::disconnected);
        expect(routes[1].selected);
    }
};

static RingBufferTest ringBufferTest;
static DriftTest driftTest;
static FanoutTest fanoutTest;
static DeviceListTest deviceListTest;
static SourceAndFeedbackTest sourceAndFeedbackTest;
static SettingsTest settingsTest;

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        if (auto* result = runner.getResult(i))
            failures += result->failures;

    std::cout << "Wavis test failures: " << failures << "\n";
    return failures == 0 ? 0 : 1;
}
