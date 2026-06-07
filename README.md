# various_wasm

A C++ project targeting both native and WebAssembly, using [Dear ImGui](https://github.com/ocornut/imgui) for portable UI and [Bazel](https://bazel.build) as the build system.

> This project is primarily written through [Claude](https://claude.ai) (Anthropic's AI assistant).

## Requirements

**Managed automatically by Bazel:**
- [Bazelisk](https://github.com/bazelbuild/bazelisk) — install to `~/.local/bin/bazel`
- [Emscripten SDK](https://emscripten.org) 6.0.0 — install to `~/emsdk`

**Requires manual install (native GL/X11 headers):**
```bash
sudo apt install libgl-dev libglu1-mesa-dev libxi-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev
```

## Build & Run

```bash
# Native binary
bazel build //src/hello:hello
./bazel-bin/src/hello/hello

# WebAssembly — assembles dist/ with index.html, hello.js, hello.wasm, style.css
bazel build //web:dist
python3 -m http.server 8080 --directory bazel-bin/web/dist
# then open http://localhost:8080 in a browser
```

## Project structure

```
various_wasm/
├── MODULE.bazel          # Bzlmod dependencies (rules_cc, emsdk)
├── .bazelrc              # Build flags
├── web/                  # Web frontend (HTML, CSS, JS)
│   ├── index.html        # Entry point, loads hello.js
│   ├── style.css
│   └── BUILD.bazel       # dist genrule: assembles WASM outputs + web assets
└── src/
    └── hello/            # Minimal hello-world, native + WASM
```
