#!/bin/sh
# Installs everything a port needs on a Mac (Apple silicon or Intel), into the port folder where possible.
#   sh setup_macos.sh <port-folder> [--no-ghidra] [--no-web]
# Safe to re-run: each step is skipped when already done.
#   Homebrew packages, only if missing: sdl2, uv, node (system-wide; nothing already installed is upgraded)
#   <port>/.venv                 Python with pefile, capstone, numpy, pillow, unicorn
#   <port>/third_party/wgpu      wgpu-native (WebGPU for the native build)
#   <port>/third_party/emsdk     Emscripten (the WebAssembly build)
#   <port>/third_party/jdk, ghidra_*_PUBLIC   Ghidra + its JDK (one-time analysis of the executable)
set -e
P=${1:?usage: setup_macos.sh <port-folder>}
P=$(cd "$P" && pwd)
shift
NO_GHIDRA=0; NO_WEB=0
for a in "$@"; do case "$a" in --no-ghidra) NO_GHIDRA=1 ;; --no-web) NO_WEB=1 ;; esac; done
TP="$P/third_party"; mkdir -p "$TP"
say() { printf '\n== %s\n' "$1"; }
ARCH=$(uname -m)   # arm64 or x86_64

WGPU_VERSION=${WGPU_VERSION:-v29.0.1.1}
GHIDRA_VERSION=${GHIDRA_VERSION:-12.1.4}
JDK_MAJOR=${JDK_MAJOR:-21}

say "Xcode command line tools"
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Installing the Xcode command line tools: a window will ask you to confirm. Re-run this script afterwards."
  xcode-select --install || true
  exit 1
fi
clang --version | head -1

say "Homebrew packages"
command -v brew >/dev/null || { echo "Homebrew is required: see https://brew.sh (one command), then re-run."; exit 1; }
# Install only what's missing, and never upgrade anything already there: a brew install normally also
# upgrades shared libraries, which can break the user's other tools (it once broke an existing Node).
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1
brew list --versions sdl2 >/dev/null 2>&1 || brew install sdl2
command -v uv >/dev/null || brew install uv
command -v node >/dev/null || brew install node   # any Node 18+ works (wrangler, the headless test build)

say "Python environment"
[ -x "$P/.venv/bin/python" ] || uv venv -q "$P/.venv"
uv pip install -q --python "$P/.venv/bin/python" pefile capstone numpy pillow unicorn
"$P/.venv/bin/python" -c "import pefile, capstone; print('pefile + capstone', capstone.__version__)"

say "wgpu-native $WGPU_VERSION"
if [ ! -f "$TP/wgpu/lib/libwgpu_native.a" ]; then
  W=$([ "$ARCH" = arm64 ] && echo aarch64 || echo x86_64)
  curl -fsSL -o "$TP/wgpu.zip" "https://github.com/gfx-rs/wgpu-native/releases/download/$WGPU_VERSION/wgpu-macos-$W-release.zip"
  mkdir -p "$TP/wgpu" && unzip -q -o "$TP/wgpu.zip" -d "$TP/wgpu" && rm "$TP/wgpu.zip"
fi
ls "$TP/wgpu/lib/libwgpu_native.a"

if [ "$NO_WEB" = 0 ]; then
  say "Emscripten"
  if [ ! -f "$TP/emsdk/emsdk_env.sh" ]; then
    git clone -q --depth 1 https://github.com/emscripten-core/emsdk.git "$TP/emsdk"
  fi
  if [ ! -d "$TP/emsdk/upstream/emscripten" ]; then
    (cd "$TP/emsdk" && ./emsdk install latest >/dev/null 2>&1 && ./emsdk activate latest >/dev/null 2>&1)
  fi
  (. "$TP/emsdk/emsdk_env.sh" >/dev/null 2>&1 && emcc --version | head -1)
fi

if [ "$NO_GHIDRA" = 0 ]; then
  say "JDK $JDK_MAJOR (for Ghidra)"
  if [ ! -x "$TP/jdk/Contents/Home/bin/java" ]; then
    J=$([ "$ARCH" = arm64 ] && echo aarch64 || echo x64)
    curl -fsSL -o "$TP/jdk.tgz" "https://api.adoptium.net/v3/binary/latest/$JDK_MAJOR/ga/mac/$J/jdk/hotspot/normal/eclipse"
    mkdir -p "$TP/jdk.tmp" && tar -xzf "$TP/jdk.tgz" -C "$TP/jdk.tmp" && rm "$TP/jdk.tgz"
    mv "$TP/jdk.tmp"/*/ "$TP/jdk" && rmdir "$TP/jdk.tmp"
  fi
  "$TP/jdk/Contents/Home/bin/java" -version 2>&1 | head -1

  say "Ghidra $GHIDRA_VERSION"
  if ! ls -d "$TP"/ghidra_*_PUBLIC >/dev/null 2>&1; then
    URL=$(curl -fsSL "https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/tags/Ghidra_${GHIDRA_VERSION}_build" 2>/dev/null \
      | python3 -c 'import sys,json; print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if a["name"].endswith(".zip")))' 2>/dev/null || true)
    if [ -z "$URL" ]; then   # that version is gone: take the newest release
      URL=$(curl -fsSL https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/latest \
        | python3 -c 'import sys,json; print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if a["name"].endswith(".zip")))')
    fi
    echo "downloading $URL"
    curl -fsSL -o "$TP/ghidra.zip" "$URL" && unzip -q "$TP/ghidra.zip" -d "$TP" && rm "$TP/ghidra.zip"
  fi
  G=$(ls -d "$TP"/ghidra_*_PUBLIC | tail -1)
  echo "$G"
  # Ghidra ships no Apple-silicon decompiler; analysis uses it, so build it from the bundled sources
  OS_DIR=$([ "$ARCH" = arm64 ] && echo mac_arm_64 || echo mac_x86_64)
  if [ ! -x "$G/Ghidra/Features/Decompiler/os/$OS_DIR/decompile" ]; then
    say "building Ghidra's decompiler for $OS_DIR (a few minutes)"
    (cd "$G/Ghidra/Features/Decompiler/src/decompile/cpp" &&
      for d in com_opt com_dbg ghi_opt ghi_dbg sla_opt sla_dbg; do mkdir -p $d; touch $d/depend; done &&
      make -j"$(sysctl -n hw.ncpu)" ghidra_opt sleigh_opt ARCH_TYPE="-arch $ARCH" CXX="clang++ -std=c++11" >/dev/null &&
      mkdir -p "../../../os/$OS_DIR" && cp ghidra_opt "../../../os/$OS_DIR/decompile" && cp sleigh_opt "../../../os/$OS_DIR/sleigh")
  fi
  ls "$G/Ghidra/Features/Decompiler/os/$OS_DIR/decompile"
fi

say "done"
echo "Everything is in $TP and $P/.venv (git-ignored)."
