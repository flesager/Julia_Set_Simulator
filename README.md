# Julia Set Simulator

A real-time Julia set fractal renderer written in C++, targeting both **native desktop** and **WebAssembly**. The UI is built with [Dear ImGui](https://github.com/ocornut/imgui); the build system is [Bazel](https://bazel.build).

> Primarily developed with [Claude Code](https://claude.ai/claude-code) (Anthropic).

---

## Features

- Interactive ImGui control panel: Julia *c* parameter (sliders + `−`/`+` fine-step buttons), zoom, pan, max iterations
- **Mouse interaction directly on the fractal**: left-drag to pan, scroll wheel to zoom toward the cursor
- Start / Stop the render loop at any time
- Native: tile-based parallel computation via a reactor thread pool (`core/engine`)
- WASM: dual-binary with runtime detection — multi-threaded CoreEngine pipeline when SharedArrayBuffer is available (COOP+COEP), single-threaded fallback otherwise (e.g. GitHub Pages)
- WASM UI shows the active mode: **Multi-thread** (green) or **Single-thread / fallback** (orange)

---

## Requirements

### Native (Linux)

Run the provided install script to set up all host dependencies in one step:

```bash
bash install.sh
```

What it installs:
- System packages via `apt`: `build-essential`, `git`, `curl`, `libsdl2-dev`, `libgl-dev`, `python3`
- [Bazelisk](https://github.com/bazelbuild/bazelisk) v1.29.0 → `~/.local/bin/bazel`

Run `bash install.sh --help` for a full description.

**Managed by Bazel automatically (no manual installation needed):**
- C++ toolchain (GCC / Clang)
- GoogleTest (tests only)
- Dear ImGui 1.91.9b

### WebAssembly

**Managed by Bazel automatically:**
- Emscripten SDK 6.0.0 (via `emsdk` Bazel module)
- Dear ImGui 1.91.9b

No host libraries required for the WASM build.

---

## Build & Run

### Native desktop app

```bash
~/.local/bin/bazel run //app:julia_app
```

### Tests

```bash
~/.local/bin/bazel test //core/... //common/... --test_timeout=120
```

### WebAssembly dist

```bash
# Build dist/ (index.html + julia_st.js/.wasm + julia_mt.js/.wasm + style.css)
~/.local/bin/bazel build //web:dist

# Serve locally (plain HTTP — single-threaded binary will load)
python3 -m http.server 8080 --directory bazel-bin/web/dist
# open http://localhost:8080

# Serve with COOP+COEP headers (multi-threaded binary will load)
# e.g. using a server that sets:
#   Cross-Origin-Opener-Policy: same-origin
#   Cross-Origin-Embedder-Policy: require-corp
```

Pushes to `main` deploy automatically to GitHub Pages via `.github/workflows/pages.yml`.

---

## Project structure

```
various_wasm/
├── MODULE.bazel              # Bzlmod deps: rules_cc, emsdk, googletest, imgui
├── .bazelrc                  # Global Bazel flags
├── install.sh                # Host environment setup (apt packages + Bazelisk)
├── .github/workflows/
│   └── pages.yml             # CI: test → build WASM dist → deploy to Pages
│
├── core/engine/              # Reactor thread pool (native + WASM MT)
│   └── README.md
│
├── common/                   # Tile-based Julia pipeline (native + WASM MT)
│   ├── compute/              #   ComputeProcObj  — per-tile Julia iteration
│   ├── assembler/            #   FrameAssemblerProcObj — collects tiles → frame
│   ├── controller/           #   FrameControllerProcObj — frame loop + FPS throttle
│   └── README.md
│
├── app/                      # Application layer
│   ├── main.cpp              #   Native entry point
│   ├── app.cpp / app.hpp     #   App: wires engine + common pipeline + ImGui
│   ├── imgui_layer.cpp/.hpp  #   Native ImGui/SDL2/OpenGL3 render loop
│   ├── wasm_main.cpp         #   WASM ST entry point (standalone, no CoreEngine)
│   ├── wasm_mt_main.cpp      #   WASM MT entry point (App + CoreEngine, -sPTHREAD)
│   ├── BUILD.bazel           #   julia_app + julia_st_wasm + julia_mt_wasm
│   └── README.md
│
└── web/                      # Static web assets
    ├── index.html
    ├── style.css
    └── BUILD.bazel           # dist genrule: assembles WASM outputs + web assets
```

---

## Architecture

The three targets (native, WASM ST, WASM MT) share the same Julia iteration math and ImGui control layout but differ fundamentally in their threading and platform models.

### Native — reactor pipeline

```
main thread (ImGui render loop)
    │
    └── App ──► CoreEngine (N worker threads)
                    ├── ComputeProcObj ×M   ← compute tiles in parallel
                    ├── FrameAssemblerProcObj ← collect tiles → frame
                    └── FrameControllerProcObj ← pace frame rate, dispatch next frame
```

Tiles are distributed across *M* compute procs by the controller (round-robin). The assembled frame is double-buffered so the ImGui thread can read the latest frame lock-free.

### WASM — dual-binary with runtime detection

`index.html` checks `window.crossOriginIsolated` before loading any script:

```javascript
script.src = crossOriginIsolated ? 'julia_mt.js' : 'julia_st.js';
```

**Single-threaded fallback** (`julia_st.js/.wasm` — loads everywhere):
```
browser event loop (requestAnimationFrame)
    └── render_frame()
            ├── compute_frame()   ← all pixels, synchronous, this thread
            ├── glTexSubImage2D() ← upload result to GPU
            └── ImGui             ← handle events + draw UI
```

**Multi-threaded** (`julia_mt.js/.wasm` — requires COOP+COEP headers):
```
browser event loop (requestAnimationFrame)
    └── render_frame()
            ├── app.get_latest_frame()  ← double-buffered, lock-free read
            ├── glTexSubImage2D()       ← upload latest frame to GPU
            └── ImGui                  ← handle events + draw UI
Web Workers (via -sPTHREAD):
    └── CoreEngine worker pool
            ├── ComputeProcObj ×3   ← compute tiles in parallel
            ├── FrameAssemblerProcObj ← collect tiles → assembled frame
            └── FrameControllerProcObj ← pace frame rate, dispatch next
```

See [app/README.md](app/README.md) for the full explanation of the two architectures.

---

## Threading constraints for WASM

Running the `CoreEngine` reactor in WebAssembly requires `std::thread`, which Emscripten only supports with `-sPTHREAD` and `SharedArrayBuffer`.

`SharedArrayBuffer` was disabled in all major browsers in January 2018 following the disclosure of the **Spectre and Meltdown CPU vulnerabilities**. Those attacks exploit speculative execution to read arbitrary memory via high-resolution timers; shared memory between threads is an implicit high-resolution timer, so browsers removed it as a mitigation. It was re-enabled in 2020 only for pages that opt into **cross-origin isolation**, verified by two HTTP response headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

GitHub Pages does not allow custom response headers, so those headers cannot be set and `SharedArrayBuffer` remains unavailable there. The WASM build therefore ships **two binaries**: `julia_st` (single-threaded, loads anywhere) and `julia_mt` (multi-threaded with CoreEngine, requires COOP+COEP). `index.html` detects `crossOriginIsolated` at runtime and loads the appropriate one.
