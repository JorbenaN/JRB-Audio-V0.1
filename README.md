# JRB Audio (working name)

A professional, single-window digital audio workstation for macOS. Apple Silicon first, one-time purchase, offline forever.

**Current state: Phase 1b (technical spikes) complete.** S1 (Swift ↔ C++ core), S2 (real-time engine) and S4 (plugin hosting) passed. S3 chose Metal for the editors. One open issue: an intermittent AudioToolbox crash (see STATUS B-005). There's no app UI, instruments or project files yet. See [STATUS](docs/STATUS.md).

## Documents

| Doc | Contents |
|---|---|
| [01 · Product and experience](docs/01-product-and-experience.md) | Competitor analysis, progressive disclosure, adaptive workspace, unified pattern model, piano roll, design system, accessibility, business model |
| [02 · Architecture](docs/02-architecture.md) | Technology comparison, recommended stack, modules, threads, UI↔engine communication, build and toolchain |
| [03 · Audio engine](docs/03-audio-engine.md) | Time model, render graph compiler, multicore and workgroups, RT rules, MIDI and automation, streaming, recording, offline render and audio testing |
| [04 · Plugin hosting](docs/04-plugin-hosting.md) | VST3, AU and CLAP licensing, scanning, isolation, state safety, distribution constraints, compatibility program |
| [05 · Data model and format](docs/05-data-model-and-project-format.md) | Entities, undo, the package format, versioning, autosave and recovery, assets |
| [06 · MVP, roadmap and risks](docs/06-mvp-roadmap-risks.md) | Feasibility, MVP, phased roadmap, risk register, model usage |
| [Decisions](docs/DECISIONS.md) | Architecture decision log |
| [Status](docs/STATUS.md) | Completed and incomplete work, known bugs, testing record |
| [Spike S1](docs/spikes/S1-swift-cpp-interop.md) | Swift ↔ C++ core: results and measurements |
| [Spike S2](docs/spikes/S2-realtime-skeleton.md) | Real-time engine on Core Audio: results and measurements |
| [Spike S3](docs/spikes/S3-editor-rendering.md) | Editor rendering: Core Graphics vs Metal |
| [Spike S4](docs/spikes/S4-plugin-hosting.md) | Plugin scanning and hosting: AU, VST3, isolation |

## Architecture at a glance

```
Swift/AppKit app ── custom-drawn editors · SwiftUI inspector/settings · ⌘K palette
        │  CoreAPI (narrow, JUCE-free C++ via Swift C++ interop)
C++20 core ── model (immutable snapshots) · edit (commands/undo) · io (package format)
           ── engine (graph compiler → RenderPlan, lock-free swap, workgroup workers)
           ── dsp (native instruments/effects) · midi · plugins (VST3/AU/CLAP adapters)
           ── device (Core Audio via JUCE) · rt (lock-free primitives)
Helpers   ── jrb-plugin-scanner (out-of-process) · jrb-render (headless CLI for tests)
```

## Build and test

Requires Xcode 27+ (selected with `xcode-select`) and CMake 3.29+.

```bash
cmake -S . -B build/xcode -G Xcode
```

```bash
xcodebuild -project build/xcode/JRBAudio.xcodeproj -scheme ALL_BUILD -configuration Release -quiet build && ctest --test-dir build/xcode -C Release
```

Or open `build/xcode/JRBAudio.xcodeproj` in Xcode. C++ core only (no Swift, any generator):

```bash
cmake -S . -B build/cpp -DJRB_BUILD_SWIFT=OFF && cmake --build build/cpp && ./build/cpp/core/jrb_core_tests
```

Real-device soak test (S2). It is muted by default; `--audible` plays a quiet test tone:

```bash
./build/xcode/tools/s2-soak/Release/jrb_s2_soak --seconds 30 --buffer 64 --audible
```

Rendering benchmark (S3). The on-screen run needs its window visible and uncovered for about a minute (launch with `open` so it can come to the front):

```bash
open -W "build/xcode/apps/s3-bench/Release/JRB S3 Bench.app" --args --metal
```

Offscreen (no window), comparing renderers:

```bash
"build/xcode/apps/s3-bench/Release/JRB S3 Bench.app/Contents/MacOS/JRB S3 Bench" --offscreen --metal
```

Plugin hosting test (S4). Build the test VST3s once, then run the host test:

```bash
cmake -S tools/test-plugin -B build/test-plugin -DCMAKE_BUILD_TYPE=Release && cmake --build build/test-plugin -j
```

```bash
./build/cpp/tools/s4-host/jrb_s4_host --scanner build/cpp/tools/plugin-scanner/jrb-plugin-scanner --vst3-folder "$PWD/build/test-plugin/JRBTestGain_artefacts/Release/VST3" --vst3-folder "$PWD/build/test-plugin/JRBTestSynth_artefacts/Release/VST3" --skip "FL Studio"
```
