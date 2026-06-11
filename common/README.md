# common — Julia set pipeline

`common/` contains the three `ProcObj` types that form the Julia set render pipeline, plus the shared message types that flow between them.  All three modules sit on top of `core/engine` and rely on `std::thread` indirectly through `CoreEngine`.

They are used by **two of the three build targets**:
- **Native** (`julia_app`) — always.
- **WASM MT** (`julia_mt`) — when the page is cross-origin isolated; compiled with `-sPTHREAD` so `std::thread` maps to Web Workers.
- **WASM ST** (`julia_st`) — **not used**; that binary computes the Julia set synchronously without any pipeline.

See [Threading constraints](#threading-constraints) and [app/README.md](../app/README.md) for the full picture.

---

## Pipeline overview

```
FrameControllerProcObj
        │  TileWorkMsg ×N  (one per tile)
        ▼  (round-robin across M compute procs)
ComputeProcObj ×M ──► ComputeProcObj ──► ComputeProcObj
        │  TileResultMsg (pixels for one tile)
        ▼
FrameAssemblerProcObj
        │  FrameDoneMsg (when all N tiles assembled)
        ▼
FrameControllerProcObj   ← back to the controller to start the next frame
```

One *frame* equals one Julia set image at the current parameters.  The controller splits the image into fixed-size tiles and sends each tile to one compute proc.  When the assembler has received all tiles it signals the controller, which paces the frame rate and dispatches the next frame.

---

## Message types (`common/inc/messages.hpp`)

| Message | Sender | Receiver | Payload |
|---|---|---|---|
| `TileWorkMsg` | controller | compute | tile bounds, image size, Julia *c*, zoom, center, max\_iter |
| `TileResultMsg` | compute | assembler | tile bounds + `pixels[]` (RGBA, row-major) |
| `FrameDoneMsg` | assembler | controller | frame\_id, dispatch\_time (for latency measurement) |

All messages carry `dispatch_time` (a `std::chrono::steady_clock::time_point` set when the controller begins a frame).  The assembler echoes it into `FrameDoneMsg` so the controller can measure end-to-end frame latency.

---

## ComputeProcObj (`common/compute/`)

Receives `TileWorkMsg`, iterates the Julia function for every pixel in the tile, and posts a `TileResultMsg` to the assembler.

### Julia iteration

```
z₀ = (pixel position mapped to complex plane)
zₙ₊₁ = zₙ² + c     until |zₙ|² > 4  or  n == max_iter
```

The escape count is converted to a colour using a Bernstein polynomial palette.  Pixels that never escape are black.

### Pixel format

`iter_to_rgba()` stores the colour as `0xAA_bb_gg_rr` so that the bytes in memory read `[R, G, B, A]` — the layout expected by `GL_RGBA` + `GL_UNSIGNED_BYTE`.

---

## FrameAssemblerProcObj (`common/assembler/`)

Maintains two framebuffers (back and front, each `cache_width × cache_height` pixels, where cache dimensions are `img_width × cache_margin`).

On each `TileResultMsg`:
1. Copies tile pixels into the correct rows of the **back buffer**.
2. Increments a tile counter.
3. When the counter reaches `tiles_per_frame`: swaps back ↔ front under a mutex, resets the counter, and posts `FrameDoneMsg` to the controller.

`get_latest_frame()` returns a snapshot of the front buffer under the same mutex, safe to call from any thread — the native ImGui render thread or the WASM MT `render_frame()` callback.

---

## FrameControllerProcObj (`common/controller/`)

Manages the frame loop:

```
start()
  └─► dispatch_frame()
            │  stores pending center/zoom, posts TileWorkMsg for every tile
            │  (round-robin across compute procs)
            │
  ◄── FrameDoneMsg arrives in process_msg()
            ├─ update rolling average of last frame time (30-frame window)
            ├─ promote pending center/zoom to cache atomics (now in sync with pixels)
            ├─ throttle sleep (if frame was faster than target_fps)
            └─ dispatch_frame() or go idle
```

### Cache and idle mechanism

The controller maintains an oversized cache (1.5× per dimension by default). After a frame completes, it checks whether the current viewport is still within the cached region:

- **Cache hit** (view within bounds, zoom within margin): mark idle, skip recompute.
- **Cache miss** (view out of bounds or zoomed in past the margin): dispatch a new frame centred on the current view.

When a UI setter (`set_zoom`, `set_center`, etc.) changes a parameter while the controller is idle, it calls `wake_if_idle()` to re-enter `dispatch_frame()`.

### Thread-safe setters

The controller exposes atomic setters callable from the UI thread:

| Setter | Behaviour |
|---|---|
| `set_julia_c(r, i)` | stores atomics, calls `wake_if_idle()` |
| `set_zoom(z)` | stores atomic, calls `wake_if_idle()` |
| `set_center(cx, cy)` | stores atomics, calls `wake_if_idle()` |
| `set_view(zoom, cx, cy)` | stores all three atomics **then** calls `wake_if_idle()` once — ensures `dispatch_frame()` reads a consistent (zoom, center) snapshot |
| `set_max_iter(n)` | stores atomic, calls `wake_if_idle()` |
| `set_target_fps(fps)` | stores atomic (no wake needed — affects throttle only) |

`set_view()` is the preferred path for simultaneous zoom+center updates (e.g. wheel zoom toward cursor) to avoid dispatching a frame with the new zoom but the old center.

### Cache info synchronisation

`get_cache_info()` returns `(cx, cy, zoom, width, height, valid)` describing what is **currently in the texture**. These atomics are updated only inside `process_msg()` after `FrameDoneMsg` — i.e. after the assembler has swapped the new pixels into the front buffer — never at dispatch time. This keeps the UV sub-rectangle computed by the display layer always consistent with the pixels it is applied to.

### Pause / resume (internal API)

`pause()` sets an atomic flag; the next `FrameDoneMsg` handler skips `dispatch_frame()`.  `resume()` clears the flag and calls `dispatch_frame(force=true)` directly from the calling thread to re-enter the pipeline immediately.  These methods are not currently exposed in any UI.

---

## Tests

Each module has its own test suite in its `tst/` subdirectory.  A shared helper library lives in `common/tst/`:

| Target | Tests |
|---|---|
| `//common/compute/tst:compute_proc_obj_test` | metadata echo, pixel count, opaque alpha, dispatch\_time round-trip, multi-tile dispatch |
| `//common/assembler/tst:frame_assembler_test` | no FrameDoneMsg before all tiles, done after all tiles, frame\_id echo, pixel placement, front-buffer swap |
| `//common/controller/tst:frame_controller_test` | tile count, full-image coverage, FrameDone triggers next dispatch, pause, resume, set\_julia\_c reflected, round-robin distribution |

```bash
bazel test //common/...
```

---

## Threading constraints

`common/` modules use `core::engine::ProcObj` which relies on `CoreEngine`'s `std::thread` pool.  Emscripten supports threads only with `-sPTHREAD` + `SharedArrayBuffer`.  `SharedArrayBuffer` was disabled in browsers in 2018 following the **Spectre and Meltdown CPU vulnerabilities** (shared memory acts as a high-resolution timer that makes cache-timing attacks easier) and was only re-enabled for cross-origin isolated pages — requiring `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` response headers that GitHub Pages cannot send.

The WASM build ships **two binaries** selected by a runtime `crossOriginIsolated` check in `index.html`:

- **`julia_st`** (single-threaded fallback) — does **not** use `common/` at all.  It computes the Julia set synchronously inside `render_frame()` in `app/wasm_main.cpp`.  Loads on GitHub Pages and any server without COOP+COEP headers.
- **`julia_mt`** (multi-threaded, requires COOP+COEP) — **does** use `common/` and `core/engine`.  Compiled with `-sPTHREAD`; `std::thread` maps to Web Workers, which requires `SharedArrayBuffer`.  Only loads when `window.crossOriginIsolated === true`.

See [app/README.md](../app/README.md) for the full architecture description of both binaries.
