# Twilight Princess Multiplatform Port Plan (GCN-First, Time-to-Ship Optimized)

## Scope

- Canonical game behavior/content: **GameCube version**.
- Target platforms: **Windows, Linux, macOS, NX Switch (homebrew)**.
- Primary rendering backend for first ship: **OpenGL**.
- Tooling direction: modern C++ build with **CMake**.
- Development tooling: **ImGui** integrated from the start.

## Platform Matrix

| Platform | Graphics | Platform Layer |
|---|---|---|
| Windows | OpenGL 4.5 | SDL3 |
| Linux | OpenGL 4.5 | SDL3 |
| macOS | OpenGL 4.1 | SDL3 |
| NX Homebrew | OpenGL 4.3 (Mesa/Nouveau) | libnx + EGL |

## Core Principles

- **GCN-first semantics**: preserve GameCube gameplay feel, camera behavior, and content assumptions.
- **One renderer first**: OpenGL everywhere for initial ship.
- **Boundary-only porting**: convert at PAL and GX boundaries; avoid rewriting gameplay systems.
- **Fail-soft, not fail-hard**: unknown GX paths render with safe defaults + log once.
- **Playable-first**: prioritize title -> field -> combat -> menu/save flow over perfect visual parity.

## What We Intentionally Skip Early

- Wii-specific systems (WPAD/KPAD, Wii speaker, Z2AudioCS-specific paths).
- Exact parity for heavy post-processing (DoF/bloom/motion blur) in first playable.
- Mirror-heavy and movie paths in first pass.
- Full async disc emulation (threaded DVD behavior).
- Full GX API upfront (implement hot path first, long tail on demand).
- Broad settings UI and advanced remapping in v1 (basic stable mapping first).

## Architecture

```
Game Logic (d/, d/actor/)
  -> Framework (f_pc/, f_op/, f_ap/)
    -> Machine Layer (m_Do/)
      -> PAL (src/pal/)
         -> GX Shim (src/pal/gx/) -> OpenGL
         -> Platform Services (window/input/audio/fs/os/save)
```

- Keep game logic/framework mostly intact.
- Replace hardware/platform dependencies behind PAL.
- Route GX API calls to a GX shim that translates to OpenGL.

## Separation of Concerns

- `src/d`, `src/d/actor`, `src/f_*`: no direct platform headers, no OpenGL headers.
- `src/m_Do`: interacts with PAL-facing interfaces only.
- `src/pal/gx`: owns GX -> OpenGL translation and render state.
- `src/pal`: owns SDL3/libnx/EGL/input/audio/fs/save implementations.
- `src/pal/imgui`: owns debug UI and runtime tooling; no gameplay ownership.

## Step-by-Step Execution Plan

### Step 1: Build Modernization Without Big-Bang Deletion

- Introduce root CMake build.
- Keep legacy directories in tree initially, but exclude from first modern build target.
- Convert compiler-specific MWCC constructs only where needed to compile.
- Keep code motion minimal to avoid destabilization.

### Step 2: Flatten Version Branching Toward GCN Behavior

- For mixed GCN/Wii code paths, keep GCN behavior as canonical.
- Remove/defer Wii-only input/speaker/control features.
- Keep platform conditionals only where platform APIs differ (PC vs NX).

### Step 3: PAL Bootstrap (Minimal Surface)

- Implement `pal_window`, `pal_os`, `pal_fs`, `pal_input`, `pal_audio`, `pal_save`.
- PC side: SDL3.
- NX side: libnx + EGL + mesa.
- Boot to running main loop with window + ImGui overlay.

### Step 4: Replace DVD Async Internals With Synchronous Completion

- Keep `mDoDvdThd_*` interfaces unchanged.
- Execute load work immediately in `create(...)`, mark command done.
- Preserve callsites and polling logic (`sync()`) without thread/queue complexity.

### Step 5: ARAM-to-RAM Simplification

- Preserve JKR/JAudio interfaces but collapse ARAM-backed behavior into RAM-backed behavior.
- Keep mount/load contracts stable; remove hardware transfer assumptions internally.

### Step 6: GX Shim Bring-Up in Priority Tiers

#### Tier A (first playable)

- GX state tracker basics.
- Vertex format + matrix upload + draw submission.
- Texture upload/decoding for common formats.
- TEV basics for dominant material paths.
- Display list replay for J3D draw flow.

#### Tier B (expand by demand)

- Add missing GX functions when encountered in real scenes.
- Keep one-time warning logs and counters for missing symbols.
- Avoid implementing rare functions until they block progression.

### Step 7: Visual Scope for First Playable

- Prioritize correctness in core gameplay scenes and essential UI.
- Defer exact parity in expensive edge paths (mirrors, heavy weather, movie player).
- Use feature toggles in ImGui to disable problematic effects at runtime.

### Step 8: Audio Bring-Up in Two Phases

- Phase A: null/stub output path so gameplay loop is unblocked.
- Phase B: replace hardware-coupled JAudio2 portions with software equivalents behind PAL audio.
- Keep higher-level Z2/game audio logic unchanged where possible.

### Step 9: Input and Save Stability

- Start with one stable GCN-style gamepad mapping.
- Add keyboard/mouse later where useful.
- Keep save format behavior stable; platform-specific file locations behind PAL.

### Step 10: NX Homebrew Bring-Up

- Reuse same GX/OpenGL renderer.
- Swap only platform services (window/input/audio/fs/os/save) to libnx/EGL/audren/hid.
- Resolve driver quirks with local workarounds rather than renderer redesign.

### Step 11: Polish for Shipment

- Validate core progression loop and menu/save reliability.
- Add missing GX long-tail only when blocking content.
- Package outputs for PC and NX homebrew.
- Keep advanced visual parity and non-critical systems as post-ship enhancements.

## High-Impact Time Savers (Confirmed)

- Keep API shapes, replace internals (DVD/ARAM/audio backend) to minimize callsite churn.
- Implement high-frequency GX subset first; long-tail by telemetry.
- Use ImGui to control runtime feature gates and debug visibility.
- Build a narrow playable vertical slice before broad parity work.
- Avoid giant refactors up front; defer broad deletions until modern path is stable.

## Known Heavy Areas To Defer or Gate

- `src/d/d_kankyo_rain.cpp` (high GX volume).
- `src/d/actor/d_a_mirror.cpp` (special rendering path).
- `src/d/actor/d_a_movie_player.cpp` (video/display complexity).
- `src/d/d_home_button.cpp` (non-core overlay path).

## Operational Rules During Port

- Never block progress on rare visual edge cases.
- Any unsupported render feature gets a safe fallback + diagnostic log.
- Keep gameplay logic untouched unless a blocker demands targeted edits.
- Prefer additive shims over invasive rewrites.
- Keep first ship focused on stable gameplay over perfect parity.

## Post-Ship Backlog (Intentionally Deferred)

- Exact post-processing parity.
- Full feature parity for all niche overlays and scenes.
- Wider input remapping UX.
- Additional graphics backend experimentation beyond OpenGL.
- Deep visual/performance tuning beyond required playability.
