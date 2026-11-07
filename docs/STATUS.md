# Project Status

Last updated: 2026-11-07 · Current phase: **1b (Technical spikes) complete. S1–S4 done.** Open issue from S4: an intermittent AudioToolbox crash (B-005)

## Completed

| Item | Verified how |
|---|---|
| Phase 1 architecture document set (`docs/01`–`06`, `DECISIONS.md`) | Approved 2026-11-07 (ADR-010, ADR-019 accepted) |
| **Spike S1:** C++ model (tracks, patterns, chunked notes, clips, tempo), validated edit commands, undo/redo with gestures and a memory budget, Swift-facing CoreAPI | 13 C++ tests pass (also under ASan/UBSan). The Swift 6 interop driver passes all checks in Debug and Release. See [spikes/S1](spikes/S1-swift-cpp-interop.md) |
| **Spike S2:** real-time engine skeleton: lock-free plan exchange, SPSC queues, worker pool in the Core Audio workgroup, JUCE device backend, compile-time real-time checks, soak tool | 5 engine tests pass (also under TSan). 2-min real-device soak at 64 frames: 0 overruns, 0 xruns, 0 discontinuities. See [spikes/S2](spikes/S2-realtime-skeleton.md) |
| **Spike S4:** JUCE-free plugin interface; out-of-process scanner; JUCE adapter (with GUIs / headless); live AU parameter and meter handling; hosting test tool; 2 test VST3s | 23 tests pass (5 new scan-isolation tests). Scan 43/43 OK. In-process 42/42 pass audio, state round trip and embedded GUI; FL Studio loads and works. 2 of 12 full-suite runs crashed in AudioToolbox. See [spikes/S4](spikes/S4-plugin-hosting.md) |
| **Spike S3:** rendering benchmark app (CMake-built signed bundle); Core Graphics vs Metal piano roll and arrangement; display-list `Canvas` abstraction; Metal renderer with text and preview atlases and GPU waveforms | Offscreen, 7 scenarios: Core Graphics 0/7, tuned 2/7, **Metal 7/7** (also 7/7 at 6K size). **On screen at 120 Hz: Metal 0 dropped frames.** Visual output compared side by side. See [spikes/S3](spikes/S3-editor-rendering.md) |

## In progress

Nothing active. Phase 1b is complete. Next: decide on the S4 open items, then start Phase 2 (visual prototype on real foundations).

## Not started (everything else)

There is **no app UI, instruments or project files yet.** Code so far: the model/edit core (S1), the real-time engine skeleton (S2), benchmark-grade Metal editor renderers (S3), and plugin scanning and hosting (S4), not yet wired into the engine. See [06-mvp-roadmap-risks.md](06-mvp-roadmap-risks.md) §3 for the plan.

## Known bugs and limitations

| ID | Issue | Severity | Plan |
|---|---|---|---|
| ~~B-001~~ | ~~Undo history for very large patterns used ~400 MB per 500 edits~~ | **Fixed 2026-11-07** | Chunked `NoteSequence` + 256 MB history budget. Now about 6 MB |
| L-001 | The test harness is a minimal in-house one (`core/tests/Check.hpp`) | Low | Replace with doctest or Catch2 (needs your OK to download) |
| L-002 | After a `CMakeLists.txt` change, the first Xcode build can fail to link while CMake regenerates. The second build succeeds | Low | Re-run `cmake` after editing CMake files. Standard CMake/Xcode behavior |
| L-003 | Moving notes scans the whole pattern (134 µs for 20k notes) | Low | Index note IDs by chunk if profiling ever shows it matters |
| B-002 | **JUCE 9.0.5 bug:** `AudioIODevice::getXRunCount()` always returns −1 on macOS (`running` flag never set) | Worked around | Our backend counts Core Audio overloads itself (verified 4 of 4). Consider reporting it to JUCE |
| L-004 | Bus summing is serial, which limits parallel speedup (3.8× with 9 threads at 300 voices) | Medium (Phase 3) | The DAG compiler parallelizes buses |
| L-005 | RealtimeSanitizer (runtime) is not installed. Compile-time checks cover our own code | Low | Install Homebrew LLVM when plugin hosting begins (needs your OK) |
| ~~B-003~~ | ~~`CMAKE_OSX_DEPLOYMENT_TARGET` set after `project()` was ignored: binaries required macOS 27.2, not 14.0~~ | **Fixed 2026-11-07** | Moved before `project()`. Verified `minos 14.0` |
| L-006 | The Metal text atlas never evicts (2048², shelf-packed) | Medium (Phase 2) | LRU eviction or multiple pages |
| L-007 | Metal shaders compile at launch from source (about 50 ms) | Low | Precompiled `.metallib` build step |
| L-008 | Builds are arm64 only. Universal 2 (Intel) not configured | Low | Set `CMAKE_OSX_ARCHITECTURES` before release work |
| B-004 | **JUCE 9.0.5:** AU hosting ignores read-only/meter flags and returns stale cached parameter values | Worked around | Adapter reads flags and live values with `AudioUnitGetProperty`/`AudioUnitGetParameter`. Worth reporting to JUCE |
| B-005 | **Intermittent crash** in AudioToolbox `CAEventReceiver::Impl::timerFired` (use-after-free) when many AU GUIs are opened and closed in one process: 2 of 12 full-suite runs, 0 of 30 isolated runs | **High (open)** | Investigate with Guard Malloc/Instruments. Possibly Apple AU editor views or the CoreAudioKit/CoreAudioAUUI duplicate-class warning. Product mitigation already planned: autosave, Safe Mode, opt-in sandboxing |
| L-009 | VST3 scan reports 0/0 channels (no bus info without instantiating) | Low | Take channel counts from the instance, not the scan cache |
| L-010 | No third-party VST3 or AUv3 plugin tested (none installed) | Medium | Install a free VST3 (e.g. Surge XT), needs your admin password |

## Testing record

| Date | What | Result |
|---|---|---|
| 2026-11-07 | Apple clang 21 `-fsanitize=realtime` | **Unsupported**, so the RTSan job needs upstream LLVM (ADR-018) |
| 2026-11-07 | Toolchain inventory on the development Mac | M4 Pro, macOS 27.0.1, Swift 6.4, Xcode 27.2 installed but `xcode-select` points to the CLT, CMake missing. **You fixed both the same day** (Xcode selected, CMake 4.4.5) |
| 2026-11-07 | S1: C++ core tests (Release, and Debug with ASan+UBSan) | 9/9 pass |
| 2026-11-07 | S1: Swift 6 interop driver (Debug + Release, Xcode generator) | All checks pass. Zero build warnings |
| 2026-11-07 | S1: model benchmark (200 tracks, 120k notes) | Edits 1.6–138 µs. Undo is sub-µs. Memory issue found (B-001) |
| 2026-11-07 | B-001 fix: 13 C++ tests (Release, ASan+UBSan), Swift driver, clean Xcode rebuild, CTest | All pass. 500 big-pattern edits: +6 MB resident (was +398 MB) |
| 2026-11-07 | S2: 18 tests (Release + ThreadSanitizer), Xcode-generator build with JUCE, CTest | All pass. No races. Zero warnings |
| 2026-11-07 | S2: overload detection check (4 injected missed deadlines) | 4 of 4 detected |
| 2026-11-07 | S2: 120 s soak, MacBook Pro speakers, 48 kHz / 64 frames, muted output | PASS: 0 overruns, 0 xruns, 0 discontinuities, 2,399 plan swaps, max callback 127 µs of 1,333 µs |
| 2026-11-07 | S2: scaling, 300 voices x 16 partials | 1 thread 653 µs, 9 threads 173 µs (3.8×). Both pass |
| 2026-11-07 | S3: Core Graphics offscreen, 7 scenarios @2x (default / tuned / 2× canvas tuned) | FAIL 0/7, 2/7, 0/6. p99 3.2–109 ms |
| 2026-11-07 | S3: Metal offscreen, 7 scenarios @2x, normal and 2× canvas | PASS 7/7 and 7/7. Worst p99: CPU 0.30 ms, GPU 2.61 ms |
| 2026-11-07 | S3: visual comparison Metal vs Core Graphics (piano roll, arrangement, waveforms) | Content matches |
| 2026-11-07 | S3: Metal on screen, built-in 120 Hz display, 7 scenarios × 600 frames | PASS: 0 dropped frames, 100 % delivered, p99 main-thread 0.68–1.57 ms |
| 2026-11-07 | S3: app bundle inspection | minos 14.0, hardened runtime, 2 entitlements, ad-hoc signature |
| 2026-11-07 | S4: 23 core tests incl. scan isolation (crash/abort/exit/garbage/truncation/hang/parallel order) | All pass |
| 2026-11-07 | S4: out-of-process scan of all installed plugins | 43/43 candidates OK, 0.4–1.3 s |
| 2026-11-07 | S4: in-process hosting, 42 plugins (40 Apple AU + 2 test VST3) | 42/42 pass audio, state round trip and GUI ×3 (after fixes B-004) |
| 2026-11-07 | S4: full-suite repeat runs | 10/12 complete. 2 crashes in AudioToolbox (B-005). 0/30 crashes in isolated runs |
| 2026-11-07 | S4: FL Studio AU in-process | Loads (51 ms), processes, state byte-identical, GUI attaches. Silent as expected (empty FL project) |

## Facts verified in Phase 1 research

| Fact | Source | Date checked |
|---|---|---|
| JUCE 9 is current. AGPLv3 or JUCE licence. Starter free ≤ $20k, Indie ≤ $300k ($800 perpetual), Pro unlimited ($3,500 perpetual) | juce.com/get-juce, JUCE 9 EULA (17 Jun 2026) | 2026-11-07 |
| VST 3 SDK is MIT-licensed from VST 3.8 (Oct 2025) | Steinberg press release, 29 Oct 2025, plus press coverage | 2026-11-07 |
| DAWproject: open format, Bitwig and Studio One at launch. Cubase 14 support reported | Bitwig, PreSonus, Production Expert | 2026-11-07 |
| RealtimeSanitizer needs LLVM 20+ clang with `[[clang::nonblocking]]` | LLVM docs | 2026-11-07 |

## Still unverified (verify in spikes)

- Hosting Intel-only AUs out-of-process via Apple's hosting service on Apple Silicon (04 §3).
- Building a signed `.app` bundle through the CMake Xcode generator (first Phase 2 task).
- Audible playback check by a person, external audio interfaces, device unplug and sample-rate changes, 4-hour soak (S2).
