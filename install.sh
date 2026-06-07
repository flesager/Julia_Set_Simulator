#!/usr/bin/env bash
# install.sh — set up the host environment for building Julia Set Simulator
# Covers: native app (SDL2/OpenGL) + Bazelisk build tool
# The WASM toolchain (Emscripten 6.0.0) and ImGui are fetched automatically by Bazel.
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Set up the host environment for the Julia Set Simulator project.

What it installs:
  - System packages (apt): build-essential, git, curl, libsdl2-dev, libgl-dev, python3
  - Bazelisk v1.29.0 → ~/.local/bin/bazel  (manages the exact Bazel version)

What Bazel fetches automatically on first build:
  - Emscripten SDK 6.0.0  (WASM toolchain, via emsdk Bazel module)
  - Dear ImGui 1.91.9b

Options:
  -h, --help    Show this help message and exit

Build commands (after install):
  bazel run  //app:julia_app                                  Native desktop app
  bazel test //core/... //common/... --test_timeout=120       Unit tests
  bazel build //web:dist                                      WASM dist artifacts
  python3 -m http.server 8080 --directory bazel-bin/web/dist  Serve WASM locally
EOF
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg"; usage; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# 1. System packages (apt)
# ---------------------------------------------------------------------------

echo ">>> Installing system packages..."
sudo apt update
sudo apt install -y \
    build-essential \
    git \
    curl \
    libsdl2-dev \
    libgl-dev \
    python3

echo ">>> System packages installed."

# ---------------------------------------------------------------------------
# 2. Bazelisk  (manages the exact Bazel version for this project)
# ---------------------------------------------------------------------------

BAZEL_BIN="$HOME/.local/bin/bazel"

if command -v "$BAZEL_BIN" &>/dev/null; then
    echo ">>> Bazelisk already installed at $BAZEL_BIN"
else
    echo ">>> Installing Bazelisk..."
    BAZELISK_VERSION="v1.29.0"
    ARCH="$(uname -m)"
    case "$ARCH" in
        x86_64)  BAZELISK_ARCH="amd64" ;;
        aarch64) BAZELISK_ARCH="arm64" ;;
        *)        echo "Unsupported architecture: $ARCH"; exit 1 ;;
    esac
    mkdir -p "$HOME/.local/bin"
    curl -fsSL \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-${BAZELISK_ARCH}" \
        -o "$BAZEL_BIN"
    chmod +x "$BAZEL_BIN"
    echo ">>> Bazelisk installed at $BAZEL_BIN"
fi

# Ensure ~/.local/bin is in PATH for the current session
export PATH="$HOME/.local/bin:$PATH"

# ---------------------------------------------------------------------------
# 3. Verify
# ---------------------------------------------------------------------------

echo ""
echo ">>> Verifying installation..."
echo "  bazel   : $(bazel version 2>&1 | grep 'Build label' || echo 'ok (first run will download Bazel)')"
echo "  gcc     : $(gcc --version | head -1)"
echo "  python3 : $(python3 --version)"
echo ""
echo "All done. Build commands:"
echo "  Native app : bazel run  //app:julia_app"
echo "  Tests      : bazel test //core/... //common/... --test_timeout=120"
echo "  WASM dist  : bazel build //web:dist"
echo "  Serve WASM : python3 -m http.server 8080 --directory bazel-bin/web/dist"
