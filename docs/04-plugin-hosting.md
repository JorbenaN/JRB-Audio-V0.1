# 04 · Plugin Hosting Strategy

Status: **Phase 1 proposal. No plugin format has been implemented or tested.** Compatibility claims will be made per plugin, only after testing (see §8).

---

## 1. Formats and licensing (checked 7 Nov 2026)

| Format | SDK and license | Priority | Notes |
|---|---|---|---|
| **VST3** | Steinberg VST 3 SDK. **MIT license since VST 3.8 (Oct 2025).** It used to be proprietary or GPLv3. No Steinberg agreement is needed any more. The VST logo and trademark guidelines apply only if we use the logo | P0 | Keep the SDK's copyright and license text in our acknowledgements |
| **Audio Units v2** | Apple AudioToolbox (system framework) | P0 | Most existing Mac plugins |
| **Audio Units v3** | AudioToolbox / AVFoundation `AUAudioUnit` (app extensions) | P0 | Out-of-process by default |
| **CLAP** | MIT, open (free-audio/clap) | **P1. My recommendation, beyond the brief** | Supported by Bitwig, Reaper, FL Studio and others. Clean threading model, per-note expression, cheap to host directly. Strong signal to power users |
| VST2 | Steinberg stopped licensing it in 2018 | ❌ Not supported | No legal path for new hosts |
| AAX | Avid-only | ❌ | Pro Tools only |

**JUCE** hosts VST3 and AU (both v2 and v3) through `AudioPluginFormatManager`, including scanning helpers and editor embedding. We use it for the first implementation, **behind our own `PluginInstance` interface**, so we can replace any format adapter (for example, a direct VST3 host for note expression and threading control) without touching the engine. CLAP hosting is written directly against the C API, because JUCE doesn't host CLAP.

---

## 2. The `PluginInstance` contract (format-agnostic)

```
PluginInstance
  describe()            → id, format, vendor, version, category, I/O layouts, latency, tail
  prepare(sampleRate, maxBlock, layout)      [non-RT]
  process(AudioBuffers&, EventsIn&, EventsOut&, ParamChanges&)   [RT]
  setParameter / beginGesture / endGesture   [thread-tagged]
  getState() → bytes ; setState(bytes)       [non-RT]
  latencyChanged / parametersChanged callbacks → marshalled to main thread
  createEditor() → EditorHandle (NSView embed or separate window)
  setOffline(bool)
```

Three implementations: `InProcessPlugin` (JUCE or direct adapter), `RemotePlugin` (out-of-process proxy, Phase 5/6), and `NativeDevice` (our own instruments and effects use the same contract, so automation, presets and state work identically for everything).

---

## 3. Discovery and scanning: always out-of-process

Loading an unknown plugin binary is the riskiest thing a DAW does. Plugins crash, hang, show license dialogs or call `exit()`.

- A **`jrb-plugin-scanner` helper process** loads each plugin and reports its description as JSON over a pipe. It has a timeout of 15 s by default. A crash or timeout in the scanner **never affects the app**. The plugin is marked "Failed scan" with a reason and a "Retry" button.
- **Incremental:** the cache is keyed by path, modification time and size (plus AU component version), so rescans are fast. New plugins are detected at launch through a background scan, and the user is never blocked.
- **Locations:** `/Library/Audio/Plug-Ins/{VST3,Components,CLAP}`, `~/Library/Audio/Plug-Ins/…`, and AUv3s via `AVAudioUnitComponentManager`. Custom folders can be added in Settings.
- **AU validation:** run Apple's `auval` validation in the scanner for AUv2 (Logic does the same). Failures are shown with Apple's message, and the user can force-enable at their own risk.
- **Architecture check:** an Intel-only plugin can't load into a native arm64 process. For AU, macOS can host such plugins **out-of-process through Apple's AU hosting service**, which is how Logic runs Intel AUs on Apple Silicon. That has to be verified in a spike before we promise it. Intel-only VST3/CLAP plugins require our own x86_64 bridge process (Phase 6, *possibly never*). Until then they're shown as "Requires Intel (not supported)" rather than silently missing.

---

## 4. Plugin Manager UX

- Lives in the Browser under the "Plugins" tab, plus a Settings pane for folders and rescanning.
- Shows: name, vendor, format badges, category, favorite star, and status (OK, failed scan, disabled, missing, Intel-only).
- When the same plugin exists in several formats, it's **shown once**. The preferred format is configurable, with the default order CLAP → VST3 → AU.
- Search runs through ⌘K as well.
- Every error is a sentence a musician understands. For example: "Serum 2 couldn't load: the plugin crashed while starting. Your project still contains its settings."

---

## 5. Process isolation: an honest assessment

| Approach | Crash safety | Cost | Verdict |
|---|---|---|---|
| **In-process** (default) | A crashing plugin kills the app | Zero overhead. Native GUI embedding | ✅ Default, as in Logic, FL and Ableton |
| **Scanner isolation** | Protects scanning only | Small | ✅ Phase 5 |
| **Per-plugin sandbox process** (Bitwig model) | A crash kills only that plugin. The track goes silent and can be reloaded from the last state | Shared-memory audio and Mach semaphores add context switches per block per process: roughly tens of µs, plus memory per process. GUIs must be **separate windows** (macOS has no cross-process NSView embedding for arbitrary plugins) | ✅ **Opt-in per plugin** in Phase 6 ("Run in isolated process"), and automatically offered after a plugin has crashed once |
| AUv3 | The system already isolates it (out-of-process extension) | Apple's IPC overhead. Remote view controller for the UI | Free isolation, provided by the OS |

**Crash recovery when running in-process:** we can't survive a segfault inside a plugin. Instead:

1. **Autosave runs frequently** (see 05), so at most about 1 minute of work is lost.
2. A per-instance **"loading" breadcrumb file** is written before instantiating each plugin and removed after success. On the next launch, the app knows which plugin was being loaded or processed when it crashed, from the breadcrumb and the crash signal handler's last-known-node record.
3. **Safe Mode reopen:** "JRB Audio quit while *Plugin X* was active. Reopen the project with it bypassed?" The project opens, the plugin is bypassed **with its saved state intact**, and the user decides what to do next.

---

## 6. State, presets and project safety

- Plugin state is stored as an **opaque blob plus metadata:** plugin ID, format, vendor, plugin version, a hash of the state, and the parameter values snapshot as a fallback.
- **A missing plugin never deletes data.** The slot becomes a "Missing: Serum 2 (VST3, v2.0.11)" placeholder that keeps the blob and passes audio through. If the plugin is installed later, it loads with full state.
- **The state is read immediately after loading** and compared with the expected data (a sanity check), so a plugin that silently drops state on load is detected and the user is warned *before* saving overwrites good data.
- **Presets:** native presets for every format (VST3 `.vstpreset`, AU `.aupreset`, CLAP preset discovery), plus our own cross-format preset format for the browser (Phase 5).

---

## 7. macOS distribution constraints that shape everything

- **The hardened runtime with `com.apple.security.cs.disable-library-validation`** is required to load third-party plugin binaries signed by other teams. It's allowed for Developer ID distribution with notarization.
- **The Mac App Store is not viable for a host of third-party VST3/AU v2 plugins** (sandbox file access and library validation rules). We distribute directly. *(AUv3-only hosting would work in the sandbox, but excluding most existing plugins would kill the product.)*
- Some plugins also expect `com.apple.security.cs.allow-jit` or unsigned executable memory, because they use JIT-compiled DSP. We'll discover these in compatibility testing and add entitlements only when proven necessary, with a written justification in the decision log.
- Plugin GUIs: VST3 and AUv2 provide an NSView to embed in a host-provided container. AUv3 provides a view controller. Retina scaling, resizing and keyboard focus are notorious sources of bugs, so each one is a separate item in the compatibility test matrix.

---

## 8. Compatibility testing program (from Phase 5)

"Compatible" means **tested**, plugin by plugin:

- **Reference set:** about 40 widely used plugins across formats and vendors (free ones in CI, commercial ones on a manual test machine with licences). For example: FabFilter, Valhalla, Xfer Serum, u-he, Native Instruments Kontakt/Komplete, Arturia, iZotope, Soundtoys, Spitfire, Apple's built-in AUs, plus open-source plugins (Surge XT, Vital, Dexed) for automated CI.
- **Per-plugin checklist:** scan, instantiate, process audio, MIDI in (instruments), automation, state save/reload round-trip, preset load, GUI open/resize/close ×20, sample-rate change, buffer-size change, offline render, latency reporting, and 1-hour soak.
- Results are published in `docs/testing/plugin-compat.md` with plugin versions and dates. **Nothing outside this list is advertised as supported.**
