# Architecture Decision Log

Each decision has a status: **Proposed** (waiting for your approval), **Accepted**, **Superseded**, or **Rejected**.
Details and alternatives are in the referenced document.

| ID | Decision | Status | Date | Reference |
|---|---|---|---|---|
| ADR-001 | Native Swift/AppKit shell, SwiftUI for inspector and settings only, custom-drawn editors | Proposed | 2026-11-07 | 02 §2.1 |
| ADR-002 | Portable C++20 core: model, edit, engine, plugins | Proposed | 2026-11-07 | 02 §2.2 |
| ADR-003 | JUCE used *inside* the core only (devices, formats, plugin adapters). JUCE types never cross CoreAPI | Proposed | 2026-11-07 | 02 §2.2–2.3 |
| ADR-004 | Our own render-graph compiler instead of `juce::AudioProcessorGraph` | Proposed | 2026-11-07 | 03 §4 |
| ADR-005 | Immutable project snapshots with a single writer (main thread). Undo restores snapshots | Proposed | 2026-11-07 | 02 §5, 05 §3 |
| ADR-006 | Three UI↔engine channels: snapshot→plan swap, SPSC parameter queues, atomic/seqlock telemetry | Proposed | 2026-11-07 | 02 §5 |
| ADR-007 | DSP workers join the Core Audio I/O workgroup | Proposed, **validated in S2** (8/8 joined) | 2026-11-07 | 02 §4, 03 §5 |
| ADR-008 | Musical time as int64 ticks at PPQ 6,720 | Proposed | 2026-11-07 | 03 §3 |
| ADR-009 | One track = one sound source = one channel strip. Patterns shared by clips. Step view and piano roll are views of the same pattern | Proposed | 2026-11-07 | 01 §5, 05 §2 |
| ADR-010 | Four modes (Create, Arrange, Mix, Sound) plus recording as a contextual state of Arrange | **Accepted** | 2026-11-07 | 01 §4.2 |
| ADR-011 | Project = macOS document package with JSON model, content-addressed plugin states, atomic saves | Proposed | 2026-11-07 | 05 §5 |
| ADR-012 | Out-of-process plugin scanning. In-process hosting by default. Opt-in per-plugin sandbox later | Proposed, **scanning validated in S4** (crash/hang/garbage contained). In-process risk confirmed by B-005 | 2026-11-07 | 04 §3, §5 |
| ADR-013 | Formats: VST3 + AU (v2/v3) in P0, **CLAP added as P1**. No VST2, no AAX | Proposed | 2026-11-07 | 04 §1 |
| ADR-014 | Distribution via Developer ID + notarization, not the Mac App Store | Proposed | 2026-11-07 | 04 §7, 01 §9 |
| ADR-015 | Dense editors (piano roll, arrangement, automation, mixer meters) render with **Metal** through a backend-neutral **display list**. Core Graphics/AppKit for chrome and simple controls *(revised after S3: Core Graphics failed 0/7, Metal passed 7/7)* | Proposed (revised after S3) | 2026-11-07 | 02 §2.4 |
| ADR-016 | Minimum macOS 14. Universal 2 binary. Apple Silicon is the primary target | Proposed | 2026-11-07 | 02 §8 |
| ADR-017 | Build: **CMake generates the whole Xcode project** (C++ core + Swift app) *(amended after S1; was: CMake core + hand-maintained Xcode project)* | Proposed | 2026-11-07 | 02 §7, spikes/S1 |
| ADR-018 | Real-time rules enforced **at compile time** with `[[clang::nonblocking]]` + `-Werror=function-effects` (works in Apple clang 21, verified in S2). The RealtimeSanitizer runtime job with upstream LLVM becomes a later supplement | Proposed (amended after S2) | 2026-11-07 | 02 §7, 03 §6 |
| ADR-019 | Insert technical spikes (Phase 1b) before the visual prototype | **Accepted** | 2026-11-07 | 06 §3 |
| ADR-020 | Offline license file (Ed25519). The app never locks a user out of opening or playing their projects | Proposed | 2026-11-07 | 01 §9 |
| ADR-021 | Time-stretch: Signalsmith Stretch (MIT) first, with a commercial engine as an upgrade path | Proposed | 2026-11-07 | 03 §8 |
| ADR-022 | Pattern notes stored in shared, time-ordered chunks (~256 notes) so edits to large patterns copy kilobytes, not megabytes. Undo history budgeted at 500 steps / 256 MB | **Accepted, implemented** | 2026-11-07 | spikes/S1 Finding 1 |
| ADR-023 | CoreAPI conventions: `SWIFT_SHARED_REFERENCE` handles with atomic refcounts, `SWIFT_UNCHECKED_SENDABLE` immutable snapshots, no exceptions across the boundary | Proposed | 2026-11-07 | spikes/S1 Finding 2 |
| ADR-024 | Count Core Audio overloads in our own backend (physical device + JUCE's private aggregate) instead of JUCE's `getXRunCount()`, which is broken in 9.0.5 | Proposed | 2026-11-07 | spikes/S2 Finding 2 |
| ADR-025 | Vendor JUCE unmodified in `third_party/JUCE` (modules + build support only). Work around JUCE bugs outside it | Proposed | 2026-11-07 | third_party/JUCE/VENDORED.md |
| ADR-026 | Waveforms: precomputed min/max peak pyramid uploaded to the GPU once, reduced per pixel column in the fragment shader, one quad per clip | Proposed | 2026-11-07 | spikes/S3 Finding 3 |
| ADR-027 | Text in Metal canvases: Core Text rasterizes each unique label once into an atlas, drawn as tinted quads | Proposed | 2026-11-07 | spikes/S3 Finding 4 |
| ADR-028 | JUCE is built into exactly one library per executable: `jrb_juce` (app/tools, with plugin GUIs) or `jrb_juce_headless` (scanner) | Proposed | 2026-11-07 | spikes/S4 |
| ADR-029 | For Audio Units, read parameter flags and live values directly from the AU, not JUCE's cache. Read-only/meter parameters are excluded from automation and state comparison | Proposed | 2026-11-07 | spikes/S4 Findings 2–3 |
| ADR-030 | Test plugins are built in a separate CMake project (`tools/test-plugin`) so JUCE's plugin tooling never changes the main build's flags | Proposed | 2026-11-07 | spikes/S4 |
