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

That source is the only producer. It feeds every selected output and the picture. A hardware input uses one `juce::AudioIODevice` opened for input. A test tone or file is decoded on a worker thread, never inside a device callback. Pulse monitors and WASAPI loopback run on their own capture threads and push into the same fan-out.

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

The picture is a Three.js scene inside `juce::WebBrowserComponent`. The native engine is the only place audio is analysed. About 60 times a second it sends a waveform (128 peaks), 64 logarithmic FFT bins, and three band levels to the page with `emitEventIfBrowserIsVisible` (`wavisFrame` on `window.__JUCE__.backend`). The page does not open a Web Audio context and does not run an FFT.

The look is **Neon Night**: cyan, magenta, and violet light. Bloom is Three.js `UnrealBloomPass`, bundled with the rest of the page (no CDN at runtime). A one-line mode caption sits on a dark chip so it stays readable, and it fades out when the picture is fullscreen.

Three modes, switched from the Visual menu or keys `1`, `2`, `3`:

- **Waveform** — a thin glowing stroke of the native samples, with a short fading trail
- **Spectrum** — slim rounded bars lit from within, with a faint reflection under them
- **3D scene** — a wireframe form, glowing particles with short trails, a colour volume that fills the view, and a room whose speaker cabinets you can drag

The Three.js library, post-processing passes, and shaders are bundled at build time (`web/src/main.js` via esbuild into `web/dist/app.js`) and served through JUCE’s resource provider.

### Bands and envelopes

Bass, mid, and treble are measured in native code from the same 2048-point FFT as the log bins (`measureBands` in `src/core/VisualFrame.h`), using the source sample rate (the test tone is 48 kHz):

| Band | Range | What it drives |
| --- | --- | --- |
| Bass | 20–150 Hz | Curvature (the mass of the invisible wells), wireframe scale, bloom strength |
| Mid | 150–2000 Hz | How many particles are born and how fast the field expands; part of size and brightness |
| Treble | 2000 Hz up to 16 kHz or Nyquist | Fast shimmer and sparkle; part of size and brightness |
| Level | Peak of the source | The accent hue, slowly, from cyan toward magenta. A smaller share also lifts the volume’s brightness. |

Each band is a peak in that Hz range, scaled by 12 like the log bins. The page receives that energy unclamped (`measureBandsOpen`), then compresses it (below) and applies a fast attack and a slow release (`VISUAL.attack` / `VISUAL.release` in `web/src/main.js`) before anything is drawn. The accent uses a slower follow (`VISUAL.accentFollow`) so loudness shifts the colour instead of flashing it. Bar heights use the same attack and release. The waveform’s slow release is the fading trail of earlier strokes.

Every one of those gains, plus bloom, particle counts, well positions, the colour volume, and the room, lives in the `VISUAL` object at the top of `web/src/main.js`.

### Keeping loud music from flashing white

A loud, full mix used to fill the picture with white. The native FFT is unnormalised, so `peak * 12` was then hard-clamped to 1. Broadband music sat on that ceiling for bass, mid, treble, every log bin, and the level at once. The page treated 1 as full drive: bloom strength climbed past 1 with a low threshold, particle brightness climbed past 2 across hundreds of additive points, and the colour volume added several bright samples. Tone mapping was off and the materials were unmapped, so anything above 1 clipped to white. The test tone only lights a few partials, so it stayed coloured while music did not.

The picture now has its own dynamics, separate from the audio outputs. A slow peak follower (attack 10/s, release 0.28/s, a few seconds) tracks recent energy and sets a makeup gain, capped by `maxMakeup` so a quiet passage is not lifted all the way to the target. A soft knee (`tanh`) then bends values above `knee` toward a ceiling of 1 and never crosses it. The fast attack and slow release still run after that, so a bass hit reads louder than the recent average and then eases off, instead of sitting at a constant maximum. Bloom strength, volume energy, and particle brightness are hard-capped, the shader clamps additive colour, and the renderer uses ACES filmic tone mapping. Every few frames the page reads a small copy of the canvas; if its average luminance is above the guard ceiling, exposure is reduced. The knobs are `VISUAL.dynamics` (`target`, `knee`, `ceiling`, `maxMakeup`, `peakAttack`, `peakRelease`, `maxBloom`, `maxEnergy`, `maxBrightness`, `maxParticleProduct`, `addCap`, `luminanceCeiling`).

`--demo-dir` holds a full-scale probe and a music-like varying probe on the 3D scene and logs `WAVIS_LUMA`. The run prints `WAVIS_LUMA_OK` when both stay under 0.62 and the music probe still moves (max − min above 0.015). That pass continues after the three stills, so allow about 40 seconds even if `--demo-seconds` is shorter.

### 3D scene

Particle motion is a CPU loop over typed arrays (a few hundred particles). Each particle is launched with an outward speed and a sideways speed, then pushed further outward — mids set that expansion — and pulled by one or two invisible wells. A pull toward a well bends a path that is not aimed straight at it, so the trails curve. Bass sets the wells’ mass, with the same fast attack and slow release, so a bass hit bends trajectories harder and they ease back. Trails keep the last few positions. Their colour runs from cyan to magenta as speed rises near a well. Treble adds a fast positional shimmer and occasional bright sparkles. The wireframe at the origin scales with bass.

The background is a raymarched colour volume on the inside of a large sphere (a GPU fragment shader, not an empty clear colour). The field mixes cyan, magenta, and violet from layers along x, y, and z, a little 3D noise, and a hue that changes with depth and with time. Before the field is sampled, the shader bends the sample coordinate toward the same wells the particles use, in proportion to bass mass, so the colour itself curves instead of sitting in flat sheets. Overall loudness moves the hue slowly. `VISUAL.gradient` holds the step count, octave count, warp, energy, and palette.

The same 3D scene is also the speaker room. A floor and three walls (open toward the camera) sit in the colour volume, in the same cyan / magenta / violet palette. Each selected output is one cabinet on the floor, up to the existing limit of 16, with the output name on a small label. Drag a cabinet and it stays where you put it: the page calls the native `placeSpeaker` function, and the spot is written into the `WAVIS1` settings as `speaker=<id>`, a tab, then x and z. Positions are clamped to the floor (`kSpeakerFloorHalf`, 3.6). An output you have not placed yet starts on an arc with the other unplaced speakers. Deselecting an output removes its cabinet; selecting it again restores the saved spot. The cabinets are only a picture of the routes — audio still goes out through the devices, not through the page.

The target is 60 fps. After a short warmup, if a frame takes longer than `VISUAL.budgetMs` for several frames in a row, the page turns bloom off, shortens trails, draws fewer particles, and drops the volume to fewer steps and one noise octave. It restores the full look only after many fast frames.

The OpenGL view is only the fallback when the web view cannot be created or WebGL is missing. It draws the same three modes in the same palette, without bloom or the colour volume. The control panel says which path is active (`Three.js · WebKitGTK`, `Three.js · WKWebView`, `Three.js · WebView2`, or `OpenGL fallback`).

The embedded browser is the platform one:

| Platform | Backend |
| --- | --- |
| Windows | WebView2 (`JUCE_USE_WIN_WEBVIEW2`, `Options::Backend::webview2`). CMake downloads Microsoft.Web.WebView2 1.0.3485.44 and copies `WebView2Loader.dll` next to the executable. The machine also needs the Evergreen WebView2 runtime, which current Windows 10/11 installs already include. |
| macOS | WKWebView, the system web view used by `WebBrowserComponent`. |
| Linux | WebKitGTK (`libwebkit2gtk-4.1`, or 4.0 if that is what pkg-config finds). JUCE loads it at runtime. |

### A whole display

The Display menu lists `juce::Desktop::getDisplays()`. **Fullscreen on display N** opens a borderless window (`windowIsTemporary`, no title bar, no buttons) on that monitor’s full logical bounds, including over a panel or taskbar, and moves the picture there. The control panel stays on the main window. `Esc` or `F` leaves it. The page sends Escape to the native `exitFullscreen` function, because the web view owns the keyboard, and on Linux Escape is also grabbed for the whole display while the stage is open. If that monitor disappears, the stage closes and the picture comes back to the main window. Identity is the display’s physical rectangle, so a reindexed list does not look like the same screen.

## Settings

Driver, input, visual mode, file path, each output’s gain, mute, and selection, and each speaker’s floor position are stored with `juce::ApplicationProperties` under the key `state`. The payload is a small `WAVIS1` text document. Gains are clamped to 0…1. Speaker lines are `speaker=<id>`, a tab, x, a tab, and z. Older files without those lines still load; those speakers use the default arc until you drag them.

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
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libwebkit2gtk-4.1-dev libsoup-3.0-dev libjavascriptcoregtk-4.1-dev
```

Node.js and npm rebuild `web/dist` from `web/src/main.js` during the CMake build. Without npm, the committed bundle in `web/dist` is embedded as-is.

`libpulse` turns on monitor-source capture (`WAVIS_HAS_PULSE`). Without it, Linux still lists ALSA/JACK devices, and a monitor only appears if the driver already exposes it as an input whose name looks like a monitor.

### macOS

Xcode command-line tools are enough for Core Audio, OpenGL, and WKWebView. BlackHole or Loopback is a separate install if you want system/browser audio. ASIO does not apply. npm rebuilds the Three.js bundle when it is on `PATH`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
open build/Wavis_artefacts/Release/Wavis.app
```

### Windows

Visual Studio 2022 (or Build Tools) with the Windows SDK, plus CMake. WASAPI loopback is compiled in. Link dependencies `ole32`, `uuid`, and `propsys` are added by CMake. The WebView2 headers and `WebView2Loader.dll` are fetched from NuGet while configuring; the WebView2 runtime has to be present to show the Three.js page (otherwise Wavis uses the OpenGL fallback).

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

Headless check. The app starts the test tone, cycles Waveform, Spectrum, and 3D scene (the scene still includes two labelled cabinets, and one of them is moved before the still), then runs the loud and music-like luminance probes. It writes a PNG of the Three.js canvas (or the OpenGL fallback) plus `WAVIS_FRAME_VARIANCE`, and logs `WAVIS_LUMA` / `WAVIS_LUMA_OK`. Allow about 40 seconds:

```bash
./build/Wavis_artefacts/Release/Wavis --demo-dir /tmp/wavis-demo --demo-seconds 20
```

## Tests

`WavisTests` covers the lock-free ring, drift planning, fan-out of one signal to several simulated outputs (including with drift correction on), device-list diffing, reconnect when a selected device leaves and returns, system-audio classification, the feedback-loop check, settings parse/apply (including speaker spots, floor clamp, and the default arc), the visual soft-knee compressor, monitor hot-plug (the stage returns to the main window when its display id is gone), the native waveform/FFT framing sent to Three.js, and the bass/mid/treble split of that FFT.

```bash
./build/WavisTests_artefacts/Release/WavisTests
```

## Limits

Simultaneous outputs were verified by feeding one buffer into three simulated consumers and checking each received it, with its own gain and mute. On a machine with no extra sound devices, the running app can still render the test tone. Opening several physical outputs, a real DJ controller, WASAPI loopback, and a macOS virtual driver needs that hardware and was not exercised here.
