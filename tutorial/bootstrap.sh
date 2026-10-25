#!/usr/bin/env bash
# Set up the filming environment on the workstation. Safe to re-run.
#   tutorial/bootstrap.sh            venv + Python packages + Chromium
# System packages are listed, not installed: run the apt line yourself (it needs sudo).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"
[ -f config.yaml ] || { cp config.example.yaml config.yaml; echo "wrote config.yaml: edit paths, then re-run"; exit 0; }

# Only what is missing: suggesting jackd2 where jackd1 works would replace it.
declare -A pkg=([Xvfb]=xvfb [xdotool]=xdotool [xdpyinfo]=x11-utils [xsetroot]=x11-xserver-utils [openbox]=openbox
  [xterm]=xterm [ffmpeg]=ffmpeg [jackd]=jackd2 [rsvg-convert]=librsvg2-bin [tesseract]=tesseract-ocr
  [espeak-ng]=espeak-ng [fc-match]=fontconfig [git]=git [make]=build-essential)
need=()
for exe in "${!pkg[@]}"; do
  command -v "$exe" >/dev/null || need+=("${pkg[$exe]}")
done
if [ ${#need[@]} -gt 0 ]; then
  echo "missing: sudo apt install ${need[*]}"
  if [[ " ${need[*]} " == *" jackd2 "* ]]; then
    echo "(jackd2 asks about realtime priority: either answer is fine, the stage runs --no-realtime)"
  fi
fi

# paths.work, with PyYAML if this python3 has it, otherwise read from the file directly.
work="$(python3 - <<'PY'
import os, pathlib, re
text = open('config.yaml').read()
try:
    import yaml
    w = ((yaml.safe_load(text) or {}).get('paths') or {}).get('work')
except ImportError:
    m = re.search(r'^paths:\n(?:[ \t]+.*\n)*?[ \t]+work:[ \t]*([^#\n]+)', text, re.M)
    w = m.group(1).strip().strip('"\'') if m else None
p = pathlib.Path(os.path.expanduser(w or './work'))
print((p if p.is_absolute() else pathlib.Path('.').resolve() / p).resolve())
PY
)"
mkdir -p "$work"
if [ ! -x "$work/venv/bin/python" ]; then
  python3 -m venv "$work/venv"
fi
"$work/venv/bin/pip" install -q --upgrade pip
"$work/venv/bin/pip" install -q -e ".[narration,test]"
"$work/venv/bin/python" -m playwright install chromium
echo
echo "venv: $work/venv   (source $work/venv/bin/activate)"
echo "next: gtut setup && gtut catalog && gtut preflight"
