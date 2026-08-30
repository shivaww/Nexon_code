#!/usr/bin/env bash
# nexon_code installer — one copy-paste from GitHub to a working tool.
# Usage:  curl -fsSL https://raw.githubusercontent.com/shivaww/Nexon_code/main/install.sh | bash
set -euo pipefail

REPO_RAW="https://raw.githubusercontent.com/shivaww/Nexon_code/main"
REPO="https://github.com/shivaww/Nexon_code"
INSTALL_DIR="$HOME/.nexon_code"

say(){ printf '\033[1;36m>>\033[0m %s\n' "$*"; }
warn(){ printf '\033[1;33m>>\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[ -n "${PREFIX:-}" ] && IS_TERMUX=1 || IS_TERMUX=0

say "nexon_code installer"

# ---- fetch helper --------------------------------------------------------
if command -v curl >/dev/null 2>&1; then
  fetch(){ curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
  fetch(){ wget -q "$1" -O "$2"; }
else
  die "This installer needs curl or wget. Install one and re-run."
fi

# ---- C++ compiler --------------------------------------------------------
CXX=""
for c in clang++ g++; do command -v "$c" >/dev/null 2>&1 && CXX="$c" && break; done
if [ -z "$CXX" ]; then
  say "No C++ compiler found — installing one..."
  if [ "$IS_TERMUX" = 1 ]; then
    pkg install -y clang || die "pkg install clang failed"
  elif command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update && sudo apt-get install -y g++ || die "apt install g++ failed"
  elif command -v dnf >/dev/null 2>&1; then
    sudo dnf install -y gcc-c++ || die "dnf install failed"
  elif command -v pacman >/dev/null 2>&1; then
    sudo pacman -S --noconfirm gcc || die "pacman install failed"
  else
    die "Install clang++ or g++ for your system, then re-run."
  fi
  for c in clang++ g++; do command -v "$c" >/dev/null 2>&1 && CXX="$c" && break; done
  [ -n "$CXX" ] || die "compiler install did not produce clang++/g++"
fi
say "Compiler: $CXX"

# ---- download ------------------------------------------------------------
mkdir -p "$INSTALL_DIR"
say "Downloading source and prompt from GitHub..."
fetch "$REPO_RAW/nexon_code.cpp" "$INSTALL_DIR/nexon_code.cpp" || die "download failed: nexon_code.cpp"
fetch "$REPO_RAW/prompt.md" "$INSTALL_DIR/prompt.md" || die "download failed: prompt.md"

# ---- compile -------------------------------------------------------------
say "Compiling (about a minute on a phone)..."
( cd "$INSTALL_DIR" && "$CXX" -std=c++17 -O2 -o nexon_code nexon_code.cpp ) || die "compilation failed"

# ---- install on PATH -----------------------------------------------------
if [ "$IS_TERMUX" = 1 ] && [ -d "$PREFIX/bin" ]; then
  BIN_DIR="$PREFIX/bin"
else
  BIN_DIR="$HOME/.local/bin"
  mkdir -p "$BIN_DIR"
  case ":$PATH:" in
    *":$BIN_DIR:"*) : ;;
    *) warn "Add $BIN_DIR to PATH:  export PATH=\"\$HOME/.local/bin:\$PATH\"" ;;
  esac
fi
mv -f "$INSTALL_DIR/nexon_code" "$BIN_DIR/nexon_code"
chmod +x "$BIN_DIR/nexon_code"
say "Installed: $BIN_DIR/nexon_code"

# ---- how to use ----------------------------------------------------------
echo ""
say "Done. Here is how to use it:"
echo ""
echo "  1. Open any AI chat (free tiers work). Attach this file as the"
echo "     system prompt / custom instructions:"
echo "         $INSTALL_DIR/prompt.md"
echo ""
echo "  2. In your terminal, launch it on a project folder:"
echo "         nexon_code /path/to/project"
echo "     It accepts up to 3 project folders at once."
echo ""
echo "  3. Your chat now emits JSON commands. Copy one, paste it into the"
echo "     nexon_code session, and paste the result back to the chat."
echo "     First command to try:"
echo '         {"t":"list","a":{"p":".","depth":2}}'
echo ""
echo "  4. Inside a session: /help lists every tool, /exit quits."
echo ""
echo "  Docs and source: $REPO"
echo ""
