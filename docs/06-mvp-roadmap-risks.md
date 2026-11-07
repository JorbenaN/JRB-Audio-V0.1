# 06 · MVP, Roadmap and Risks

Status: **Phase 1 proposal. Not yet approved.**

---

## 1. Honest feasibility assessment

- **FL Studio, Logic and Ableton each represent 20+ years of work by teams of 20 to over 100 engineers.** Matching their full feature surface isn't a single-developer project, even with AI assistance. "Switch-worthy for a professional" is a **multi-year** goal.
- **What is realistic:** a small, very good core that's genuinely better at specific things: unified pattern/track model, the piano roll, a clean single-window workspace, and reliability. That earns early adopters, and the depth comes over time.
- **The hardest parts are not the visible ones.** They're third-party plugin compatibility (an endless tail of plugin quirks), real-time engine correctness under constant editing, and time-stretching quality. The roadmap front-loads risk in these areas.
- **AI-assisted development changes speed, not physics.** Real-time bugs, plugin quirks and audio quality still have to be verified with ears, hardware and real plugins. Several verification steps require you (listening tests, plugin licences, devices).

---

## 2. MVP definition

**The MVP is the Section 22 workflow, end to end, reliably.** Nothing more is required, and nothing on the list may be faked.

| # | Workflow step | Required capability |
|---|---|---|
| 1 | Launch | Opens in under 2 s to an empty project with the default output device working |
| 2 | New project | ⌘N; default tempo 120 and 4/4 |
| 3 | Add a drum instrument | **"Kit"**, a native drum sampler: 16 pads, one-shot samples, a bundled royalty-free starter kit, plus per-pad gain, pan, tune, decay and choke |
| 4 | Drum pattern | **Step view**: 16/32/64 steps, per-step velocity, swing, pattern length |
| 5 | Melodic instrument | **"Synth"**, a native polyphonic subtractive synth: 2 anti-aliased oscillators, filter, 2 envelopes, LFO, and about 20 factory presets |
| 6 | Melody | **Piano Roll**: draw, select, move, resize, delete, velocity lane, snap, quantize, scale highlight, and audition |
| 7 | Arrangement | Timeline: place pattern clips, move, resize, loop, duplicate, delete, plus mute and solo per track |
| 8 | Tempo | A global tempo. The tempo-map *data model* exists, but tempo changes over time come later |
| 9 | Playback | Transport: play, stop, loop region, metronome. Sample-accurate and glitch-free |
| 10 | Mixer | One strip per track plus master: volume, pan, mute, solo, meters |
| 11 | Effect | Native **EQ, Compressor, Reverb, Delay, Utility and Limiter**, available as inserts |
| 12 | Save | The package format, atomic saves, and autosave |
| 13 | Reopen | The project is restored exactly, verified by a round-trip test |
| 14 | Export WAV | Offline render, 44.1/48 kHz, 16/24-bit plus 32-bit float, TPDF dither at 16-bit |

**Also in the MVP**, because they're cheap and affect trust: undo/redo for every listed edit, crash-recovery autosave, audio device selection and buffer size in Settings, and the ⌘K palette for commands.

**Explicitly not in the MVP:** audio recording, audio clip editing beyond basic playback of dragged-in samples, third-party plugins (only an architecture spike), automation lanes, sends and buses, time-stretching, MIDI controller input (*optional stretch goal: basic note input is cheap*), MP3 export, light mode, and Intel validation.

---

## 3. Roadmap

Effort estimates assume **one full-time developer working with Claude**, and they're indicative. Each phase ends with a demo, the milestone report you asked for, and an updated [STATUS.md](STATUS.md).

### Phase 1: Research and architecture ✅ (this document set)

### Phase 1b: Technical spikes (≈2–3 weeks), **before** the visual prototype

The brief places the visual prototype before the engine. I recommend **de-risking first**, because three spike results change the architecture if they fail:

| Spike | Success criterion | If it fails |
|---|---|---|
| **S1: Swift ↔ C++ core** | Swift reads a snapshot and performs a command through Swift C++ interop. Xcode + CMake build works with one click | Fall back to the C ABI facade |
| **S2: Real-time skeleton** | Core Audio output via JUCE; a sine through a compiled RenderPlan; plan swap while playing at 64 samples with no glitches; workers joined to the Audio Workgroup; RTSan CI job green | Revisit the threading design before building on it |
| **S3: Editor rendering benchmark** | Core Graphics piano roll with 20k notes and arrangement with 200 tracks at 120 fps scroll and zoom on the M4 Pro | Build the Metal canvas path |
| **S4: Plugin hosting proof** | Out-of-process scanner lists installed AU and VST3 plugins. One free VST3 (for example Surge XT) and one Apple AU load in-process, process audio and open their GUI in the main window | Adjust the `PluginInstance` interface while it's still cheap |

### Phase 2: Visual prototype on real foundations (≈4–6 weeks)

The main window, adaptive workspace with all four modes, and Arrangement, Piano Roll, Step view, Mixer, Browser and Plugin browser views, all **bound to the real C++ model** (real editing, real undo). The audio behind them is limited to S2's engine, and **every simulated element carries a visible "Prototype" tag.** Design tokens are implemented in code.

### Phase 3: Core audio engine (≈6–10 weeks)

Device management, the full graph compiler with PDC, transport, the event pipeline, the Kit and Synth instruments, the six native effects, project save/load, autosave and recovery, WAV export, the `jrb-render` CLI and golden and parity tests.

**→ Milestone: MVP workflow passes end to end (Section 22).**

### Phase 4: Core production workflow (≈3–5 months)

Audio tracks and recording (input selection, monitoring, latency compensation, loop and punch recording), audio clip editing (trim, fades, gain), automation lanes (read, draw), sends, buses, sidechain, groups and folders, tempo changes, markers and sections, MIDI controller input, MIDI file import/export, and time-stretch (Signalsmith).

**→ Milestone: a person can produce a complete song with vocals using only built-in tools.**

### Phase 5: Professional functionality (≈6–9 months)

VST3 and AU hosting in production quality, the plugin manager, the compatibility program (04 §8), CLAP, a sampler instrument, advanced piano roll (chords, transforms, per-note expression), automation recording, advanced mixing (stereo tools, metering suite), the professional export suite (stems, normalization, FLAC, MP3, AAC), and the licensing system.

**→ Milestone: private beta with about 50 producers.**

### Phase 6: Polish and optimization (ongoing, ≈4–6 months before 1.0)

Hybrid buffering (anticipative rendering), opt-in plugin sandboxing, accessibility audit, customizable shortcuts, light appearance, memory and startup optimization, DAWproject import/export, Intel validation, and documentation.

**→ 1.0 realistically ≈ 18–30 months from the start of Phase 1b** for one developer. More engineers shorten Phases 4–6, but not Phase 1b–3, which are architecture-bound.

---

## 4. Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **Scope:** the product never ships because the list is endless | High | Fatal | Strict MVP gate. Every feature request is checked against "does this deserve to exist?" Phase milestones are demoable |
| R2 | **Third-party plugin quirks** (threading, GUI, state, latency changes) | Certain | High | JUCE's battle-tested adapters, out-of-process scanning, Safe Mode, the compatibility program, never claiming untested support |
| R3 | **Real-time safety regressions** cause glitches that destroy trust | High | High | Snapshot and RenderPlan architecture, RTSan in CI, parity and fuzz tests at random block sizes, 4-hour soak test before each release |
| R4 | **Swift–C++ interop friction** (compiler crashes, slow builds, importer limits) | Medium | Medium | Spike S1. Narrow API. C ABI fallback |
| R5 | **Custom editor performance and accessibility** costs more than expected | Medium | Medium | Spike S3, `Canvas` abstraction, accessibility designed in from Phase 2 rather than retrofitted |
| R6 | **Time-stretch quality** isn't competitive | Medium | Medium | Abstract stretch engine. Signalsmith first, commercial upgrade path (Rubber Band or élastique) |
| R7 | **Licensing:** JUCE tier past $20k revenue, LGPL LAME, GPL options accidentally linked, sample content rights | Medium | High | A third-party license register (`docs/THIRD_PARTY.md`) maintained from the first dependency. Commercially compatible licenses only. Original or properly licensed sound content |
| R8 | **Apple platform changes** (AU direction, entitlements, Intel end-of-life) | Medium | Medium | Abstraction per format, tracking WWDC each year |
| R9 | **Plugin crashes take down the app** and lose work | High | High | Frequent autosave, breadcrumbs plus Safe Mode, opt-in sandbox in Phase 6 |
| R10 | **Market:** switching cost from FL/Ableton is mostly habit and project lock-in | High | High | Familiar FL gestures where they're good, DAWproject import, MIDI and stems import, a superior piano roll as the headline feature, an honest one-time price |
| R11 | **Testing coverage limits:** one Mac, limited interfaces and plugins | High | Medium | Virtual and loopback device tests, a cheap USB interface for recording tests, beta program for hardware diversity |
| R12 | **Data loss from format bugs** | Low | Fatal | Atomic saves, read-back verification, backups, golden projects for every format version, never overwriting old formats silently |

---

## 5. Claude model usage plan

You asked me to flag model choices explicitly. This session is running on **Claude Opus 5.5**, which I consider appropriate for Phase 1 through Phase 3 engine work. **No switch is needed now.**

| Work | Recommended model | Why |
|---|---|---|
| Architecture, engine (graph compiler, scheduler, lock-free code), plugin hosting, format design, hard debugging, reviews | **Opus 5.5** (current) | Subtle correctness and concurrency reasoning. Mistakes here are expensive to undo |
| Swift UI views, design-token code, straightforward commands, serialization boilerplate, unit tests for clear specs | **Sonnet 5.5** | Well specified. Patterns established by Opus work |
| Docs upkeep, changelogs, test data, renaming and other mechanical refactors | **Haiku 5.5** or Sonnet | Low risk |

When a task's model doesn't match this table, I'll flag it and leave the switch to you.
