# 02 · System Architecture and Technology Selection

Status: **Phase 1 proposal. Not yet approved.**

---

## 1. Recommendation in one sentence

**A native Swift/AppKit application shell with custom-drawn editors, on top of a portable C++20 core (project model, edit commands, audio engine, plugin hosting). JUCE is used *inside* the core only for audio devices, file formats and plugin-format hosting. The two halves talk through one narrow, JUCE-free C++ API.**

---

## 2. Technology comparison

### 2.1 UI layer

| Option | Mac feel | Dense editor performance | Accessibility | Windows path | Verdict |
|---|---|---|---|---|---|
| **SwiftUI everywhere** | Excellent for forms | Poor for 10k+ items, gestures and precise hit-testing. Diffing overhead in editors | Very good | None | ❌ For editors. ✅ For inspector, settings, browser lists |
| **AppKit + custom-drawn editors** | Native: menus, responder chain, window management, undo integration, text, drag and drop | Excellent with culling and layer caching. Metal is available where needed | Manual NSAccessibility work for canvases | None (UI rewrite for Windows) | ✅ **Recommended** |
| **JUCE UI (C++)** | Recognizably "not quite Mac": menus, text fields, focus, scrolling physics | Good | Basic | Excellent | ❌ It conflicts with the "first-class Mac app" requirement |
| **Web UI in a native shell** (Tauri/Electron) | Explicitly ruled out by the brief | Fine for chrome, weak for editors | OK | Excellent | ❌ |
| **Fully custom GPU UI** (ImGui, Skia, Flutter) | Must rebuild native behaviors by hand | Excellent | Weak | Excellent | ❌ Too much reinvention and it doesn't feel native |

**The honest cost:** an AppKit UI means a Windows release requires a **new UI shell**. The architecture limits that cost by keeping everything except drawing and event plumbing in portable C++. That includes the project model, undo, every edit operation, snapping and quantize math, the engine, and plugin hosting. A future Windows UI (most likely WinUI 3 or a C++ canvas) reimplements only views. I estimate the views at 30–40 % of the total code. This trade is made deliberately: the brief prioritizes the Mac experience, and FL/Ableton-class users notice a non-native app immediately.

### 2.2 Audio engine and infrastructure

| Option | Verdict |
|---|---|
| **C++20** for the engine | ✅ No real alternative for real-time DSP plus plugin SDKs (VST3, AU and CLAP are all C/C++ APIs). Deterministic memory, no GC, no ARC traffic on the audio thread |
| **Swift** for the engine | ❌ ARC, runtime metadata, existentials and copy-on-write make real-time-safe Swift possible but fragile (Swift 6.x has improved, but there's no compiler-enforced real-time guarantee). It isn't portable to Windows in practice |
| **Rust** for the engine | Viable technically. ❌ Rejected for this team: plugin SDKs, JUCE and Apple's audio APIs are C/C++, and a Rust↔C++↔Swift three-language boundary costs more than it buys |
| **JUCE (modules)** | ✅ **Use selected modules inside the core**: `juce_audio_devices` (Core Audio and later ASIO/WASAPI, aggregate device quirks, MIDI I/O), `juce_audio_formats` (WAV/AIFF/FLAC/Ogg, plus Core Audio decoders), `juce_audio_processors` (VST3 and AU hosting, scanning, editor embedding). Decades of host-quirk fixes we don't want to rediscover |
| **JUCE `AudioProcessorGraph` as the mixer engine** | ❌ We need our own render-graph compiler for: delay compensation across sends and sidechains, multicore scheduling with Audio Workgroups, sample-accurate automation, offline/online parity and incremental rebuilds. This is the heart of the product and must be ours |
| **Direct Core Audio / AudioToolbox** | Used where JUCE is thin: Audio Workgroup joining, AUv3 out-of-process specifics, AudioToolbox AAC/ALAC encoding. Wrapped behind our interfaces |
| **Tracktion Engine** (JUCE-based DAW engine) | Considered. It's GPL or commercially licensed, and adopting it means adopting its model, threading and edit architecture wholesale. ❌ We would be building on someone else's product core. It's a good reference design |

### 2.3 JUCE licensing (verified 7 Nov 2026 on juce.com)

The current version is **JUCE 9** (EULA published 17 June 2026). It's dual-licensed under **AGPLv3** or a commercial JUCE licence:

| Tier | Revenue limit (trailing 12 mo) | Price |
|---|---|---|
| Starter | ≤ $20,000 | Free |
| Indie | ≤ $300,000 | $40/user/month or **$800 perpetual** |
| Pro | Unlimited | $175/user/month (12-month minimum) or **$3,500 perpetual** |

**Implications:** development and early sales can use Starter. A closed-source commercial launch past $20k needs Indie, then Pro. That cost is trivial compared with the engineering time JUCE saves. We **never expose JUCE types across the core API**, so replacing JUCE in any subsystem later stays possible. *Action:* re-read the full EULA before the first public build.

### 2.4 Rendering inside custom editors

| Option | Notes |
|---|---|
| Core Graphics in layer-backed NSViews, culled, with cached tiles | Simplest. Excellent text rendering. Very fast on Apple Silicon if we draw only the visible region and cache waveform tiles |
| Metal 2D batch renderer | Needed only if CG can't hold 120 fps on worst-case content. Text needs a glyph atlas through Core Text |

~~Decision: start with Core Graphics behind a `Canvas` abstraction, and gate the Metal decision on a benchmark.~~ **Measured in spike S3: Core Graphics passed 0 of 7 scenarios (2 of 7 tuned), and Metal passed 7 of 7, also at 6K size.** The dense editors use a display list rendered by Metal (ADR-015, revised). See [spikes/S3](spikes/S3-editor-rendering.md).

---

## 3. Module breakdown

```
┌────────────────────────── macOS App (Swift) ───────────────────────────┐
│ AppShell        windows, toolbar, menus, Settings, document lifecycle │
│ Workspace       modes, layout tree, panel persistence, reset          │
│ Editors         Arrangement · PianoRoll · StepView · Mixer · Devices  │
│                 (AppKit views, Canvas renderers, accessibility trees) │
│ Inspector/Browser   SwiftUI                                          │
│ DesignSystem    tokens, controls (fader, drag-number, knob, meter)    │
│ CommandPalette  ⌘K, unified search                                   │
│ Bridge          Swift-friendly wrappers over CoreAPI, change stream   │
└──────────────────────────────────┬─────────────────────────────────────┘
                     CoreAPI (C++, JUCE-free, Swift C++ interop)
┌──────────────────────────────────┴──────── Portable C++20 core ────────┐
│ model/      immutable project snapshots, IDs, tempo map, validation   │
│ edit/       commands, transactions, undo/redo, selection, snap/quant. │
│ io/         project serialization, migrations, autosave, assets       │
│ engine/     graph compiler, render plan, scheduler, transport,        │
│             automation, PDC, offline render, metering                 │
│ dsp/        native instruments and effects, resampler, stretch, dither│
│ midi/       device I/O, timestamping, UMP/MIDI 2.0, MIDI file I/O     │
│ plugins/    format adapters (VST3, AU, later CLAP), scanner client,   │
│             PluginInstance interface, (later) out-of-process bridge   │
│ device/     AudioDevice interface → JUCE/Core Audio backend           │
│ rt/         lock-free queues, RT allocator pools, deferred reclaim    │
│ platform/   threads, workgroups, file system, crash and log hooks     │
└────────────────────────────────────────────────────────────────────────┘
Separate executables:  jrb-plugin-scanner (helper process) · jrb-render (CLI for tests and CI)
```

**Dependency rule:** `model` and `edit` depend on nothing but the standard library. `engine` depends on `model` and `rt`, but never on `edit` or `io`. Swift depends only on CoreAPI. These rules are enforced by CMake target visibility.

---

## 4. Threads and ownership

| Thread | Priority | Owns or does | Must never |
|---|---|---|---|
| **Main / UI** | User-interactive | Applies edit commands, so it is the single writer of the project model. Draws the UI | Wait on the audio thread |
| **Audio I/O callback** | Real-time (Core Audio) | Executes the current RenderPlan for one block | Allocate, lock, do I/O, call Obj-C or Swift, log |
| **DSP workers** (N = P-cores − 1) | Real-time, **joined to the device's Audio Workgroup** | Parallel node execution within a block | Same as above |
| **Graph compiler** | Normal | Snapshot → RenderPlan. Allocates freely | Touch a plan the audio thread is using |
| **Disk streamer** | High | Reads audio files into lock-free ring buffers ahead of the playhead | Block the audio thread on a miss (a miss outputs silence and raises a flag) |
| **MIDI input** | Core MIDI's thread | Timestamps and pushes into an SPSC queue | — |
| **Background jobs** | Utility | Autosave serialization, waveform peak building, sample analysis, browser indexing | — |
| **Plugin scanner** | Separate *process* | Loads unknown plugins | Run in-process (a crash would kill the app) |

**Apple Silicon detail:** worker threads that aren't in the device's `os_workgroup` (`kAudioDevicePropertyIOThreadOSWorkgroup`) may be scheduled on efficiency cores or deprioritized, causing dropouts under load. Joining the workgroup is mandatory for multicore DSP on macOS 11+.

---

## 5. How the UI and the audio engine communicate

The rule: **the UI never talks to the audio thread directly. It talks to the core, which publishes immutable data to the engine.**

```
 User gesture
     │
     ▼
 Swift Editor ──Command──▶ edit::Session (main thread, single writer)
                                │  apply → new immutable ProjectSnapshot (structural sharing)
                                │  push old snapshot onto the undo stack (cheap: shared pointers)
                 ┌──────────────┼───────────────────────────┐
                 ▼              ▼                           ▼
        UI change stream   GraphCompiler (worker)     Autosave (background)
        (diff summary →    snapshot → RenderPlan       serialize snapshot
         views redraw)            │
                                  ▼  atomic pointer swap at block boundary
                           Audio thread renders the new plan;
                           the old plan goes to the deferred-reclaim queue → freed off-thread
```

Three channels with different latency needs:

1. **Structural changes** (add track, insert plugin, edit notes, move clips) go through the **snapshot → compile → atomic swap** path. Typical latency is 1–20 ms, which is inaudible for edits. Note edits recompile only the affected pattern's event list, not the whole graph.
2. **Continuous control** (fader drag, knob twist, MIDI CC from hardware, macros) goes through **lock-free SPSC parameter queues** straight to the engine, with smoothing applied on the audio side. Sample-accurate latency is one block. The model is updated once **at gesture end** as one undoable command, so dragging a fader doesn't create 400 undo steps or 400 snapshots.
3. **Engine → UI** (meters, playhead, CPU load, xruns, plugin errors):
   - Meters, playhead and load use **atomic or seqlock shared state**, read by the UI on its display link (120 Hz on ProMotion). There are no messages per audio block.
   - Discrete events (xrun, plugin crash, disk underrun, recording finished) go through an **SPSC event queue** drained by the main thread.

**Why immutable snapshots?** They make four hard problems simple: thread-safe engine handoff, trivially correct undo, background autosave without locking the UI, and crash recovery from a consistent state. The cost is careful structural sharing in C++, using `std::shared_ptr<const T>` per entity and copy-on-write for collections. Persistent containers like `immer` are a later optimization if profiling demands them.

---

## 6. Language boundary

- **Swift ↔ C++** uses Swift's native C++ interop (Swift 6.4 is installed on this machine). CoreAPI exposes plain value types, `std::string`/`std::vector`, IDs as 64-bit integers, and opaque handles. It **never** exposes JUCE types, templates heavy enough to confuse the importer, or raw ownership.
- Snapshots are exposed read-only. Swift view-models read the fields they need. Mutation happens only through `session.perform(Command)`.
- **Fallback:** if Swift–C++ interop proves unstable in the Phase 2 spike, CoreAPI also compiles as a C ABI (`extern "C"` handles plus functions). The API design keeps that option open.

---

## 7. Build, repository and toolchain

- The **C++ core uses CMake** (it's portable and JUCE's supported path) and builds static libraries plus the `jrb-render` CLI and the scanner helper.
- ~~The macOS app uses a hand-maintained Xcode project that links the core as an XCFramework.~~ **Amended after spike S1:** CMake generates the whole Xcode project, including the Swift targets (`cmake -G Xcode`). That gives one build definition, and Swift + C++ interop worked first time with zero warnings. Developers open the generated project in Xcode and hit Run. Building the `.app` bundle this way has not been verified yet.
- **Tests:** Catch2 or doctest for C++ (model, edit, engine, DSP and serialization golden files), Swift Testing for Bridge and view logic, and render-based audio tests through `jrb-render` (see 03 §10).
- **CI:** GitHub Actions macOS arm64 runners. One additional job builds and runs tests with **upstream LLVM's `-fsanitize=realtime`** (RealtimeSanitizer, LLVM 20+). I verified in this session that **Apple clang 21 on this Mac rejects `-fsanitize=realtime`**, so this job uses Homebrew LLVM. We also run ASan/UBSan and TSan jobs.

**Toolchain findings on this machine (M4 Pro, macOS 27.0.1):**
- Xcode 27.2 is installed, but `xcode-select` points to the Command Line Tools. Phase 2 needs this to be switched with `sudo xcode-select -s /Applications/Xcode.app`. You'll need to run that yourself because it requires your password.
- CMake isn't installed (`brew install cmake`).

---

## 8. Platform matrix

| Target | Plan |
|---|---|
| Apple Silicon, macOS 14+ | Primary. All performance targets are measured here |
| Intel Mac | Universal 2 binary (arm64 + x86_64). Functionally supported and tested on x86_64 hardware or CI runners where available. **No Intel-specific optimization.** Reconsider at launch: Apple's support for Intel is ending, and supporting Intel costs test time |
| Windows | Not before v1.x. The core is portable by design. JUCE provides ASIO/WASAPI device backends (Steinberg also offers the ASIO SDK under GPLv3 or a proprietary licence). The UI would be new |

*Minimum macOS 14* gives us `NSView` display links, modern Swift concurrency, current AUv3 APIs and Swift–C++ interop support. That lowers risk, and a DAW launching in 2027 or later can reasonably require it.
