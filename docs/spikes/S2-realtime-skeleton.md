# Spike S2: Real-time skeleton

Date: 2026-11-07 · Machine: M4 Pro MacBook Pro, macOS 27.0.1, built-in speakers, JUCE 9.0.5, Apple clang 21
Result: **Passed.** Real Core Audio output through our engine, with lock-free plan swaps at 64 samples, no missed deadlines and workers in the device's audio workgroup.

## What was built

| Path | What |
|---|---|
| `core/include/jrb/rt/` | `JRB_NONBLOCKING` annotation and audited real-time primitives, `SpscQueue` (wait-free ring), `PlanExchange` (publish / acquire / reclaim with no locks and no frees on the audio thread) |
| `core/include/jrb/platform/` | Real-time thread scheduling (Mach time-constraint policy) and `WakeSemaphore` (Mach semaphore) |
| `core/include/jrb/device/` | `AudioDevice` interface. JUCE never leaks past it |
| `core/src/device/JuceAudioDevice.cpp` | Core Audio backend through JUCE, workgroup joining, and our own overload counter (see Finding 2) |
| `core/include/jrb/engine/` | `WorkerPool` (parallel jobs per block), `RenderPlan` (immutable parameters plus shared stateful nodes), `Engine`, and `SpikeCompiler` (a stand-in for the Phase 3 graph compiler) |
| `core/tests/EngineTests.cpp` | 5 hardware-free engine tests |
| `tools/s2-soak/` | Real-device soak test with continuity analysis and an overload-injection mode |
| `third_party/JUCE/` | JUCE 9.0.5 modules, unmodified (see `VENDORED.md`) |

What carries forward to Phase 3: the rt primitives, the worker pool, the device layer, the plan exchange and the engine's callback structure. What gets replaced: `SpikeCompiler` and the fixed two-stage plan shape, which become the general DAG compiler (03 §4).

## Success criteria

| Criterion | Result |
|---|---|
| Core Audio output via JUCE | ✅ Built-in speakers at 48 kHz / 64 frames, exactly as requested |
| Signal through a compiled RenderPlan | ✅ The tone matches an independent reference to < 1e-6 across plan swaps and block sizes from 1 to 700 frames |
| Plan swaps while playing at 64 samples with no glitches | ✅ 2,399 swaps in 2 minutes (every 50 ms, adding and removing nodes each time): **0 missed deadlines, 0 device overloads, 0 discontinuities** |
| Workers joined to the Audio Workgroup | ✅ 8 of 8 workers got real-time scheduling and joined the device workgroup |
| Real-time rule checking | ✅ Stronger than planned: enforced **at compile time** in every build (Finding 1). The runtime RealtimeSanitizer is still not installed |

## Measurements

**2-minute soak** (48 load voices × 16 partials plus a test tone, plan swap every 50 ms, gain change every 200 ms):

| Metric | Value |
|---|---|
| Callbacks | 89,928 (119.9 s of audio) |
| Callback time | 75 µs average, 127 µs maximum, against a 1,333 µs budget |
| Engine overruns / device overloads | 0 / 0 |
| Plan swaps / gain changes | 2,399 / 600 |
| Jobs run by worker threads | 2.86 million |
| Output continuity | 5.76 M samples captured. Largest step 0.0177 against a theoretical maximum of 0.0282. 0 discontinuities |

**Parallel scaling** (300 voices × 16 partials, 64 frames, 15 s each):

| Threads | Average callback | Maximum callback | DSP load |
|---|---|---|---|
| 1 (audio thread only) | 653 µs | 934 µs | 49 % |
| 9 (audio thread + 8 workers) | 173 µs | 243 µs | 13 % |

That's a **3.8× speedup**. It's below 9× because the final summation of 300 voices is still serial and waking workers has a fixed cost per block. The Phase 3 DAG compiler parallelizes bus summing too.

**Hardware-free tests (all pass):**
- An SPSC queue carries 1 M items across threads in order.
- A plan exchange handles 50 k publishes against a spinning consumer with no leaks.
- The worker pool runs 20 k random batches with every job executed exactly once.
- The tone matches the reference.
- Parallel output is **bit-identical** to serial output, including swaps and gain ramps.

All of these pass under **ThreadSanitizer** too, with no data races.

## Findings

1. **Apple clang 21 enforces real-time rules at compile time.** `[[clang::nonblocking]]` combined with `-Werror=function-effects` turns any allocation, lock, throw or call to unverified code inside audio-thread code into a **build error**. The whole render path compiles under it. This is stronger than the runtime RealtimeSanitizer planned in ADR-018: it checks every code path on every build, not only the paths tests happen to run. Some system calls are real-time safe but unannotated: `mach_absolute_time`, `semaphore_signal` and libm `sin`. They're wrapped in an audited list in `rt/Realtime.hpp`, each with a justification. *Out-of-line definitions must repeat the annotation.* RealtimeSanitizer is still useful later for code we don't compile ourselves, such as third-party plugins.
2. **JUCE 9.0.5 bug: `getXRunCount()` always returns −1 on macOS.** `AudioDeviceProcessor::start()` never sets the `running` flag that the overload counter checks. Our backend works around it by listening for Core Audio's `kAudioDeviceProcessorOverload` notification itself. Because JUCE 9 runs every device through a private aggregate device it creates, we listen on that aggregate as well as the physical device. **Verified:** 4 deliberately missed deadlines produced exactly 4 detected overloads. Worth reporting to JUCE.
3. **The device workgroup recommends 10 threads on this M4 Pro.** We use the audio thread plus up to 8 workers, leaving headroom for the UI and the OS.
4. **The built-in speakers report about 850 frames (about 18 ms) of output latency** at a 64-frame buffer. That's the speaker DSP chain, not our engine. Recording latency compensation must use the device-reported figure (03 §9).
5. **Plan swap latency:** 2,399 of 2,401 published plans were picked up. The other 2 were superseded by a newer plan before the next block, which is the intended "newest wins" behavior.

## Not verified yet

- **Audible playback.** The soak muted the speakers and analyzed the captured mix instead. To listen yourself, run `jrb_s2_soak --audible` (see the README). It plays a quiet tone switching between A4 and E5.
- **Other hardware:** external interfaces, Bluetooth, sample-rate changes, unplugging a device while running, and buffer sizes other than 64.
- **Long soak:** the target is 4 hours (03 §1). This run was 2 minutes.
- **RealtimeSanitizer** at runtime. It needs Homebrew LLVM, a large download that needs your OK.
