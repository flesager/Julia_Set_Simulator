# app — Application layer

`app/` contains three entry points that share the ImGui control layout but differ in threading and platform model:

| Entry point | Target | Threading |
|---|---|---|
| `main.cpp` | native | SDL2 window + CoreEngine worker pool |
| `wasm_main.cpp` | WASM ST (`julia_st`) | single-threaded, synchronous compute |
| `wasm_mt_main.cpp` | WASM MT (`julia_mt`) | CoreEngine via Web Workers (`-sPTHREAD`) |

---

## Native target (`julia_app`)

```
main.cpp  →  App  →  ImGuiLayer (SDL2 + OpenGL3, resizable window)
                 └►  CoreEngine  (hardware_concurrency() worker threads)
                          ├── ComputeProcObj ×(hardware_concurrency−1)
                          ├── FrameAssemblerProcObj
                          └── FrameControllerProcObj
```

**Build & run:**
```bash
~/.local/bin/bazel run //app:julia_app
# requires: sudo apt install libsdl2-dev libgl-dev  (or: bash install.sh)
```

### App (`app.cpp` / `app.hpp`)

`App` is the coordinator between the ImGui UI thread and the `CoreEngine` reactor:

- `setup()` — creates the three pipeline ProcObjs, wires them up (`assembler->set_controller(controller)`), starts the engine, and adds the ProcObjs in the correct order (compute first, controller last so its `start()` fires after all receivers are registered).
- `teardown()` — stops the engine, then clears the shared\_ptrs so ProcObjs are destroyed cleanly.
- `restart(num_workers, num_compute_procs)` — calls `teardown()` + `setup()` with new topology; invoked from the UI when the user changes thread/proc counts.
- `set_display_size(w, h)` — tears down and restarts the pipeline at a new resolution while preserving all view parameters (zoom, center, Julia *c*, etc.), called when the user resizes the window.
- `set_view(zoom, cx, cy)` — combined zoom+center setter that stores all three atomics before waking the controller, ensuring `dispatch_frame()` always reads a consistent snapshot.
- `run()` — native: creates an `ImGuiLayer` and blocks until the window is closed. Under `__EMSCRIPTEN__` the body is compiled out (guarded with `#ifndef __EMSCRIPTEN__`); the render loop is driven by `emscripten_set_main_loop` in the WASM entry points instead.

### ImGuiLayer (`imgui_layer.cpp` / `imgui_layer.hpp`)

Owns the SDL2 window (maximised on startup, freely resizable), OpenGL 3.3 Core context, and Dear ImGui lifecycle.

**Per frame:**
1. Poll SDL events → forward to ImGui; handle `SDL_WINDOWEVENT_RESIZED` by calling `resize_to()`.
2. Call `app.get_latest_frame()` → upload to a `GL_RGBA` texture via `glTexSubImage2D`.
3. Build the ImGui UI (`build_ui`): compute UV sub-rectangle from `get_cache_info()`, draw fractal, handle mouse input.
4. Render ImGui draw data → `SDL_GL_SwapWindow`.

**`resize_to(win_w, win_h)`** updates `img_w_/img_h_` and `cache_w_/cache_h_`, reallocates the GL texture, then calls `app.set_display_size()` which restarts the pipeline with all current view parameters preserved.

**UV sub-rectangle rendering:** the oversized cache texture (1.5× per dimension) is uploaded once per computed frame. Pan and zoom within cache bounds update only the UV sub-rect passed to `ImGui::Image(tex, size, uv0, uv1)` — no recomputation needed. A recompute is triggered only when the viewport leaves the cached region or exceeds the zoom margin.

**UI controls:**

| Section | Controls |
|---|---|
| Status | Last frame calculation time (ms) |
| Julia c | 2D drag picker (±2 on each axis) + ±0.0005 fine-step buttons; value readout below |
| Viewport | zoom (logarithmic, 0.01 … 5000), center x/y (−2 … +2) |
| Performance | target FPS (0 = unlimited), max iterations (16 … 4096) |
| Engine | threads (1 … 100), compute procs (1 … 100) + "Apply & restart" |

**Mouse interaction on the fractal viewport:**

| Gesture | Effect |
|---|---|
| Left-button drag | Pan — the complex point under the cursor at drag-start tracks the cursor throughout the drag |
| Scroll wheel | Zoom toward cursor — the complex point under the cursor stays fixed as zoom changes |

The drag pivot and zoom anchor are computed in the complex plane: `zr = (px/w − 0.5) × scale × aspect + center_x`, `zi = (py/h − 0.5) × scale + center_y`. Zoom and center are updated together via `app.set_view()` in a single atomic store+wake to avoid dispatching a frame with mismatched parameters.

---

## WASM targets

The WASM build ships **two binaries** selected at runtime by `index.html`:

| Condition | Binary loaded | Entry point | Mode label in UI |
|---|---|---|---|
| `crossOriginIsolated === true` (COOP+COEP headers present) | `julia_mt.js/.wasm` | `wasm_mt_main.cpp` | **Multi-thread** (green) |
| Otherwise (GitHub Pages, plain HTTP, etc.) | `julia_st.js/.wasm` | `wasm_main.cpp` | **Single-thread (fallback)** (orange) |

The active mode is shown at the top of the controls panel so it is always clear which binary is running.

### Single-threaded fallback (`julia_st_wasm`)

```
wasm_main.cpp  →  emscripten_set_main_loop(render_frame)
                        ├── handle_fractal_input()  (pan + zoom from mouse/wheel)
                        ├── compute_frame()          (synchronous, all pixels this thread)
                        ├── glTexSubImage2D()        (upload to WebGL2 texture)
                        └── ImGui                   (OpenGL3 renderer backend only)
```

```bash
~/.local/bin/bazel build //app:julia_st_wasm
# outputs: bazel-bin/app/julia_st_wasm/julia_st.js + julia_st.wasm
```

### Multi-threaded (`julia_mt_wasm`)

```
wasm_mt_main.cpp  →  emscripten_set_main_loop(render_frame)
                           ├── handle_fractal_input()   (pan + zoom from mouse/wheel)
                           ├── app.get_latest_frame()   (double-buffered, lock-free)
                           ├── glTexSubImage2D()         (upload to WebGL2 texture)
                           └── ImGui                     (OpenGL3 renderer backend)
Web Workers (-sPTHREAD):
    └── CoreEngine (hardware_concurrency() threads)
            ├── ComputeProcObj ×(hardware_concurrency−1)  ← tile Julia iterations
            ├── FrameAssemblerProcObj ← assemble tiles → frame
            └── FrameControllerProcObj ← dispatch + pace frame rate
```

```bash
~/.local/bin/bazel build //app:julia_mt_wasm
# outputs: bazel-bin/app/julia_mt_wasm/julia_mt.js + julia_mt.wasm
```

`wasm_mt_main.cpp` includes `app.hpp` and creates a `static App julia_app(cfg)` in `main()` (static so it survives `emscripten_set_main_loop`'s longjmp). `app.cpp` is compiled with `#ifndef __EMSCRIPTEN__` guards around `ImGuiLayer`, so the SDL2 backend is excluded and only the CoreEngine pipeline is built.

### Why two separate entry points?

Two constraints force the WASM app to be architecturally different from the native one.

#### 1. No multi-threading on GitHub Pages

`CoreEngine` runs a pool of `std::thread` workers.  Emscripten supports `std::thread` only when compiled with `-sPTHREAD`, which activates `SharedArrayBuffer`.

`SharedArrayBuffer` was disabled in all major browsers in January 2018 after the disclosure of the **Spectre and Meltdown CPU vulnerabilities**.  Those attacks abuse speculative execution and cache timing to leak arbitrary memory; a shared memory region between threads acts as an implicit high-resolution timer and makes the attack easier, so browsers removed it as a mitigation.  It was re-enabled in 2020 only on pages that prove **cross-origin isolation** via two HTTP headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

**GitHub Pages does not support custom response headers**, so those headers cannot be sent, `SharedArrayBuffer` is unavailable, `-sPTHREAD` cannot be used, and `CoreEngine` is unavailable in the deployed WASM binary.

#### 2. Emscripten port system incompatible with Bazel

The native ImGui backend (`imgui_impl_sdl2`) depends on SDL2.  In Emscripten the normal way to include SDL2 is via the port system (`-sUSE_SDL=2`), which downloads and compiles SDL2 into the Emscripten cache at build time.

The emsdk Bazel integration sets `FROZEN_CACHE=1` and points Emscripten at a pre-built, read-only cache snapshot.  When `-sUSE_SDL=2` attempts to add SDL2 to that cache it fails:

```
Exception: FROZEN_CACHE is set, but cache file is missing: "sysroot_install.stamp"
```

Redirecting `EM_CACHE` to a writable path breaks Emscripten's sysroot lookup for the same reason.  The SDL2 port cannot be used inside a Bazel sandbox at all.

#### Consequence

**Single-threaded** (`wasm_main.cpp`) avoids both issues by:

- Computing the Julia set **synchronously on the main thread** each frame — no `CoreEngine`, no worker threads.
- Using the **Emscripten HTML5 API** (`emscripten/html5.h`) directly for WebGL2 context and input — no SDL2 port.
- Using only the **ImGui OpenGL3 renderer backend** (`imgui_opengl3`) — no platform backend library.

**Multi-threaded** (`wasm_mt_main.cpp`) uses the same HTML5 platform layer but replaces the synchronous compute with `App + CoreEngine`:

- `-sPTHREAD` / `-pthread` enable `std::thread` via Web Workers (requires SharedArrayBuffer).
- `-sPTHREAD_POOL_SIZE=8` pre-allocates 8 Web Workers so thread creation in `CoreEngine::start()` does not block the main thread.
- `App::run()` is a no-op under `__EMSCRIPTEN__` (guarded in `app.cpp`); the render loop is driven by `emscripten_set_main_loop` instead.
- The `App` is `static` inside `main()` so it lives beyond the longjmp; the engine is never stopped (no `teardown()`) since WASM programs run until the tab is closed.

### WASM event handling

Without SDL2, the platform glue uses Emscripten HTML5 callbacks:

| Callback | Emscripten API | Purpose |
|---|---|---|
| mouse move | `emscripten_set_mousemove_callback` | track cursor position for ImGui and fractal interaction |
| mouse down/up | `emscripten_set_mousedown/up_callback` | update button state; fire `AddMouseButtonEvent` to ImGui |
| mouse wheel | `emscripten_set_wheel_callback` | accumulate `wheel_dy` for zoom and ImGui scroll |
| key down/up | `emscripten_set_keydown/up_callback` | `ImGuiIO::AddKeyEvent` + `AddInputCharacter` |

`imgui_new_frame()` reads the accumulated state and calls the ImGui IO methods before each `ImGui::NewFrame()`.  Mouse wheel events consumed by `handle_fractal_input()` (cursor over fractal) are zeroed before reaching ImGui so the two systems do not double-count them.

### WASM mouse interaction

`handle_fractal_input(State& s, float bx, float by)` runs at the top of every `render_frame()`, before ImGui:

- **Pan**: on left-button press while the cursor is inside the fractal area, the complex point under the cursor is recorded as an anchor.  While the button is held, `center_x/y` is updated each frame so the anchor tracks the cursor.
- **Zoom**: when `wheel_dy ≠ 0` and the cursor is inside the fractal area, the complex point under the cursor is computed, zoom is multiplied by `exp(wheel_dy × 0.2)`, and `center_x/y` is recalculated so that point stays fixed.

CSS scale (`emscripten_get_element_css_size` vs `emscripten_get_canvas_element_size`) is computed once per frame and applied to CSS mouse coordinates before passing them to `handle_fractal_input`, ensuring correct mapping regardless of browser window size.

### WASM main loop

`emscripten_set_main_loop(render_frame, 0, 1)` installs `render_frame` as the browser's `requestAnimationFrame` callback. The third argument (`simulate_infinite_loop = 1`) longjmps back to the browser event loop; `main()` never returns.  All mutable state lives in the global `g_state` struct so it remains valid after the stack unwind.

---

## Bazel targets

| Target | Platform | Tags | Description |
|---|---|---|---|
| `//app:julia_app` | native | — | ImGui + SDL2 + OpenGL3 + CoreEngine pipeline |
| `//app:julia_st` | WASM (cc\_binary) | `manual` | single-threaded, no SDL2, no threads; built via `julia_st_wasm` only |
| `//app:julia_st_wasm` | WASM (wasm\_cc\_binary) | — | wraps `julia_st`; outputs `julia_st.js` + `julia_st.wasm` |
| `//app:julia_mt` | WASM (cc\_binary) | `manual` | multi-threaded, App + CoreEngine, `-sPTHREAD`; built via `julia_mt_wasm` only |
| `//app:julia_mt_wasm` | WASM (wasm\_cc\_binary) | — | wraps `julia_mt`; outputs `julia_mt.js` + `julia_mt.wasm` |

`julia_st` and `julia_mt` are tagged `manual` so that `bazel build //...` and `bazel test //...` do not attempt to compile them with the host GCC toolchain (which has no `emscripten.h`). They are still built correctly as explicit dependencies of their `wasm_cc_binary` wrappers.
