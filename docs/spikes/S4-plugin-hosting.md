# Spike S4: Plugin hosting proof

Date: 2026-11-07 · Machine: M4 Pro, macOS 27.0.1, JUCE 9.0.5 (bundled VST3 SDK 3.8.0, MIT)
Result: **Passed, with one serious open issue.** Out-of-process scanning works and is crash-proof. In-process loading, audio, state round trip and embedded GUIs work for every plugin tested. An intermittent crash inside Apple's AudioToolbox needs follow-up (see Finding 5).

## What was built

| Path | What |
|---|---|
| `core/include/jrb/plugins/Plugin.hpp` | JUCE-free hosting interface: `PluginHost`, `PluginInstance`, `PluginEditor`, `PluginInfo`, `ParameterInfo` |
| `core/src/plugins/JucePluginHost.cpp` | The JUCE adapter, compiled twice: with editors (`jrb_juce`) and headless (`jrb_juce_headless`) |
| `core/src/plugins/ScanClient.cpp` | Out-of-process scanning: `posix_spawn`, timeout and kill, crash detection, parallel scans, a plain-text wire format |
| `tools/plugin-scanner/` | `jrb-plugin-scanner`, the helper that loads untrusted plugins. Headless JUCE only |
| `tools/s4-host/` | The hosting test: scan → load → audio on a separate thread → state round trip → GUI inside our own `NSView` ×3 |
| `tools/test-plugin/` | Two test VST3s built from JUCE: an effect with a reported 64-sample latency, and a polyphonic synth. A separate CMake project, because JUCE's plugin tooling changes global compiler flags |
| `core/tests/PluginTests.cpp`, `FakeScanner.cpp` | 5 tests for scan isolation using a stand-in scanner that crashes, aborts, hangs, calls `exit()`, prints garbage or dies mid-write |

The CMake layout changed so that JUCE modules compile into exactly one library per executable: `jrb_juce` for the app and tools, `jrb_juce_headless` for the scanner. Linking both into one binary would duplicate JUCE's symbols.

## What's installed on this Mac

No VST3 or CLAP plugins are installed. There are 40 Apple Audio Units and 1 third-party AU (**FL Studio**, Image-Line). So VST3 hosting was tested with our own JUCE-built plugins, which proves the VST3 host path but not compatibility with third-party VST3s (see "Not verified").

## Results

**Out-of-process scan:** **43 of 43 candidates described, 0 failures, in 0.4–1.3 s** (4 scanner processes in parallel). That includes FL Studio, which reports 32 output channels.

**Isolation tests (no real plugins needed):** crash → `crashed`; `abort()` → `crashed`; `exit(1)` → `failed` with the plugin's stderr; garbage or truncated output → `failed`; a hang → killed at the timeout (0.5 s in the test). Parallel scans keep input order. All pass.

**In-process hosting** (48 kHz, 512-frame blocks, 400 blocks on a separate "audio" thread; effects get a sine, instruments get a C-major chord):

| | Result |
|---|---|
| Plugins tested | 42 (40 Apple AUs + 2 test VST3s), plus FL Studio separately (see below) |
| Load time | 0–13 ms. AUSpeechSynthesis takes about 330 ms |
| Audio | 42/42 produce finite output. Every effect and instrument produces sound. 6 generators and mixers are silent by design, and the test classifies them that way |
| Worst block time | 2.4 ms (AURoundTripAAC). Typically 0.01–0.5 ms per 512 frames |
| Reported latency | Correct for all, e.g. TestGain 64, AUPeakLimiter 96, AUDynamicsProcessor 256, AUNewPitch 4096 samples |
| State round trip | **42/42.** A parameter is changed, saved, and restored into a fresh instance; all writable parameters match. In 41 of 42 the restored state is byte-identical (AURoundTripAAC differs only in bytes, with parameters equal) |
| GUI in our `NSView` | **42/42** open, report a size and attach a native subview, 3 open/close cycles each. Snapshots show the plugins' real interfaces rendering inside our view |
| Full-suite runs | 10 of 12 completed with 42/42 passing. 2 crashed inside Apple's AudioToolbox (Finding 5) |

## Findings

1. **JUCE 9 splits hosting into a headless module** (`juce_audio_processors_headless`). That's exactly what the scanner needs: plugins are loaded without pulling in any GUI framework.
2. **JUCE's Audio Unit hosting doesn't mark read-only meter parameters.** It ignores Core Audio's `IsWritable` and `MeterReadOnly` flags, so gain-reduction meters look like automatable parameters. Our adapter reads the flags from the Audio Unit and exposes `ParameterInfo.readOnly`. A DAW must never offer meters as automation lanes or treat them as user state.
3. **JUCE's cached Audio Unit parameter values go stale.** After `setValueNotifyingHost`, or after restoring state, `getValue()` kept returning old values (for example AUMIDISynth Gain read 0.889 after being set to 0.3). The plugin itself held the correct value, as both its saved state and its own GUI showed. Waiting a full second didn't help. Our adapter now reads live values with `AudioUnitGetParameter`, and with that **all 12 failures disappeared**. Without this fix, automation read-back and the inspector would show wrong values.
4. **AU v2 hosting needs `CoreAudioKit`** (for `AUGenericView`). It's now linked explicitly. macOS also warns that CoreAudioKit and Apple's CoreAudioAUUI bundle both define some Objective-C classes ("may cause spurious casting failures and mysterious crashes"). That's the system's warning, not ours, but see Finding 5.
5. **Intermittent crash inside Apple's AudioToolbox:** `EXC_BAD_ACCESS` in `CAEventReceiver::Impl::timerFired`, on the main run loop, through a garbage pointer: a use-after-free. It happened once during a full run, when the test was opening and closing AU GUIs. JUCE disposes its own AU event listener before destroying each instance, so the dangling listener is probably inside an Apple AU editor view. **Measured: 2 crashes in 12 full-suite runs** (42 plugins, each with 3 GUI open/close cycles, all in one process). Both crashes had the identical stack and the identical bad address (`0x6f432f656c646e75`, which is the bytes of a `.bundle/Contents` path string, so a freed object was reused). Both happened at the same point in the sequence, after AUFilter. **0 crashes in 30 runs of the same plugins in isolation**, so it depends on state left behind by earlier plugins in the same process. Root cause not yet found.
   This is precisely the scenario the architecture plans for. In-process hosting means a plugin (or the system code around it) can crash the app. Hence frequent autosave, breadcrumbs, Safe Mode reopen, and opt-in sandboxing (04 §5).
6. **VST3 scan-time channel counts are 0/0** for our test plugins. JUCE reads VST3 descriptions without instantiating buses. The real counts are known after loading, and the app must take them from the instance, not the scan cache.
7. **Latency reporting works end to end** (TestGain reports 64 samples and really delays 64). That's the input plugin delay compensation (03 §4.2) will need.

## FL Studio

Tested in-process separately (with a 2-minute watchdog): **loaded in 51 ms, processed 400 blocks with finite output, state round trip byte-identical, editor opened inside our view (274 × 82), no crash.** Output was silent, which is expected: the FL Studio plugin is an entire DAW, and its default empty project plays nothing. A meaningful audio test would need an FL project loaded through its own UI. The GUI snapshot was blank because its view is GPU-drawn, which `cacheDisplayInRect` can't capture. The editor attached correctly.

## Not verified yet

- **Third-party VST3 plugins.** None are installed. Installing a free one such as Surge XT or Vital (needs your admin password) would let the suite cover a real third-party VST3.
- **AUv3 (app-extension) plugins:** none are installed besides Apple's system ones. JUCE requires asynchronous or off-main-thread instantiation for these. Not exercised.
- **Intel-only plugins under Rosetta** and out-of-process AU hosting (04 §3): not tested, since no such plugins are present.
- **Plugins inside the real-time engine:** S4 processed audio on a test thread. Wiring `PluginInstance` into the S2 render graph happens in Phase 3/5.
- **Root cause of the AudioToolbox crash** (Finding 5).
