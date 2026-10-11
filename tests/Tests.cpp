#include "core/DeviceList.h"
#include "core/DisplayLayout.h"
#include "core/DriftCorrector.h"
#include "core/VisualDynamics.h"
#include "core/VisualFrame.h"
#include "core/OutputFanout.h"
#include "core/RoutingModel.h"
#include "core/SettingsModel.h"
#include "core/SourceClassifier.h"
#include "core/SpscRingBuffer.h"

#include <juce_core/juce_core.h>

#include <algorithm>
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

        beginTest("the same device under several drivers is listed once in the all-drivers view");
        const auto wasapi = makeEndpoint("Windows Audio", "Speakers", false, true);
        const auto dsound = makeEndpoint("DirectSound", "Speakers", false, true);
        const auto asio = makeEndpoint("ASIO", " speakers ", false, true);
        const auto dsBooth = makeEndpoint("DirectSound", "Booth", false, true);
        const auto loopA = makeEndpoint("WASAPI Loopback", "id-a", true, false, true);
        const auto loopB = makeEndpoint("WASAPI Loopback", "id-b", true, false, true);

        auto collapsed = collapseDuplicateDevices<AudioEndpoint>({ wasapi, dsound, asio, dsBooth, loopA, loopB }, {});
        expectEquals(static_cast<int>(collapsed.size()), 4);
        expect(collapsed[0].id == wasapi.id);

        collapsed = collapseDuplicateDevices<AudioEndpoint>({ wasapi, dsound, dsBooth }, { dsound.id });
        expectEquals(static_cast<int>(collapsed.size()), 2);
        expect(collapsed[0].id == dsound.id);

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
        expectEquals(static_cast<int>(loaded->speakers.size()), 0);

        beginTest("speaker spots round trip, clamp onto the floor, and default to an arc");
        AppSettings withSpeakers = settings;
        withSpeakers.speakers.push_back({ "ALSA::hw:0,0", 1.25f, -0.5f });
        withSpeakers.speakers.push_back({ "ALSA::hw:0,0", 9.0f, 0.0f });
        withSpeakers.speakers.push_back({ "ALSA::hw:2,0", 99.0f, -99.0f });
        const auto speakerText = serialiseSettings(withSpeakers);
        const auto speakerLoaded = parseSettings(speakerText);
        expect(speakerLoaded.has_value());
        if (speakerLoaded.has_value())
        {
            expectEquals(static_cast<int>(speakerLoaded->speakers.size()), 2);
            expect(speakerLoaded->speakers[0].id == "ALSA::hw:0,0");
            expect(near(speakerLoaded->speakers[0].x, 1.25f));
            expect(near(speakerLoaded->speakers[0].z, -0.5f));
            expect(near(speakerLoaded->speakers[1].x, kSpeakerFloorHalf));
            expect(near(speakerLoaded->speakers[1].z, -kSpeakerFloorHalf));
        }

        const auto junk = parseSettings("WAVIS1\nspeaker=booth\tnope\t2\nspeaker=\t1\t1\nroute=still-here\t0.5\t0\t1\n");
        expect(junk.has_value());
        if (junk.has_value())
        {
            expectEquals(static_cast<int>(junk->speakers.size()), 1);
            expect(junk->speakers[0].id == "booth");
            expect(near(junk->speakers[0].x, 0.0f));
            expect(near(junk->speakers[0].z, 2.0f));
            expectEquals(static_cast<int>(junk->outputs.size()), 1);
        }

        expect(near(clampSpeakerCoord(9.0f), kSpeakerFloorHalf));
        expect(near(clampSpeakerCoord(-9.0f), -kSpeakerFloorHalf));
        expect(near(clampSpeakerCoord(std::nanf("")), 0.0f));

        float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f, cx = 0.0f, cz = 0.0f;
        defaultSpeakerArc(0, 1, ax, az);
        defaultSpeakerArc(0, 3, bx, bz);
        defaultSpeakerArc(2, 3, cx, cz);
        expect(near(ax, 0.0f));
        expect(std::abs(az) <= kSpeakerFloorHalf);
        expect(bx < -0.5f);
        expect(cx > 0.5f);
        expect(std::abs(bx) <= kSpeakerFloorHalf && std::abs(bz) <= kSpeakerFloorHalf);
        expect(std::abs(cx) <= kSpeakerFloorHalf && std::abs(cz) <= kSpeakerFloorHalf);
        expect(std::abs(bx - cx) > 0.5f);
    }
};

class DisplayAndFrameTest : public juce::UnitTest
{
public:
    DisplayAndFrameTest() : juce::UnitTest("Display hot-plug and visual frames", "Wavis") {}

    void runTest() override
    {
        beginTest("monitor labels name the display and the fullscreen toggle");
        expect(monitorId(0, 0, 1920, 1080) == "0,0,1920,1080");
        expect(monitorLabel(2, 1920, 1080, false) == "Display 2 — 1920×1080");
        expect(monitorLabel(1, 2560, 1440, true).find("(main)") != std::string::npos);
        expect(fullscreenToggleLabel(3) == "Fullscreen on display 3");

        MonitorDesc main { monitorId(0, 0, 1920, 1200), 0, 0, 1920, 1200, true };
        MonitorDesc projector { monitorId(1920, 0, 1280, 720), 1920, 0, 1280, 720, false };
        std::vector<MonitorDesc> both { main, projector };
        expectEquals(defaultMonitorIndex(both), 0);
        expectEquals(indexOfMonitor(both, projector.id), 1);

        beginTest("removing the stage display drops back to the main window");
        expect(stageAfterHotplug(false, projector.id, { main }) == StageDecision::unchanged);
        expect(stageAfterHotplug(true, projector.id, both) == StageDecision::unchanged);
        expect(stageAfterHotplug(true, projector.id, { main }) == StageDecision::exitToMainWindow);
        expect(stageAfterHotplug(true, "", both) == StageDecision::exitToMainWindow);
        expect(stageAfterHotplug(true, monitorId(1920, 0, 1280, 800), both) == StageDecision::exitToMainWindow);

        beginTest("waveform peaks survive decimation and log bins keep frequency order");
        float source[8] = { 0, 0, 0, 0.2f, 0, -0.9f, 0, 0 };
        float wave[4];
        decimatePeaks(source, 8, wave, 4);
        expect(std::abs(wave[2]) > 0.8f);
        expect(std::abs(wave[0]) < 0.01f);

        std::vector<float> magnitudes(128, 0.0f);
        magnitudes[3] = 1.0f;
        float low[kSpectrumBins];
        reduceLogBins(magnitudes.data(), static_cast<int>(magnitudes.size()), low, kSpectrumBins);
        magnitudes[3] = 0.0f;
        magnitudes[100] = 1.0f;
        float high[kSpectrumBins];
        reduceLogBins(magnitudes.data(), static_cast<int>(magnitudes.size()), high, kSpectrumBins);
        const auto peakAt = [](const float* bins)
        {
            int index = 0;
            for (int i = 1; i < kSpectrumBins; ++i)
                if (bins[i] > bins[index])
                    index = i;
            return index;
        };
        expect(peakAt(low) < peakAt(high));

        beginTest("bass, mid, and treble come from separate FFT ranges");
        constexpr int fftSize = 2048;
        constexpr double sampleRate = 48000.0;
        const double binHz = sampleRate / static_cast<double>(fftSize);
        std::vector<float> spectrum(static_cast<size_t>(fftSize / 2), 0.0f);
        spectrum[5] = 0.2f;
        const auto lowBand = measureBands(spectrum.data(), static_cast<int>(spectrum.size()), sampleRate, fftSize);
        expect(lowBand.bass > 0.9f);
        expect(lowBand.treble < 0.05f);
        expect(5.0 * binHz > 20.0 && 5.0 * binHz < 150.0);

        std::fill(spectrum.begin(), spectrum.end(), 0.0f);
        spectrum[40] = 0.2f;
        const auto midBand = measureBands(spectrum.data(), static_cast<int>(spectrum.size()), sampleRate, fftSize);
        expect(midBand.mid > midBand.bass);
        expect(midBand.mid > midBand.treble);

        std::fill(spectrum.begin(), spectrum.end(), 0.0f);
        spectrum[200] = 0.2f;
        const auto highBand = measureBands(spectrum.data(), static_cast<int>(spectrum.size()), sampleRate, fftSize);
        expect(highBand.treble > 0.9f);
        expect(highBand.bass < 0.05f);
        expect(highBand.treble > highBand.mid);
    }
};

class VisualDynamicsTest : public juce::UnitTest
{
public:
    VisualDynamicsTest() : juce::UnitTest("Visual compressor", "Wavis") {}

    void runTest() override
    {
        beginTest("the soft knee is linear below the knee and never reaches the ceiling");
        expectEquals(visualSoftKnee(0.0f, 0.62f, 1.0f), 0.0f);
        expect(std::abs(visualSoftKnee(0.4f, 0.62f, 1.0f) - 0.4f) < 1.0e-5f);
        expect(visualSoftKnee(0.62f, 0.62f, 1.0f) <= 0.62f + 1.0e-5f);
        const float huge = visualSoftKnee(1.0e6f, 0.62f, 1.0f);
        expect(huge > 0.9f);
        expect(huge < 1.0f);
        expect(visualSoftKnee(2.0f, 0.62f, 1.0f) > visualSoftKnee(1.0f, 0.62f, 1.0f));

        beginTest("a sustained loud signal settles near the target and a hit still reads above it");
        VisualDynamics loud;
        float settled = 0.0f;
        for (int i = 0; i < 480; ++i)
            settled = visualCompress(30.0f, loud, 1.0f / 60.0f);
        expect(std::abs(settled - loud.target) < 0.04f);

        const float hit = visualCompress(180.0f, loud, 1.0f / 60.0f);
        expect(hit > settled + 0.2f);
        expect(hit < loud.ceiling);

        beginTest("quiet material stays below a loud passage and the reference falls slowly");
        VisualDynamics quiet;
        float quietOut = 0.0f;
        for (int i = 0; i < 480; ++i)
            quietOut = visualCompress(0.04f, quiet, 1.0f / 60.0f);
        expect(quietOut < 0.25f);
        expect(quietOut < settled);

        const float referenceAfterLoud = loud.reference;
        for (int i = 0; i < 12; ++i)
            visualCompress(0.0f, loud, 1.0f / 60.0f);
        expect(loud.reference > referenceAfterLoud * 0.7f);
        expect(loud.reference < referenceAfterLoud);
    }
};

static VisualDynamicsTest visualDynamicsTest;
static RingBufferTest ringBufferTest;
static DriftTest driftTest;
static FanoutTest fanoutTest;
static DeviceListTest deviceListTest;
static SourceAndFeedbackTest sourceAndFeedbackTest;
static SettingsTest settingsTest;
static DisplayAndFrameTest displayAndFrameTest;

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
