# 05 · Data Model, Undo and Project Format

Status: **Phase 1 proposal. Nothing is implemented yet.**

---

## 1. Principles

1. **One source of truth:** the immutable `ProjectSnapshot` in the C++ core. The UI and the engine are both derived from it.
2. **Stable IDs everywhere.** Every entity has a 64-bit ID that's unique within the project. References use IDs, never indices, so reordering, undo and copy-paste can't break links.
3. **Never discard user data we don't understand.** Unknown fields from a newer minor version, missing plugins, and missing samples are all preserved and round-tripped.
4. **Musical time in ticks (PPQ 6,720), audio time in samples**, as described in 03 §3.

---

## 2. Entity model

```
Project
├── meta            formatVersion, appVersion, created/modified, title, author
├── settings        sampleRate (preferred), key/scale, defaultSnap, metronome, groove/swing default
├── tempoMap        [TempoSegment{tick, bpm, curve: hold|linear}]
├── timeSigMap      [TimeSig{bar, numerator, denominator}]
├── markers         [Marker{id, tick, name, color}]
├── sections        [Section{id, startTick, endTick, name, color}]          ← "multiple arrangement sections"
├── tracks          [Track]                                                  (ordered)
├── patterns        {PatternID → Pattern}                                    (shared sources)
├── audioAssets     {AssetID → AudioAsset}
├── mixer           Mixer
├── automation      {LaneID → AutomationLane}
└── ui              per-project UI state (mode, layouts, zoom, selection) — optional, ignorable

Track { id, kind: instrument|audio|folder|bus, name, color, parentFolder?,
        instrument?: DeviceSlot,              // instrument tracks
        input?: InputRoute,                    // audio tracks
        channel: ChannelID,                    // its mixer strip
        clips: [Clip], laneIDs: [LaneID], armed, monitor, frozen?, collapsed, height }

Clip  { id, startTick, lengthTicks, source: PatternID | AssetID,
        sourceOffset, loop?{start, length}, muted, color?,
        // audio-only:
        gainDb, fadeIn{len, curve}, fadeOut{…}, timeBase: musical|absolute, stretch?{mode, ratio}, pitch? }

Pattern { id, name, lengthTicks, view: piano|step, notes: NoteSequence, stepSettings?{resolution, swing} }
                                     // NoteSequence = sorted notes in shared chunks of 64–512 (ADR-022)
Note    { id, pitch(0–127, float detune cents), startTick, lengthTicks,
          velocity(float 0–1), releaseVelocity, probability(0–1), muted,
          expression?: {pitchCurve, pressureCurve, timbreCurve} }      // per-note, ticks-relative

AudioAsset { id, contentHash(SHA-256), originalURL, relativePath, bookmark(security-scoped),
             channels, sampleRate, frames, format, analysis?{tempo, key, transients} }

Mixer { channels: {ChannelID → Channel}, master: ChannelID, order: [ChannelID] }
Channel { id, name, color, kind: track|bus|master, volumeDb, pan, width, mute, solo, phaseInvert,
          inserts: [DeviceSlot], sends: [Send{target, levelDb, pre|post, enabled}],
          output: ChannelID | HardwareOut, sidechainSources: [ChannelID], group? }

DeviceSlot { id, kind: native|vst3|au|clap, pluginRef{uid, name, vendor, version, format},
             stateBlob (stored separately, see §5), stateHash, bypass, wet,
             paramSnapshot{paramID → value}, isolated?:bool }

AutomationLane { id, target{deviceSlot|channel|transport, paramID}, points: [Point{tick, value, curve}],
                 enabled, mode: read|touch|latch|write }
```

### Decisions and their rationale

- **One track equals one channel strip** for instrument and audio tracks. Buses and master are channel-only. This matches the product decision in 01 §5, so beginners never have to route anything.
- **Patterns are shared sources referenced by clips**, which gives you linked clips for free. "Make Unique" means cloning the pattern under a new ID and repointing the clip.
- **Drum kits are a native instrument with pads.** Each pad's pitch is its row in the step view. Per-pad mixer outputs are created as extra channels fed by the instrument's auxiliary outputs.
- **Folders are tracks without sound.** Group processing uses a bus with routing, kept separate from visual folders, because conflating them (as in FL) confuses people.
- **Per-project UI state is optional** and loaded only if present. Corrupted or missing UI state can never prevent a project from opening.

---

## 3. Edits, transactions and undo

- Every mutation is a **Command** (`AddNotes`, `MoveClips`, `SetChannelVolume`, `InsertDevice` and so on). Commands are pure functions `Snapshot → Snapshot` plus a human-readable name ("Move 3 Notes").
- A **Transaction** groups commands into one undo step. Drag gestures open a transaction on mouse-down and commit on mouse-up, so the user sees one undo step per gesture.
- **Undo means restoring the previous snapshot.** It's a pointer swap, and memory use is small because of structural sharing. It's also bug-free in a way inverse commands never are.
- **History limits:** 500 steps, or until memory use reaches 256 MB, whichever comes first. Very large blobs, such as plugin state, are shared between snapshots and never duplicated.
- **Things that aren't model edits:** plugin internal edits made inside a plugin's own GUI. We capture these as an undo step when the plugin signals a gesture end or a state change, through VST3 `performEdit`/`endEdit`, AU parameter events, or CLAP gesture events. Plugins that change state without notifying the host can't be undone. **This is a known industry-wide limitation and we document it rather than hiding it.**
- The macOS Edit menu, ⌘Z/⇧⌘Z, and the "Undo Move 3 Notes" title text are bridged from the core's history.
- **Undo history survives save but not app restart** in the MVP. Persistent history is a candidate for Phase 6.

---

## 4. Selection and the clipboard

- Selection is part of the global UI state, not the project model. It's stored as sets of IDs per entity kind and survives mode switches.
- The clipboard uses a private pasteboard type (serialized model fragment with remapped IDs on paste), plus standard MIDI data when copying notes to other apps.

---

## 5. The project file on disk

### 5.1 The package

A project is a **macOS document package**, a folder that Finder shows as one file:

```
My Song.jrbaudio/
├── Project.json          ← model (UTF-8, canonical key order, zstd-compressed if > 1 MB: Project.json.zst)
├── Plugins/<slotID>.state ← opaque plugin blobs (binary), referenced by hash
├── Audio/                ← recordings + consolidated samples
├── Cache/                ← peaks, converted-rate audio (deletable; excluded from consolidation & backups)
├── Autosave/             ← rolling snapshots (see §6)
└── Info.plist            ← format version, app version, quick-look metadata
```

**Why JSON and not a binary or SQLite format?** JSON is readable, diffable, testable with golden files, and recoverable by hand in a disaster. Size is manageable: a heavy project is about 5–30 MB before compression. Serialization runs off the main thread from an immutable snapshot, so its speed doesn't affect the UI. If profiling later shows a problem with very large MIDI data, note arrays can move into a compact binary sidecar without changing the model.

**Why a package?** Recordings and plugin states sit naturally next to the project, saving can write only changed files, and Finder, iCloud Drive, Dropbox and Time Machine all handle packages correctly.

### 5.2 Writing safely

- Every save is **atomic**: write to a temporary file in the same volume, `fsync`, then `rename` over the original. The previous `Project.json` is kept as `Project.json.bak` until the next successful save.
- Plugin state files are **content-addressed** (named by hash), so unchanged plugins aren't rewritten and no partially-written blob can be referenced.
- After saving, the file is read back and parsed in the background as a verification step. If verification fails, the user gets a loud warning and the backup is kept.

### 5.3 Versioning and compatibility

- `formatVersion` uses major.minor. **Minor bumps are additive:** older app versions ignore unknown fields but **preserve** them on save, via a passthrough map per entity. **Major bumps** go through a migration chain `vN → vN+1` that runs on load. The original file is never overwritten until the user saves, and the first save in the new format keeps a copy named `<name> (v1 backup).jrbaudio`.
- Opening a project from a **newer major version** shows a clear message ("Made with JRB Audio 3. Please update to open it") and never attempts partial loading.
- **Every format version ships with golden test projects** that must load correctly in every later version, forever.

---

## 6. Autosave and crash recovery

- An autosave runs **60 s after the last edit**, and at most once every 60 s during continuous editing. It's an off-thread serialization of the current snapshot into `Autosave/`. The last 10 autosaves are kept.
- On launch after a crash, the app finds autosaves newer than the last real save and offers: **"Recover unsaved changes to My Song from 2 minutes before the crash?"** The options are to restore, open the last saved version, or show both. Nothing is ever discarded automatically.
- Untitled projects autosave to `~/Library/Application Support/JRB Audio/Untitled/`, so a crash before the first save loses nothing either.
- Combined with plugin breadcrumbs (04 §5), recovery can offer to reopen with the suspect plugin bypassed.

---

## 7. Assets, missing files and portability

- **Import policy (configurable):** by default, *reference* files where they live and copy into `Audio/` only for recordings and for "Consolidate". Sound-pack samples that ship with the app are referenced by a stable library ID, not by path.
- **References store:** content hash, original absolute path, path relative to the project, and a **security-scoped bookmark**, so files survive renames and moves on the same volume.
- **Missing sample resolution** happens at load without blocking. The project opens and missing clips show a striped "Missing" state. A single panel lists everything missing and offers automatic search (by hash, then name) in the project folder, user library folders and Spotlight, plus manual relinking. Resolving one file can resolve others in the same folder.
- **Consolidate / "Save as Portable"** copies every external sample into `Audio/`, collects plugin presets, and writes a manifest of the plugins needed. The result is a self-contained package for collaborators or backup.
- **Destructive actions** (deleting unused audio, consolidating with "delete originals") show exactly what will be deleted and move files to the Trash, never permanently deleting them.

---

## 8. Interchange (post-MVP)

| Format | Direction | Phase |
|---|---|---|
| Standard MIDI File (type 0/1) | Import and export, per pattern or per song | 4 |
| WAV/AIFF/FLAC stems | Export | 5 |
| **DAWproject** (open XML format from Bitwig/PreSonus, reported in Cubase 14) | Import and export of tracks, clips, notes, audio, automation and plugin states | 6. A strategic tool for users switching DAWs |
| AAF/OMF | — | Not planned (video post-production audience) |
