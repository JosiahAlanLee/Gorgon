#!/usr/bin/env bash
# Set up the filming environment on the workstation. Safe to re-run.
#   tutorial/bootstrap.sh            venv + Python packages + Chromium
# System packages are listed, not installed: run the apt line yourself (it needs sudo).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"
[ -f config.yaml ] || { cp config.example.yaml config.yaml; echo "wrote config.yaml: edit paths, then re-run"; exit 0; }

need=()
for exe in Xvfb xdotool xdpyinfo xsetroot openbox xterm ffmpeg jackd rsvg-convert tesseract espeak-ng fc-match git make; do
  command -v "$exe" >/dev/null || need+=("$exe")
done
if [ ${#need[@]} -gt 0 ]; then
  echo "missing: ${need[*]}"
  echo "sudo apt install xvfb xdotool x11-utils x11-xserver-utils openbox xterm ffmpeg jackd2 librsvg2-bin tesseract-ocr espeak-ng fonts-dejavu-core build-essential"
  echo "(jackd2 asks about realtime priority: either answer is fine, the stage runs --no-realtime)"
fi

work="$(python3 - <<'PY'
import yaml, os, pathlib
c = yaml.safe_load(open('config.yaml'))
w = (c.get('paths') or {}).get('work') or './work'
p = pathlib.Path(os.path.expanduser(w))
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
