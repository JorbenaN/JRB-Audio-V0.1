# Spike S3: Editor rendering

Date: 2026-11-07 · Machine: M4 Pro MacBook Pro, built-in 120 Hz Liquid Retina XDR, macOS 27.0.1, Xcode 27.2
Result: **Core Graphics failed. Metal passed with large margins, offscreen and on screen (0 dropped frames at 120 Hz).** ADR-015's fallback applies: the dense editors render with Metal through a backend-neutral display list.

## Question

Can the piano roll (20,000-note pattern) and the arrangement (200 tracks, 1,307 MIDI clips, 40 audio tracks with waveforms) scroll and zoom at 120 fps on Apple Silicon?

**Target:** at most half a 120 Hz frame (4.17 ms) at the 99th percentile for drawing, leaving the other half for event handling and app logic. For Metal, the CPU side (build + encode) and the GPU side are each held to that target.

## What was built

| Path | What |
|---|---|
| `apps/s3-bench/` | A real macOS app bundle generated entirely by CMake: Info.plist, hardened runtime, entitlements, ad-hoc signature |
| `PianoRollCanvas`, `ArrangementCanvas` | Core Graphics editors (the baseline), plus a "tuned" variant |
| `DisplayList` | Backend-neutral drawing commands: shapes, alpha masks, text, waveforms. **This is the `Canvas` abstraction from ADR-015** |
| `SceneBuilders` | The same two editors emitting display lists instead of drawing |
| `MetalRenderer` | GPU renderer: instanced quads, anti-aliased rounded corners from a signed-distance function, text and pattern previews from R8 atlases, waveforms reduced per pixel column on the GPU from the peak pyramid, per-item clipping |
| `MetalHostView` | `CAMetalLayer` view for on-screen presentation |
| `BenchController` | 7 scripted scroll and zoom scenarios. Modes: on-screen (display link) or offscreen; Core Graphics, tuned or Metal; normal or 2× canvas |

Project data comes from the real C++ core through CoreAPI (S1). Audio waveforms are synthetic peak pyramids, because the model has no audio clips yet.

## Results: p99 frame cost in ms (1440 × 860 pt window at @2x = 2880 × 1720 px)

| Scenario | Visible items | Core Graphics | CG tuned | **Metal CPU** | **Metal GPU** |
|---|---|---|---|---|---|
| Piano roll: scroll, 16 bars | 504 notes | 9.26 | 3.17 | **0.11** | **1.03** |
| Piano roll: scroll, 128 bars | 4,005 notes | 8.33 | 4.80 | **0.19** | **0.46** |
| Piano roll: zoom 2 ↔ 256 bars | 1,639 notes | 11.06 | 8.29 | **0.27** | **0.67** |
| Arrangement: scroll, 32 bars × 17 tracks | 25 clips | 63.11 | 93.66 | **0.10** | **0.37** |
| Arrangement: zoom 8 bars ↔ whole song | 119 clips | 21.72 | 6.01 | **0.18** | **0.69** |
| Arrangement: audio waveforms, 16 bars | 15 clips | 68.07 | 109.45 | **0.08** | **0.40** |
| Arrangement: whole song, 20-pt tracks | 329 clips | 14.33 | 3.69 | **0.19** | **0.67** |
| **Passes (target < 4.17 ms)** | | **0 / 7** | **2 / 7** | **7 / 7** | **7 / 7** |

**2× canvas** (2880 × 1720 pt = 5760 × 3440 px, about a 6K display): **Metal passes 7/7**, with worst cases of 0.30 ms CPU and 2.61 ms GPU. Tuned Core Graphics fails 6/6 of the scenarios it ran (11–26 ms, with spikes up to 1 s).

**On screen** (built-in 120 Hz display, `CAMetalLayer`, display-link driven, 600 frames per scenario): **Metal delivered 100 % of frames with 0 dropped in all 7 scenarios.** Main-thread cost per frame (build + encode + drawable wait) was 0.3–0.9 ms at p50 and at most 1.6 ms at p99.

| Scenario | p50 | p99 | Dropped | Delivered |
|---|---|---|---|---|
| Piano roll: scroll, 16 bars | 0.37 | 0.93 | 0 | 100 % |
| Piano roll: scroll, 128 bars | 0.85 | 1.36 | 0 | 100 % |
| Piano roll: zoom 2 ↔ 256 bars | 0.42 | 1.57 | 0 | 100 % |
| Arrangement: scroll | 0.43 | 1.14 | 0 | 100 % |
| Arrangement: zoom to whole song | 0.49 | 1.13 | 0 | 100 % |
| Arrangement: audio waveforms | 0.31 | 0.68 | 0 | 100 % |
| Arrangement: whole song, 20-pt tracks | 0.86 | 1.57 | 0 | 100 % |

The on-screen Core Graphics comparison was started but stopped. Its window was covered partway through, which pauses the display link. It isn't needed for the decision: the offscreen numbers already show Core Graphics missing the target by 2–25×.

**Visual check:** Metal and Core Graphics frames of the same scenario were compared side by side for the piano roll, the arrangement and the waveforms. The content matches. Metal is slightly crisper and clips the keyboard correctly, where the Core Graphics baseline had a small clipping bug.

## Findings

1. **Core Graphics is limited by fill rate, not object count.** Profiling showed the time in anti-aliased edge rasterization (`aa_render`), alpha blending (`DplusDM`) and image resampling (`argb32_sample_argb32`), all per pixel on the CPU. Tuning (no anti-aliasing, opaque colours, pixel-snapped hairlines) halves the cost but doesn't change the scaling: a canvas twice as wide costs about 4× more. A DAW running full-screen on a large display can't rely on this path.
2. **Metal is 30–600× cheaper on the CPU** and leaves the GPU 75–95 % idle at 120 Hz. A frame is about 1,000–8,000 quads in 4–8 draw calls.
3. **Waveforms must be reduced on the GPU.** The first Metal version emitted one quad per pixel column: 131,000 quads, failing at 6K. Uploading the min/max peak pyramid once and letting the fragment shader reduce the peaks under each column makes a waveform clip **one quad**. The CPU cost dropped from 2 ms to 0.07 ms. That's now the design (ADR-026).
4. **Text goes through a glyph-string atlas.** Each unique label is shaped and rasterized once by Core Text at device resolution, then drawn as a tinted quad. Quality matches Core Text, because it *is* Core Text. The atlas needs eviction before production (it's 2048², shelf-packed, and never evicts yet).
5. **The display list helps portability.** The editors only emit drawing commands, so a future Windows renderer (Direct3D 12) needs only a new renderer for the same list. The scene builders themselves could later move into C++.
6. **Fixed along the way:**
   - **Minimum macOS setting.** `CMAKE_OSX_DEPLOYMENT_TARGET` was set after `project()`, so it was silently ignored, and every binary from S1 and S2 had a minimum of macOS 27.2 instead of 14.0. It's now set before `project()`, and the binary is verified as `minos 14.0`.
   - **Waveform index crash.** Pixel-snapping produced peak index −1 at some zoom levels.
7. **App bundle from CMake (part of ADR-017) is verified:** Info.plist, bundle ID, hardened runtime (`flags=runtime`), and the `disable-library-validation` and `audio-input` entitlements are embedded. The signature is ad-hoc. Developer ID signing and notarization need a paid Apple Developer Program membership: your Mac has *Apple Development* identities but no *Developer ID* one.
8. **A display link only runs while its window is visible.** The first on-screen run stalled because another app was in front, and macOS doesn't let apps launched from a shell take focus. Launching through LaunchServices (`open`) fixed that. Covering the window mid-run pauses it again. That's correct OS behavior, and something the app should use to stop rendering when its editors are hidden.

## Not verified yet

- On-screen pacing for Core Graphics (comparison only; not needed for the decision).
- Text and atlas behaviour under heavy churn (thousands of unique labels), and atlas eviction.
- Precompiled shaders: they compile at launch now (about 50 ms). They'll be a built `.metallib` in the app.
- Accessibility: custom canvases still need their `NSAccessibilityElement` trees. That's unchanged from the plan in 01 §8.
- Intel Macs, and integrated versus discrete GPUs: the bundle is arm64 only so far (Universal 2 build not configured yet).
