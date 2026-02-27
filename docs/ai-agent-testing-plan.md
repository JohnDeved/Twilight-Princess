# AI Agent Automated Port Testing Plan

## Goal

Enable AI agents (Copilot, Claude, etc.) to **build, run, and evaluate** the PC port in CI
without human intervention. The agent should determine: does the game boot? How far does it
get? What broke? — and then fix it automatically.

## Architecture

```
GitHub PR (agent pushes code)
  → CI workflow (.github/workflows/port-test.yml)
    → Build PC port (CMake + host compiler)
    → Run headless with instrumented PAL
    → Collect structured log (JSON milestones)
    → Upload log + framebuffer captures as artifacts
  → Agent reads artifacts via GitHub API
    → Parses milestone progress
    → Identifies first failure point
    → Pushes fix → re-triggers CI
```

## 1. Headless Rendering Mode (~50 LOC)

The port already uses bgfx. bgfx has a **software rasterizer backend** (`bgfx::RendererType::Noop`)
that requires zero GPU. In CI:

```c
// pal_window.cpp — CI mode
bgfx::Init init;
if (getenv("TP_HEADLESS")) {
    init.type = bgfx::RendererType::Noop;  // no GPU needed
} else {
    init.type = bgfx::RendererType::Count;  // auto-detect
}
```

For visual regression, use bgfx's **software renderer** (`bgfx::RendererType::Count` with
`BGFX_CONFIG_RENDERER_OPENGLES=0` + Mesa `llvmpipe`) in CI containers that have Mesa installed.
This produces real framebuffers without a physical GPU.

**Framebuffer capture** (~30 LOC): At configurable frame intervals, read back the bgfx
framebuffer via `bgfx::readTexture()` and write a PNG. Upload as CI artifact.

## 2. Boot Milestone Instrumentation (~200 LOC)

Insert lightweight log markers at known game boot stages. These print structured JSON to stdout
that the CI harness parses. **Zero impact on game logic** — all markers go in PAL/shim code
that only exists in the port build.

### Milestone sequence (maps to existing code flow)

| # | Milestone | Where to instrument | What it proves |
|---|---|---|---|
| 0 | `BOOT_START` | `main()` entry in `m_Do_main.cpp` | Binary launched successfully |
| 1 | `HEAP_INIT` | After `mDoMch_Create()` returns | JKRHeap arena + dual heaps working |
| 2 | `GFX_INIT` | After `mDoGph_Create()` returns | bgfx initialized, GX shim ready |
| 3 | `PAD_INIT` | After `mDoCPd_c::create()` | Input subsystem ready |
| 4 | `FRAMEWORK_INIT` | After `fapGm_Create()` | Process manager + scene system ready |
| 5 | `FIRST_FRAME` | First `fapGm_Execute()` return | Game loop running |
| 6 | `LOGO_SCENE` | `dScnLogo_c` create callback | Logo scene loaded (PROC_LOGO_SCENE) |
| 7 | `TITLE_SCENE` | `dScnTitle` create callback | Title screen reached (PROC_TITLE) |
| 8 | `PLAY_SCENE` | `dScnPly_c` create callback | Gameplay scene loaded (PROC_PLAY_SCENE) |
| 9 | `STAGE_LOADED` | `dComIfGp_getStartStageName()` logged | Specific stage/room loaded |
| 10 | `FRAMES_60` | Frame counter ≥ 60 in main loop | Sustained execution (no crash in first second) |
| 11 | `FRAMES_300` | Frame counter ≥ 300 | 5 seconds of gameplay — likely stable |
| 12 | `FRAMES_1800` | Frame counter ≥ 1800 | 30 seconds — extended stability |

### Log format

```json
{"milestone": "HEAP_INIT", "id": 1, "time_ms": 42, "detail": "rootHeap=0x1000000 sysHeap=0x400000"}
{"milestone": "GFX_INIT", "id": 2, "time_ms": 85, "detail": "bgfx=Noop 640x480"}
{"milestone": "FIRST_FRAME", "id": 5, "time_ms": 230, "frame": 1}
{"milestone": "LOGO_SCENE", "id": 6, "time_ms": 310, "detail": "PROC_LOGO_SCENE"}
{"milestone": "FRAMES_60", "id": 10, "time_ms": 1230, "frame": 60}
{"milestone": "STAGE_LOADED", "id": 9, "time_ms": 5200, "detail": "F_SP103 room=0"}
```

### Implementation

```c
// pal_milestone.h — ~40 LOC
#pragma once
#include <stdio.h>
#include <time.h>

static unsigned long s_boot_time_ms;

inline void pal_milestone_init() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_boot_time_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

inline void pal_milestone(const char* name, int id, const char* detail) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    fprintf(stdout, "{\"milestone\":\"%s\",\"id\":%d,\"time_ms\":%lu,\"detail\":\"%s\"}\n",
            name, id, now - s_boot_time_ms, detail ? detail : "");
    fflush(stdout);
}

inline void pal_milestone_frame(const char* name, int id, unsigned frame) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    fprintf(stdout, "{\"milestone\":\"%s\",\"id\":%d,\"time_ms\":%lu,\"frame\":%u}\n",
            name, id, now - s_boot_time_ms, frame);
    fflush(stdout);
}
```

**Placement**: 12 one-line `pal_milestone()` calls in PAL code and `m_Do_main.cpp` conditionals.
Game logic files stay untouched.

## 3. Timeout + Exit Harness (~30 LOC)

The game loop (`do { ... } while (true)` in `main01()`) never exits. For CI testing:

```c
// In main loop, after fapGm_Execute():
#if PLATFORM_PC
    if (getenv("TP_TEST_FRAMES")) {
        static u32 max_frames = atoi(getenv("TP_TEST_FRAMES"));
        if (frame >= max_frames) {
            pal_milestone("TEST_COMPLETE", 99, "max_frames_reached");
            exit(0);
        }
    }
#endif
```

CI runs with `TP_TEST_FRAMES=1800` (30 seconds at 60fps). The process self-terminates after
the target frame count. CI also wraps with `timeout 120s` as a hard kill if the game hangs.

## 4. Crash Capture (~20 LOC)

```c
// pal_crash.cpp — signal handler
#include <signal.h>
#include <execinfo.h>

static void crash_handler(int sig) {
    fprintf(stderr, "{\"milestone\":\"CRASH\",\"id\":-1,\"signal\":%d}\n", sig);
    void* bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

void pal_crash_init() {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE, crash_handler);
}
```

Crash output goes to stderr. CI harness captures both stdout (milestones) and stderr (crashes).

## 5. GX Stub Coverage Tracker (~80 LOC)

Track which GX functions are called but not yet implemented:

```c
// gx_stub_tracker.h
static u32 gx_stub_hits[256];  // indexed by GX function enum
static const char* gx_stub_names[256];

inline void gx_stub_hit(int id, const char* name) {
    if (gx_stub_hits[id]++ == 0) {
        fprintf(stderr, "{\"stub_hit\":\"%s\",\"id\":%d}\n", name, id);
    }
}

// At exit or FRAMES_1800:
void gx_stub_report() {
    for (int i = 0; i < 256; i++) {
        if (gx_stub_hits[i] > 0) {
            fprintf(stdout, "{\"stub\":\"%s\",\"hits\":%u}\n",
                    gx_stub_names[i], gx_stub_hits[i]);
        }
    }
}
```

This tells the agent: "GXSetFog was called 47 times but is a stub — implement it next."

## 6. CI Workflow (~80 LOC YAML)

```yaml
# .github/workflows/port-test.yml
name: Port Test (PC Headless)

on:
  push:
    paths: ['src/pal/**', 'CMakeLists.txt', 'include/pal/**']
  pull_request:
    paths: ['src/pal/**', 'CMakeLists.txt', 'include/pal/**']

jobs:
  test:
    runs-on: ubuntu-latest
    timeout-minutes: 10
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }

      - name: Install dependencies
        run: |
          sudo apt-get update -q
          sudo apt-get install -y cmake ninja-build libsdl3-dev mesa-utils libegl1-mesa-dev

      - name: Build PC port
        run: |
          cmake -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DTP_VERSION=PC \
            -DTP_HEADLESS=ON
          ninja -C build

      - name: Run headless test
        env:
          TP_HEADLESS: "1"
          TP_TEST_FRAMES: "1800"
        run: |
          timeout 120s build/tp-pc 2>crash.log | tee milestones.log
          EXIT_CODE=$?
          echo "exit_code=$EXIT_CODE" >> $GITHUB_OUTPUT

      - name: Parse milestones
        if: always()
        run: |
          python3 tools/parse_milestones.py milestones.log \
            --output milestone-summary.json \
            --min-milestone 5

      - name: Upload test artifacts
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: port-test-results
          path: |
            milestones.log
            crash.log
            milestone-summary.json
            screenshots/*.png
```

## 7. Milestone Parser Tool (~100 LOC Python)

```python
#!/usr/bin/env python3
# tools/parse_milestones.py
"""Parse milestone JSON lines and produce summary for AI agent."""
import json, sys, argparse

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logfile")
    parser.add_argument("--output", default="milestone-summary.json")
    parser.add_argument("--min-milestone", type=int, default=0,
                        help="Fail if highest milestone < this value")
    args = parser.parse_args()

    milestones = []
    stubs = []
    crash = None

    with open(args.logfile) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"): continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "milestone" in obj:
                milestones.append(obj)
                if obj["milestone"] == "CRASH":
                    crash = obj
            elif "stub" in obj:
                stubs.append(obj)

    highest = max((m["id"] for m in milestones if m["id"] >= 0), default=-1)
    last = milestones[-1] if milestones else None

    summary = {
        "highest_milestone": highest,
        "last_milestone": last,
        "crash": crash,
        "total_milestones": len([m for m in milestones if m["id"] >= 0]),
        "stubs_hit": sorted(stubs, key=lambda s: -s.get("hits", 0))[:20],
        "pass": highest >= args.min_milestone and crash is None,
    }

    with open(args.output, "w") as f:
        json.dump(summary, f, indent=2)

    # Print human-readable summary
    print(f"\n{'='*60}")
    print(f"PORT TEST SUMMARY")
    print(f"{'='*60}")
    print(f"Highest milestone: {highest} ({last['milestone'] if last else 'NONE'})")
    if crash:
        print(f"CRASH: signal {crash.get('signal', '?')}")
    if stubs:
        print(f"Top unimplemented stubs:")
        for s in summary["stubs_hit"][:10]:
            print(f"  {s['stub']}: {s['hits']} hits")
    print(f"Result: {'PASS' if summary['pass'] else 'FAIL'}")
    print(f"{'='*60}\n")

    sys.exit(0 if summary["pass"] else 1)

if __name__ == "__main__":
    main()
```

## 8. AI Agent Workflow

The agent follows this loop for each port task:

```
1. Read milestone-summary.json from latest CI run
2. Identify highest_milestone reached
3. If crash: read crash.log backtrace → locate failing code → fix
4. If stuck at milestone N: implement what's needed for N+1
5. If stubs_hit: implement most-hit stub first (biggest impact)
6. Push fix → CI runs automatically → repeat from step 1
```

### Decision table for the agent

| Highest milestone | Agent action |
|---|---|
| -1 (no output) | Fix build errors — won't compile |
| 0 `BOOT_START` | Fix `mDoMch_Create` — heap init crash |
| 1 `HEAP_INIT` | Fix `mDoGph_Create` — GX/bgfx init issue |
| 2 `GFX_INIT` | Fix input init or PAL bootstrap |
| 3–4 `PAD/FRAMEWORK` | Fix `fapGm_Create` — process manager setup |
| 5 `FIRST_FRAME` | Game loop runs but scene fails to load — fix DVD/archive loading |
| 6 `LOGO_SCENE` | Logo loads but title doesn't — fix scene transition or asset loading |
| 7 `TITLE_SCENE` | Title works but gameplay doesn't — fix `dScnPly_c` or stage loading |
| 8 `PLAY_SCENE` | Gameplay scene loads — focus on GX stubs and rendering |
| 10+ `FRAMES_60+` | Sustained execution — fix top stub hits from coverage report |

### What the agent reads from CI

1. **Build log** — compiler errors, linker errors
2. **milestones.log** — structured milestone progress
3. **crash.log** — signal + backtrace for crashes
4. **milestone-summary.json** — parsed summary with stub hits
5. **screenshots/*.png** — visual state at key frames (if using Mesa software renderer)

## 9. Visual Regression (Optional — Mesa llvmpipe)

For actual framebuffer verification without a GPU, CI can use Mesa's `llvmpipe` software
renderer with bgfx's OpenGL backend:

```yaml
- name: Run with software rendering
  env:
    LIBGL_ALWAYS_SOFTWARE: "1"
    GALLIUM_DRIVER: llvmpipe
    TP_TEST_FRAMES: "300"
    TP_SCREENSHOT_FRAMES: "60,120,180,240,300"
  run: timeout 60s build/tp-pc 2>crash.log | tee milestones.log
```

Framebuffer captures at specific frames let the agent (or a human) visually verify rendering
progress. The agent can compare screenshots across commits to detect rendering regressions.

## 10. Auto-Input for Scene Progression (~60 LOC)

To test beyond the title screen, the PAL input layer can inject synthetic inputs:

```c
// pal_input.cpp — test input sequence
static const struct { int frame; u16 button; } test_inputs[] = {
    { 120, PAD_BUTTON_START },    // skip logo at 2s
    { 180, PAD_BUTTON_START },    // start game at 3s
    { 240, PAD_BUTTON_A },        // confirm at 4s
    { 300, PAD_BUTTON_A },        // confirm again
    { 0, 0 }                      // sentinel
};

void pal_input_inject(u32 frame, PADStatus* status) {
    if (!getenv("TP_TEST_INPUTS")) return;
    for (int i = 0; test_inputs[i].frame; i++) {
        if (frame == test_inputs[i].frame) {
            status->button = test_inputs[i].button;
        }
    }
}
```

This lets CI automatically navigate: logo → title → new game → Ordon village, proving the
full boot path without human interaction.

## LOC Summary

| Component | LOC | Notes |
|---|---|---|
| `pal_milestone.h` | ~40 | Milestone logging macros |
| Milestone call sites | ~12 | One line each in PAL/m_Do code |
| `pal_crash.cpp` | ~20 | Signal handler + backtrace |
| Timeout/exit harness | ~10 | Frame-count exit in main loop |
| Headless bgfx init | ~10 | Noop renderer selection |
| Framebuffer capture | ~30 | `bgfx::readTexture` → PNG |
| GX stub tracker | ~80 | Hit counter + report |
| `tools/parse_milestones.py` | ~100 | CI log parser |
| `port-test.yml` | ~80 | CI workflow |
| Auto-input injection | ~60 | Synthetic PAD inputs for test |
| **Total** | **~440** | All additive — no game logic changes |

## Integration with Port Plan

This testing infrastructure is **built alongside Step 1 (CMake)**. The milestone system activates
from the first successful compile:

- **Step 1 complete** → CI reports milestone 0 (BOOT_START)
- **Step 3 complete** → CI reports milestones 0–4 (all PAL init done)
- **Step 5 in progress** → CI reports milestones up to 6–8 (scenes loading)
- **Step 8 target** → CI reports milestone 11+ (sustained 5s+ gameplay)

The agent can work on Steps 3–7 in parallel, with CI continuously reporting which milestone
each commit reaches. No human needs to run the game manually at any point.
