# 03 · Audio Engine Design

Status: **Phase 1 proposal. Nothing here is implemented yet.**

The engine is the product. Everything in this document has to be correct before any polished UI matters.

---

## 1. Design goals (measurable)

| Goal | Target, measured on an M-series Mac |
|---|---|
| Round-trip latency | 64-sample buffers at 48 kHz usable for live playing on a 20-track project |
| Stability | 0 xruns in a 4-hour soak test on the reference project at 128 samples |
| Large projects | 200 tracks, 400 plugin instances (native), 2,000 clips, 100k notes: playback at ≤60 % DSP load at 256 samples |
| Edit-to-audible | A note edit is audible on the next loop pass. A structural change applies in under 50 ms |
| Determinism | Offline render of the same project produces **bit-identical** output across runs (native DSP only. Third-party plugins may be nondeterministic) |
| Parity | Offline export equals real-time playback within numerical noise for the same project (verified by test) |

---

## 2. Device layer

- The `AudioDevice` interface lives in the core. Its first backend is **JUCE `AudioIODeviceType` → Core Audio**.
- **Core Audio is the primary backend on macOS. There is no realistic alternative**, and AVAudioEngine is too high-level for a DAW engine.
- Supported: device selection (separate input and output, plus macOS aggregate devices), sample rate, buffer size, channel selection, hot-plug and default-device changes, and sleep/wake.
- **Graceful device loss:** the transport stops, the engine keeps running on a null device, and a non-modal banner says "Audio interface disconnected. Playback paused". Nothing is lost.
- The callback reads the **I/O workgroup** and passes it to the scheduler (see §5).
- Reported latencies (`kAudioDevicePropertyLatency` + safety offset + stream latency) feed recording latency compensation. Users can apply a manual offset, measured with a loopback test helper, in Phase 5.

---

## 3. Time model

- **Musical time** is stored as `int64` ticks at **PPQ = 6,720** (2⁶·3·5·7). Triplets, quintuplets, septuplets and straight notes down to 1/256 are all exact integers, so there are no float drift bugs in editing.
- **Tempo map:** an ordered list of segments, each constant or a linear ramp in BPM. The map is compiled into a lookup table with prefix integrals so tick↔sample conversion takes O(log n) and is exact at segment boundaries.
- **Time-signature map:** a separate list, used only for display, the grid and bar math. It never affects audio timing.
- **Audio clips** have a time base: *musical* (they follow the tempo, needing stretching or resampling) or *absolute* (fixed in seconds, for recordings and dialogue). The default for recordings is absolute. The default for loops is musical.
- Sample positions on the timeline are `int64` sample frames at the session rate. Fractional positions are kept only inside resamplers.

---

## 4. The render graph

### 4.1 Node types

`InstrumentNode`, `AudioClipPlayerNode`, `PluginNode` (native or hosted), `ChannelStripNode` (gain, pan, mute, solo, meter), `SumNode` (buses), `SendNode`, `DelayCompNode`, `HardwareIn/OutNode`, `MeterTapNode`, `RecordTapNode`.

### 4.2 Compilation (off the audio thread)

```
ProjectSnapshot
   │ 1. build DAG: tracks → channel strips → buses/sends/sidechains → master
   │ 2. validate: reject cycles (the UI prevents them; the compiler is the safety net)
   │ 3. topological order + dependency counts for parallel execution
   │ 4. latency pass: compute each node's path latency;
   │    insert DelayCompNodes so all paths into every summing point are aligned
   │ 5. buffer allocation: graph-coloring reuse of audio buffers (memory ∝ width, not node count)
   │ 6. sequence compilation: per-track sorted event arrays (notes→MIDI events in sample time
   │    are NOT precomputed; events stay in ticks and are converted per block, so tempo edits are cheap)
   │ 7. carry over state: reuse existing node instances (plugin objects, voice state, reverb tails)
   │    for unchanged nodes; only new nodes get constructed
   ▼
RenderPlan (immutable, fully preallocated)
```

**Incremental rebuilds:** node instances live in a registry keyed by stable model IDs, so a recompile reuses them. Changing a note or a fader never reconstructs a plugin. Adding a plugin constructs exactly one node.

### 4.3 Swapping plans safely

- The audio thread holds a raw pointer to the current plan. The compiler publishes a new plan with an atomic `exchange` that the audio thread picks up **at the start of a block**.
- Retired plans go into a lock-free **deferred-reclaim queue**. They're destroyed on a background thread only after the audio thread has acknowledged a newer epoch. **The audio thread never runs a destructor that frees memory.**
- Nodes that carry state across plans (reverb tails, voices) are reference-counted *off* the audio thread. The audio thread only borrows them.

---

## 5. Per-block execution and multicore

```
audio callback(block of N frames):
  1. pick up new plan if published (atomic)
  2. drain UI→engine queues: transport commands, parameter changes (timestamped), MIDI input
  3. transport: compute block's tick range (handles loop wrap → split into sub-blocks)
  4. for each instrument/clip source: generate events for [tickStart, tickEnd) → sample offsets
  5. execute DAG:
       ready nodes → work-stealing queue → DSP workers (workgroup members) + the I/O thread itself
       each node: apply sample-accurate param changes (split at change points, or per-sample ramps), process
       on completion: decrement dependents' counters; push newly-ready nodes
  6. master → hardware out; write meter taps (atomics); record taps → lock-free ring → disk writer
  7. publish playhead + load stats
```

- Workers are **pre-spawned real-time threads**, joined to the device's `os_workgroup`. They spin briefly, then wait on a semaphore (`semaphore_signal` from Mach is real-time-safe) when there's no work. We'll measure spin versus sleep on Apple Silicon P-cores and E-cores.
- **Parallelism is per node**, so a chain of plugins on one track is serial by nature. Wide projects scale and deep chains don't. This is how all DAWs behave. Hybrid buffering, which pre-renders tracks that aren't armed or monitored ahead of time with larger buffers (as Logic and Reaper do), is the big Phase 6 performance multiplier. **The architecture accommodates it** because a track's input is either live or not, a fact the compiler knows.
- **Load metering:** time per block divided by block duration, plus a per-node CPU profile available in the Depth layer ("What's using CPU?").

---

## 6. Real-time discipline (non-negotiable rules)

On the audio thread and the DSP workers:

1. **No allocation or deallocation.** All buffers are preallocated in the plan. Variable-size work uses fixed pools sized at compile time.
2. **No locks,** including `std::mutex`, `@synchronized`, `dispatch_sync` and `os_unfair_lock`. Communication uses atomics and wait-free SPSC/MPSC ring buffers only.
3. **No system calls that can block:** no file I/O, no logging to disk or console (logging goes to an RT-safe ring that a background thread drains), no Obj-C messaging, no Swift.
4. **Bounded work.** No algorithm with data-dependent unbounded loops: event lists are pre-sorted and voice counts are capped.
5. **No exceptions** across the RT path. RT code compiles with `-fno-exceptions` semantics by convention and is checked by RTSan.

**Enforcement, not hope:** every RT entry point is annotated `[[clang::nonblocking]]`, and a CI job runs the full test suite and a render soak under **RealtimeSanitizer** (upstream LLVM, because Apple clang doesn't support it; see 02 §7). Third-party plugins break these rules all the time, so RTSan suppresses checks inside plugin calls and measures their time instead.

---

## 7. Instruments, MIDI and automation

- **Event representation inside the engine:** a single internal event type supporting note-on/off with **32-bit velocity and per-note IDs**, per-note expression, CC, pitch bend, and program changes. It converts losslessly to MIDI 1.0, MIDI 2.0 UMP (Core MIDI supports UMP), VST3 events plus note expression, AU MIDI/MusicDevice events, and CLAP events.
- **MIDI input:** Core MIDI timestamps are mapped to the sample clock and delivered with sample offsets in the next block. Live input is therefore early by up to one block rather than jittered, which is standard practice.
- **Automation:** each lane is a breakpoint curve (linear, hold, or eased curves). It's evaluated **sample-accurately** for plugins that accept timestamped parameter changes (VST3 parameter queues, CLAP events, AU scheduled parameters with ramps). Otherwise it's evaluated at sub-block granularity of 32 samples, so automation never sounds stepped. Native DSP uses per-sample smoothing.
- **Recording automation** (touch, latch, write) is captured from the continuous-control queue with gesture boundaries (Phase 5).

---

## 8. Disk, samples and resampling

- **Preloading and streaming:** files ≤ 8 MB (configurable) are fully loaded into memory. Larger files stream from disk with a look-ahead ring of about 2 s per active clip. The streamer prefetches at transport start and for upcoming clips, using the plan's clip index.
- **Decoding:** WAV/AIFF/FLAC/Ogg via JUCE. MP3/AAC/M4A/CAF/ALAC via Core Audio's `ExtAudioFile` (MP3 patents have expired). Files at a foreign sample rate are converted at import or on first use to a cached file at the session rate in the background, so the real-time path never resamples sample-rate mismatches.
- **Resampler:** **r8brain-free-src** (MIT) for offline and cached conversion. A polyphase sinc path for real-time varispeed and sampler pitching.
- **Time-stretch and pitch-shift (Phase 4/5):**

| Library | License | Quality | Plan |
|---|---|---|---|
| **Signalsmith Stretch** | MIT | Good, modern | ✅ First implementation |
| Rubber Band v3 | GPL **or** commercial | Very good | Candidate upgrade if Signalsmith falls short. Requires a paid licence for closed source |
| zplane élastique | Commercial | Industry reference | Consider after revenue |

---

## 9. Recording (Phase 4)

- Input → `RecordTapNode` → lock-free ring → disk writer thread → CAF/WAV file. The file is written incrementally and its header is finalized safely, so a crash leaves a recoverable file.
- **Latency compensation:** recorded audio is placed at `recordStartSample − (inputLatency + outputLatency + safetyOffsets)`, so a take lines up with what the user heard.
- Loop recording creates take lanes, and punch in/out is sample-accurate with crossfades at the boundaries.
- **Monitoring:** software monitoring through the input's channel strip (auto, on or off). Direct monitoring through the interface is the user's choice, and we show a hint about it.
- The system permission prompt for microphone input is shown on first input use, not at launch. The app needs `NSMicrophoneUsageDescription` and the `com.apple.security.device.audio-input` entitlement under the hardened runtime.

---

## 10. Offline render, export and testing

- **The same RenderPlan runs offline**, driven by a non-RT thread pulling blocks as fast as possible. Plugins are told they're offline (VST3 `kOffline` process mode, AU `kAudioUnitProperty_OfflineRender`, JUCE `setNonRealtime`). **Export is not a different code path.** That's how output matches playback.
- Tails are rendered until the output falls below –96 dBFS, or for a fixed number of seconds.
- **Formats:** WAV, AIFF, FLAC (via JUCE / libFLAC, BSD license). ALAC/AAC through AudioToolbox. **MP3 encoding:** LAME is LGPL (dynamic linking keeps us compliant; the patents have expired). This is post-MVP.
- **Delivery options (Phase 5):** sample rate (with high-quality SRC), bit depth 16/24/32f, TPDF dither with optional noise shaping (applied only when reducing bit depth, and last in the chain), loudness or peak normalize (true-peak aware, a two-pass render), export range or loop, stems per track or bus, mono or stereo.
- **The `jrb-render` CLI** renders a project file to WAV headlessly. It's the backbone of automated audio testing:
  - **Golden tests:** fixed projects render to files compared against stored references with tolerances (null test at about –120 dBFS for native DSP).
  - **Parity tests:** offline render versus a simulated real-time render using a fake device clock at randomized block sizes (catches block-boundary bugs, the most common class of engine bug).
  - **Fuzz tests:** random edit sequences during playback, checking invariants (no NaNs or denormals, no xruns in a simulated clock, model round-trip).

---

## 11. Safety nets

- **NaN/Inf guard** on every channel strip output. A plugin that emits NaN is muted and flagged, never propagated to the master.
- **Denormals are flushed to zero** on all RT threads (FPCR FZ bit on ARM, DAZ/FTZ on x86).
- **Master safety limiter** (off by default, prompted after the first clip at >+6 dBFS) protects ears and speakers during wild sound design.
- **Watchdog:** if a plugin's process call exceeds the block budget repeatedly, the user sees "Plugin X is overloading the CPU" with options to bypass or freeze it.
