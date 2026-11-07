# Spike S1: Swift ↔ C++ core

Date: 2026-11-07 · Machine: M4 Pro, macOS 27.0.1, Xcode 27.2, Swift 6.4.2, Apple clang 21, CMake 4.4.5
Result: **Passed.** Swift's native C++ interop is viable for CoreAPI, and the C ABI fallback isn't needed.

## What was built

| Path | What |
|---|---|
| `core/include/jrb/model/` | Immutable model: `Project`, `Track`, `Clip`, `Pattern`, `NoteSequence` (chunked notes), typed IDs, PPQ 6,720 |
| `core/include/jrb/edit/` | `Draft` (copy-on-write working copy), validated commands, `Session` (single writer, undo/redo, gestures) |
| `core/include/jrb/api/CoreAPI.hpp` | The Swift-facing API: `SessionHandle` (reference type), `ProjectView` (Sendable snapshot), plain data structs |
| `core/tests/` | 13 C++ test cases (including a randomized differential test) and a model benchmark |
| `spikes/s1-interop/main.swift` | Swift 6 driver: calls CoreAPI the way the app will, checks results, times the boundary |

This is real foundation code that the app will grow from, not throwaway code. The only spike-only parts are the Swift driver and the minimal test harness.

## Success criteria

| Criterion | Result |
|---|---|
| Swift reads a snapshot through C++ interop | ✅ Tracks, clips, notes and range queries all read from Swift |
| Swift performs commands through C++ interop | ✅ Add, move, delete, clip, tempo, gestures, undo/redo |
| Errors cross the boundary without crashing | ✅ Rejected edits return 0/false, and `lastError()` carries the message. No C++ exception can reach Swift |
| Swift 6 strict concurrency | ✅ Compiles in Swift 6 language mode with zero warnings. A snapshot was passed to a detached task and read off the main thread |
| Xcode + CMake build | ✅ `cmake -G Xcode` generates a project that `xcodebuild` builds with zero warnings, in Debug and Release. CTest runs both suites |
| Memory safety | ✅ C++ tests are clean under AddressSanitizer and UndefinedBehaviorSanitizer |

## Measurements (Release)

**Model (C++), on a project with 200 tracks, 2,000 clips and 120,000 notes:**

| Operation | Time |
|---|---|
| Move 1 note in a 500-note pattern (new snapshot) | 7.4 µs median |
| Move 1 note in a 20,000-note pattern | 138 µs median |
| Change tempo | 1.6 µs |
| Undo / redo | sub-microsecond (pointer swap) |

**Swift → C++ boundary:**

| Operation | Time |
|---|---|
| Take a snapshot | 0.03 µs |
| Add 1 note (including undo bookkeeping) | 0.8 µs |
| Add 20,000 notes in one call | 539 µs |
| Read all 20,000 notes into Swift | 78 µs |
| Read the visible region (16 bars, 512 notes) | 3.6 µs |

At 120 fps a frame lasts 8,333 µs. Reading the visible notes uses less than 0.05 % of that, so the boundary isn't a bottleneck.

**Incremental rebuild** after changing one core file: 1.7 s.

## Findings

1. **✅ Fixed (same day): undo memory for very large patterns.** Copy-on-write originally worked per *pattern*, so every edit to a 20,000-note pattern copied about 800 KB, and 500 such undo steps used **406 MB**. Two fixes are now implemented:
   - **Chunked note storage** (`NoteSequence`, ADR-022): a pattern's notes are stored in sorted chunks of 64–512 notes (target 256), each shared separately. An edit rebuilds only the chunks it touches. Each chunk also records its latest note end, so range queries skip whole chunks.
   - **A memory-budgeted history:** every undo step records the approximate memory it keeps alive. History is trimmed oldest-first beyond 500 steps **or** 256 MB, and the most recent step is always kept.

   **After the fix:** 500 edits to the 20k-note pattern add **about 6 MB of resident memory instead of about 398 MB**, and the history estimate is 9.2 MB. Edit times are unchanged (8.5 µs and 134 µs). Verified by a differential test (7,500 random add/move/delete edits checked against a plain sorted vector, including range queries), a chunk-sharing test, and a test that undoes and redoes through a byte-limited history.
2. **Swift annotations that make the API idiomatic:**
   - `SWIFT_SHARED_REFERENCE(retain, release)` imports `SessionHandle` as a Swift class with automatic reference counting. The reference count must be atomic, because Swift can release from any thread.
   - `SWIFT_UNCHECKED_SENDABLE` on `ProjectView` lets a snapshot cross threads under Swift 6. This is correct only because a snapshot is immutable, so the type must never gain mutating members.
   - `SWIFT_RETURNS_RETAINED` on the factory function.
3. **C++ structs with default member initializers** are imported with a zero-argument initializer. Swift code sets fields one by one. That's fine, and a Swift-side wrapper (`NoteSpec`) hides it.
4. **Build:** with the Xcode generator, C++ interop and the Swift version must be set through Xcode build settings (`SWIFT_OBJC_INTEROP_MODE=objcxx`, `SWIFT_VERSION`), not raw flags. Otherwise Xcode warns that the language mode is overridden. This is now handled in CMake.
5. **Not yet verified:**
   - Building a real `.app` bundle with Info.plist, entitlements, signing and asset catalog through the CMake Xcode generator. This is the first task of Phase 2.
   - The Ninja generator path, because Ninja isn't installed. It isn't needed while Xcode is the developer workflow.

## Decision impact

- **ADR-002, ADR-005:** supported by evidence. ADR-022 (chunked notes) is implemented.
- **ADR-017:** amended proposal. **CMake generates the whole Xcode project, including the Swift app**, instead of a hand-maintained Xcode project plus a CMake library. One build definition, and it worked first time for Swift + C++. To be confirmed when the app bundle is built.
- **C ABI fallback:** not needed.

## Remaining observations

- **Moving one note in a 20k-note pattern takes 134 µs**, mostly hash lookups while validating and scanning all notes for the selected IDs. That's 1.6 % of a 120 fps frame, so it's fine for now. It can drop to the touched chunks only if note IDs are ever indexed by chunk.
- **CMake + Xcode quirk:** after a `CMakeLists.txt` change, the first Xcode build regenerates the project mid-build and can fail to link. The second build succeeds. This is standard CMake behavior with the Xcode generator. Re-running `cmake` before building avoids it.
