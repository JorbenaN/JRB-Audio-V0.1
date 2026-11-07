# 01 · Product and Experience

Status: **Phase 1 proposal. Not yet approved.**
Working name: **JRB Audio**, a placeholder until naming is decided.

---

## 1. The vision in one paragraph

A DAW with FL Studio's creative speed and depth, organized the way a Mac app should be: one window, a stable frame, and content that rearranges itself around what you are doing. Pros get the full feature set. Beginners don't get a reduced edition; they get the same app, where nothing is in the way until they need it.

The product promise we must keep: **a new user makes a loop within five minutes, and a professional never hits a ceiling.** Every design decision below is judged against those two sentences.

---

## 2. Competitive analysis: keep, fix, and avoid

This is based on established, publicly known workflows of each product, not on hands-on testing of current versions in this session.

### FL Studio

| Keep | Fix |
|---|---|
| Channel Rack: drums in seconds, no setup | The rack and playlist are two separate mental models. Patterns are not tied to tracks, so beginners lose track of where a sound plays |
| Piano Roll: fast drawing, ghost notes, the "last note length" behavior, strong tools | Floating windows everywhere. The workspace turns into window management |
| Lifetime free updates. This is a very strong loyalty driver, and we are competing against it | Mixer routing is powerful but opaque. Insert numbering is a source of errors |
| Pattern-based composition: loop-first ideation | Visual density: every control is shown all the time |

### Logic Pro

| Keep | Fix |
|---|---|
| Native Mac feel, single-window main layout, the inspector | Many hidden key-command-only features, so discoverability is poor |
| Strong defaults: a project sounds good immediately | Mac-only by design (we intend to keep a Windows path open) |
| Track stacks, take folders and comping | Its MIDI editing is good but slower than FL for pattern work |
| Smart Controls: a small, curated surface over deep instruments | |

### Ableton Live

| Keep | Fix |
|---|---|
| Device chain at the bottom, with editing in context | The Session/Arrangement split confuses newcomers |
| Very low visual noise; color used meaningfully | Piano roll and automation editing are comparatively limited |
| Warping and time-stretching as a first-class concept | Strict single-window design means mixing big sessions feels cramped |
| Racks and macros: complexity is wrapped, not exposed | |

### What we take from this

1. **One musical data model, several views.** FL's split between Channel Rack and Playlist is its biggest conceptual cost. We unify it (see §5).
2. **A single window and a stable frame**, like Logic and Live, while keeping FL's speed.
3. **Wrap complexity (racks and macros), don't hide it.** Advanced features stay one deliberate action away.
4. **Interoperability.** Switching DAWs is expensive mostly because projects don't move. Supporting **DAWproject** (the open project-exchange format from Bitwig and PreSonus, with reported support in Cubase 14) after the MVP is a strategic way to win users who are switching. FL Studio `.flp` import is not realistic: the format is proprietary and undocumented.

---

## 3. Progressive disclosure: the core design mechanism

There is no "beginner mode" toggle. Every capability lives on one of three layers.

| Layer | What it holds | How you reach it |
|---|---|---|
| **Surface** | The 5–10 controls needed right now: play, tempo, draw a note, volume | Always visible |
| **Context** | Controls for what is selected: clip gain, note velocity, plugin parameters | Appears in the **Inspector** and on hover or selection, then disappears again |
| **Depth** | Routing matrices, latency settings, MIDI filters, per-step parameters, sidechain | Inspector "More" disclosure, context menus, **⌘K command palette**, Settings |

Rules:

- **Defaults do the right thing.** Buffer size, latency compensation, gain staging and routing work without the user touching them. A beginner never needs to know these exist.
- **The ⌘K command palette is the universal entry point.** One search field finds commands, sounds, instruments, presets, plugins, tracks and markers. It is also how every Depth feature remains discoverable without permanent chrome. This doubles as the "unified search" requirement.
- **Every Depth feature has a context-menu or palette path.** If a feature can only be reached through a keyboard shortcut, that is a bug.
- **No modal dialogs for everyday work.** Modal dialogs are used only for destructive confirmations and first-run device permission.
- **Undo covers everything that changes the project,** including mixer moves and plugin parameter gestures.

---

## 4. The adaptive workspace

### 4.1 The frame never moves

```
┌──────────────────────────────────────────────────────────────────────┐
│  Toolbar: ◀ Mode switcher ▶   │ Transport · Tempo · Position │ ⌘K │ CPU│  ← fixed
├────────┬──────────────────────────────────────────────┬──────────────┤
│        │                                              │              │
│Browser │           STAGE (primary editor)             │  Inspector   │
│(toggle │                                              │  (toggle ⌘I) │
│  ⌘B)   │                                              │  shows the   │
│        ├──────────────────────────────────────────────┤  selection   │
│        │           DOCK (secondary editor, toggle)    │              │
└────────┴──────────────────────────────────────────────┴──────────────┘
```

- **Toolbar (fixed):** mode switcher, transport, tempo and time signature, position, ⌘K, CPU and disk meters. These elements never change position in any mode.
- **Browser (left, optional):** sounds, instruments, presets, plugins and project files. It behaves the same in every mode.
- **Stage (center):** the primary editor of the current mode.
- **Dock (bottom, optional):** a secondary editor that pairs with the stage, for example the piano roll under the arrangement.
- **Inspector (right, optional):** properties of the current selection. It is content-driven, not mode-driven.

Modes change **which editors fill the Stage and Dock, and which Inspector sections are expanded first.** They never move the toolbar, never reorder the browser, and never relocate a control within an editor.

### 4.2 Modes

The brief lists five modes. **My recommendation is four, plus a recording state.**

| Mode | ⌘ | Stage | Dock (default) | Inspector focus |
|---|---|---|---|---|
| **Create** | ⌘1 | Pattern editor (Piano Roll or Step view) | Instrument | Notes, scale, chords |
| **Arrange** | ⌘2 | Timeline | Piano roll for the selected clip (hidden until a clip is opened) | Clip, track, automation |
| **Mix** | ⌘3 | Mixer | Device chain of the selected channel | Channel, sends, routing |
| **Sound** | ⌘4 | Device chain / instrument, full size | Modulation, presets | Parameters, macros |

**Why these four modes:**

- **Recording becomes a state of Arrange, not a separate place.** Arming any track (R) reveals what recording needs in context: input selector and monitoring on armed tracks, the count-in, punch and loop-record controls in the transport, input meters, and take lanes. Disarming hides them again. Users record *into* an arrangement. Moving them to another screen to do that is the kind of indirection this product exists to remove. Latency and device settings live in the Inspector's Depth layer and in Settings.
- **Composition and Arrangement stay separate** because the loop-first workflow (Create) and the timeline (Arrange) really are different activities. FL users will recognize this split.

If you still want a dedicated Record mode, it fits into the system above without architectural changes. It is a layout preset, not a code path. **This is a decision I'm asking you to make.**

### 4.3 Predictability rules

1. **Modes change only when the user asks** (switcher, ⌘1–4, or double-clicking something that obviously opens an editor). The app never switches mode on its own.
2. **Selection, playhead, loop range, zoom per editor and scroll position are global state** that survives every mode switch.
3. **Each mode remembers its layout** (panel visibility and split sizes). "Reset Layout" (⌥⌘0) restores the default for the current mode only.
4. **⌘⇧\ toggles back to the previous mode**, which works like "Back".
5. **Custom layouts** are saved as named presets per user and can optionally be stored in a project.
6. Transitions between modes take ≤180 ms, use a crossfade plus a slight slide, and are disabled under Reduce Motion. The editor accepts input at full speed during the transition.

### 4.4 Floating windows

Only two cases: third-party plugin GUIs (which we can't embed reliably in every case; see [04-plugin-hosting.md](04-plugin-hosting.md)) and, optionally, a detached editor for a second display. Native instruments and effects always open inside the Dock or Stage.

---

## 5. The unified musical model, from the user's point of view

FL separates the Channel Rack (who plays) from the Playlist (when). Ableton separates Session from Arrangement. We use **one model, presented through several views:**

- A **Track** owns one sound source (an instrument, or audio input) and one mixer channel. One sound equals one track equals one channel strip, so beginners never have to route anything.
- A **Pattern** is a reusable piece of music on a track. **Clips** in the timeline are *instances* of a pattern. Editing the pattern changes all its instances (shown with a link badge). "Make Unique" forks it.
- **The Step Sequencer and the Piano Roll are two views of the same pattern.** A drum track defaults to Step view (rows equal pads), and a melodic track defaults to Piano Roll. Either view can be switched with one click. You never convert between them, and nothing is lost.
- A **drum kit is one instrument with pads** (like a drum rack), so a beat is one track and one pattern. Each pad can be "exploded" to its own mixer channel in the Depth layer.
- **Create mode shows a "rack" view** listing all instrument tracks with their current pattern's steps, giving FL users the familiar Channel Rack overview. It is a view over tracks, not a second data structure.

This is the single most important product simplification in the plan. It removes FL's biggest confusion while keeping its speed.

---

## 6. The Piano Roll as a flagship

### 6.1 MVP interaction model (Phase 4)

| Action | Gesture |
|---|---|
| Create a note | Click in an empty cell. Drag horizontally while creating to set the length. The new note inherits the last-used length and velocity, as in FL |
| Move | Drag the note body. ⌥-drag duplicates. ⇧ constrains the axis |
| Resize | Drag the right edge (the cursor changes near the edge). ⌥ disables snap |
| Velocity | Drag vertically with ⌘ held on a note, or use the velocity lane underneath. Note color and opacity show velocity at all times |
| Delete | Right-click (FL muscle memory) or ⌫ |
| Select | Marquee on empty space with ⌘ or in Select tool. ⌘A selects all. Click a piano key to select the whole row |
| Audition | Clicking the keyboard or creating or moving a note plays it, using the track's instrument in real time and in parallel with transport playback |
| Transpose | ↑↓ by a semitone, ⇧↑↓ by an octave, scale-aware when a scale is set |
| Quantize | Q, with strength and swing in the Inspector |

A tool switcher exists (Draw, Select, Slice, Mute), but the **default tool is context-sensitive**, so most users never switch tools.

### 6.2 What makes it a reason to switch (Phase 5+)

- **Scale lock and highlight:** out-of-scale rows dim. Optional "fold to scale" hides non-scale rows.
- **Chord stamps** with voicing and inversion cycling (scroll on a stamped chord), plus a chord-progression lane that shows chords above the grid and drives scale-aware transposition.
- **Ghost notes** from other tracks and patterns, as in FL.
- **Per-note properties in the Inspector:** probability, velocity range for randomization, micro-timing, release velocity, and per-note expression (pitch, pressure, timbre) using MIDI 2.0 / MPE, VST3 note expression, and CLAP note expression where the instrument supports them.
- **Transform panel (Depth layer):** humanize, strum, arpeggiate (rendered into notes, so it stays editable), legato, flip, reverse, and scale-quantize.
- **Assistive features are opt-in and never inserted automatically:** a "suggest next chord" option and scale detection. No generative AI in the core flow.

### 6.3 Performance targets

- A note appears under the cursor in the **same frame** as the click (≤8 ms at 120 Hz).
- Audition latency is the audio buffer plus at most one block.
- Scrolling and zooming stay at 120 fps with 20,000 notes in a pattern on an M-series Mac.

---

## 7. Design system (summary)

| Token family | Decision |
|---|---|
| **Type** | SF Pro Text/Display for UI; SF Mono or SF Pro with **tabular figures** for every numeric readout, so numbers never jitter; 6 sizes in total (11, 12, 13, 15, 20, 28 pt) |
| **Spacing** | 4 pt base grid. Panel padding is 12 or 16. Row heights are 24 (dense) and 28 (default) |
| **Color** | Dark-first neutral graphite scale (6 steps). **One accent color** for selection and focus. **Track colors** come from a curated 12-hue palette with equalized perceived lightness (OKLCH), so no color shouts. Semantic colors are reserved: red only for record and clip-over, amber for warnings |
| **Borders** | Avoided. Hierarchy comes from surface elevation (background steps) and spacing. Hairlines appear only between editors |
| **Icons** | SF Symbols where a fitting symbol exists. Custom icons are drawn on the SF Symbols grid and weight, so they look the same in text |
| **Controls** | Linear faders and drag-numbers by default. Knobs appear only inside instrument and effect panels, where the analog metaphor helps. Every value is double-click-editable, ⌥-click resets, ⌘-drag is fine adjustment |
| **Motion** | 120–180 ms, ease-out, or critically damped springs for panels. **No animation during direct manipulation**, where the content tracks the pointer 1:1. Reduce Motion is honored |
| **Appearance** | Dark first. Light appearance comes in Phase 6, because studio users overwhelmingly use dark, and doing both well from day one doubles the visual QA |
| **Scaling** | Interface scale at 90/100/115/130 % in Settings, independent of system text size, because dense editors need their own control |

The full token tables will be created at the start of Phase 2 as code (a `DesignTokens` module), not as a static document that drifts out of date.

---

## 8. Accessibility

Custom-drawn editors are invisible to VoiceOver by default. That is a real cost we're accepting by drawing them ourselves (see the architecture document). Plan:

- Every custom canvas exposes an `NSAccessibilityElement` tree: tracks, clips and notes with roles, labels and values. For example: "Note C4, beat 3.2, length 1/8, velocity 96".
- A **full keyboard path** for every editor: arrow keys move focus between notes and clips, ⏎ edits, ⌥-arrows nudge.
- Contrast checked against WCAG AA for text. Meters and state are never conveyed by color alone.
- Shortcuts are customizable from Phase 6, built on the command registry, which exists from Phase 2.

---

## 9. Business model, and where it touches engineering

- **One-time purchase. Every user keeps the version they bought forever and it keeps working offline forever.**
- **Update policy (recommendation):** a purchase includes all updates for that major version (1.x). Major versions arrive no more often than every ~2 years, with a discounted upgrade. Anyone who buys within 90 days before a major release gets it free.
  - *Challenge:* FL Studio gives lifetime free updates. Matching that is a strong marketing weapon but risky for a small company's revenue. My recommendation is the major-version model above, framed honestly. Decide before launch, not now.
- **Licensing implementation:** a signed license file (Ed25519). The app verifies it offline with an embedded public key. Activation is online once, with an offline challenge-response alternative. Up to 3 machines. **No periodic check-in is ever required.** Failure mode: if the license can't be verified, the app keeps opening and playing every project but disables saving. It never locks the user out of their own work.
- Accepting that cracks will exist is a feature: heavy DRM costs more in support and goodwill than it recovers.
- **Distribution is outside the Mac App Store** (Developer ID plus notarization). The App Store's sandbox is fundamentally hostile to hosting third-party VST3/AU plugins. See [04-plugin-hosting.md](04-plugin-hosting.md). Payments go through a merchant of record such as Paddle or FastSpring, which handles VAT and sales tax worldwide.
- **Optional paid expansions:** sound packs and instruments. The core never nags.
