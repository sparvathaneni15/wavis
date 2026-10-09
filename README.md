# Wavis

Wavis takes a waveform from a DJ controller (or any other audio input) and turns it into video, while sending that same audio to as many output devices as you select.

One input is fanned out to every selected output at the same time. Each output has its own gain, mute, status, and latency readout. The picture is drawn from that same input.

## What you can select

The input list always starts with **Test tone** and **Audio file**. After those, Wavis lists the inputs the operating system reports for the driver you picked (or for every driver, when the filter is **All drivers**).

System and browser audio show up in that same list, labeled **System/Browser audio**, so they are easy to find next to a DJ controller:

| Platform | How system audio becomes an input |
| --- | --- |
| Windows | WASAPI loopback capture of a chosen render device. JUCE 8.0.15 does not expose loopback, so Wavis opens the endpoint itself with `AUDCLNT_STREAMFLAGS_LOOPBACK` in shared mode. Each active output device appears as **System/Browser audio — {name} (loopback)** under the **WASAPI Loopback** driver. |
| Linux | PulseAudio and PipeWire monitor sources. Wavis asks libpulse for sources whose name or description is a monitor, and labels them **System/Browser audio — Monitor of …**. PipeWire’s Pulse compatibility layer exposes the same monitor sources, so they show up the same way. They are inputs, not a separate capture API. |
| macOS | There is no native loopback. Install a virtual driver such as [BlackHole](https://github.com/ExistentialAudio/BlackHole) or Rogue Amoeba Loopback (Soundflower is recognised too). It appears as a normal Core Audio input. Wavis prefixes matching names with **System/Browser audio**. If none of those inputs is present, the control panel shows a short hint explaining that macOS needs the virtual driver. |

That source is the only producer. It feeds every selected output and the OpenGL visuals. A hardware input uses one `juce::AudioIODevice` opened for input. A test tone or file is decoded on a worker thread, never inside a device callback. Pulse monitors and WASAPI loopback run on their own capture threads and push into the same fan-out.

### Feedback

If a system/browser input is routed back to the device being captured, the panel warns you and offers **Mute that output**. The check compares normalised names (the `System/Browser audio` prefix, `Monitor of`, and `(loopback)` are stripped). A match is an equal name, or one name contained in the other when the shorter side is at least 6 characters. Ordinary microphones are not flagged. Muted outputs are not flagged. On Linux, an ALSA playback name and a Pulse monitor description only warn when those names still match after normalisation.

## Several outputs at once

`juce::AudioDeviceManager` opens a single device, so it is not the output path. Each selected output gets its own `juce::AudioIODevice`. The producer copies one interleaved stereo block into a lock-free single-producer single-consumer ring for every active output (up to 16). The device callback for that output is the only consumer of its ring. Gain and mute are atomics. Smoothing and drift state live on the consumer. Callbacks do not allocate and do not take locks.

Independent devices do not share a clock. Wavis keeps about 50 ms of audio in each ring (clamped to 10–250 ms when the slot is activated). If the fill wanders outside a deadband of one fifth of that target, the consumer skips up to 64 frames when the ring is high, or repeats up to 8 frames when it is low. If the input and output sample rates differ, the consumer linearly resamples. That is not sample-accurate sync across devices: expect the ring delay plus each device’s own buffer, and the occasional skipped or repeated block instead of a continuous pitch change. The row for each output shows Live, Muted, Underrun, Disconnected, Error, Ready, or Off, plus latency and drift.

ASIO hosts often allow only one client. Selecting several ASIO devices at once can fail for that reason. WASAPI shared, DirectSound, Core Audio, and ALSA can each open their own device.

## Devices coming and going

At startup Wavis asks each `juce::AudioIODeviceType` for its inputs and outputs, and also enumerates Pulse monitors (Linux) or WASAPI loopback endpoints (Windows). It listens for `juce::ChangeListener` messages on `juce::AudioDeviceManager` and for `AudioIODeviceType::Listener` list changes. Those callbacks only request a scan. A 10 Hz timer, off the audio thread, rescans about every 1.5 seconds, or sooner when a notification arrived. The UI is told only when the endpoint list actually changed.

If a selected output disappears, it stays in the routing list with status **Disconnected** and is closed. When the same endpoint id returns, it is opened again while the session is running. A selected input that disappears stops capture, shows disconnected, and is reopened when it comes back. An output that failed to open is not retried until it disappears and returns.

## Visuals

An OpenGL view redraws continuously, with swap interval 1, from a mono ring filled by the same producer as the outputs. Three modes: oscilloscope, spectrum (2048-point FFT, logarithmic bars), and a radial scene. **Full screen** hides the control panel for a projector. Keys `1`, `2`, `3` switch modes, `F` toggles full screen, and Escape leaves the projector view.

## Settings

Driver, input, visual mode, file path, and each output’s gain, mute, and selection are stored with `juce::ApplicationProperties` under the key `state`. The payload is a small `WAVIS1` text document. Gains are clamped to 0…1.

## Build

Dependencies are CMake 3.22+, a C++17 compiler, and [JUCE 8.0.15](https://github.com/juce-framework/JUCE/releases/tag/8.0.15), downloaded by CMake `FetchContent` from the release tarball. You do not vendor JUCE in this repo.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The app is `build/Wavis_artefacts/Release/Wavis`. The tests are `build/WavisTests_artefacts/Release/WavisTests`.

### Linux

Install a compiler, CMake, ALSA, JACK (optional), Pulse, GTK 3, and OpenGL headers. On Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake pkg-config libasound2-dev libjack-jackd2-dev \
  libpulse-dev libgl1-mesa-dev libglu1-mesa-dev libfreetype6-dev libgtk-3-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

`libpulse` turns on monitor-source capture (`WAVIS_HAS_PULSE`). Without it, Linux still lists ALSA/JACK devices, and a monitor only appears if the driver already exposes it as an input whose name looks like a monitor.

### macOS

Xcode command-line tools are enough for Core Audio and OpenGL. BlackHole or Loopback is a separate install if you want system/browser audio. ASIO does not apply.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
open build/Wavis_artefacts/Release/Wavis.app
```

### Windows

Visual Studio 2022 (or Build Tools) with the Windows SDK, plus CMake. WASAPI loopback is compiled in. Link dependencies `ole32`, `uuid`, and `propsys` are added by CMake.

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### ASIO

ASIO is off by default. The ASIO SDK is Steinberg’s, and it cannot be bundled here. To enable it, install the SDK and configure with the folder that contains `common/iasiodrv.h`:

```bash
cmake -S . -B build -DWAVIS_ASIO=ON -DWAVIS_ASIO_SDK=/path/to/asiosdk
```

That defines `JUCE_ASIO=1` for the app target only.

## Run it

Start the app and press **Start**. With no outputs selected, the test tone still drives the visuals (“Video only”). Tick one or more outputs to hear them together; each row’s slider is gain, and Mute is per output.

- **DJ controller.** Plug it in, choose its driver (ASIO, WASAPI, Core Audio, or ALSA), and pick the controller or its interface as the input. Select the outputs that should receive the mix (monitors, booth, a recorder). If the controller is unplugged mid-session, its row or input shows Disconnected and comes back when it reappears.
- **System or browser audio.** On Windows, choose **WASAPI Loopback** (or All drivers) and a **System/Browser audio** entry. On Linux, choose **PulseAudio** and an entry that starts with **System/Browser audio — Monitor of**. On macOS, install BlackHole or Loopback, route the browser to that device, then select the matching **System/Browser audio** input. If the hint says macOS has no native loopback, none of those drivers was detected.
- **No hardware.** Leave the input on **Test tone**, or choose **Audio file** and pick a wav, aiff, flac, ogg, or mp3. A virtual cable (VB-Cable, BlackHole, a Pulse null sink) is the stand-in for a controller: it shows up as an ordinary input, or as system audio when its name matches the rules above.

Headless check, writing one PNG per visual mode and printing `WAVIS_FRAME_VARIANCE`:

```bash
./build/Wavis_artefacts/Release/Wavis --demo-dir /tmp/wavis-demo --demo-seconds 8
```

## Tests

`WavisTests` covers the lock-free ring, drift planning, fan-out of one signal to several simulated outputs (including with drift correction on), device-list diffing, reconnect when a selected device leaves and returns, system-audio classification, the feedback-loop check, and settings parse/apply.

```bash
./build/WavisTests_artefacts/Release/WavisTests
```

## Limits

Simultaneous outputs were verified by feeding one buffer into three simulated consumers and checking each received it, with its own gain and mute. On a machine with no extra sound devices, the running app can still render the test tone. Opening several physical outputs, a real DJ controller, WASAPI loopback, and a macOS virtual driver needs that hardware and was not exercised here.
