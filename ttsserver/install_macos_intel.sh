#!/usr/bin/env bash
set -euo pipefail

ENV_NAME="${1:-chattts}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if ! command -v conda >/dev/null 2>&1; then
  echo "ERROR: conda not found. Please install Miniconda/Anaconda first." >&2
  exit 1
fi

if ! conda env list | awk '{print $1}' | grep -qx "$ENV_NAME"; then
  echo "[1/4] Creating conda env '$ENV_NAME' with Python 3.11..."
  conda create -n "$ENV_NAME" python=3.11 -y
else
  echo "[1/4] Conda env '$ENV_NAME' already exists."
fi

echo "[2/4] Verifying interpreter and architecture..."
conda run -n "$ENV_NAME" python - <<'PY'
import platform
import sys

print("python     :", sys.executable)
print("version    :", sys.version.replace("\n", " "))
print("machine    :", platform.machine())

if sys.version_info[:2] != (3, 11):
    raise SystemExit("ERROR: ttsserver on this Intel Mac is pinned to Python 3.11")
if platform.machine() != "x86_64":
    raise SystemExit("ERROR: this installer is specifically for Intel macOS (x86_64)")
PY

echo "[3/4] Upgrading pip tooling inside '$ENV_NAME'..."
conda run -n "$ENV_NAME" python -m pip install --upgrade pip setuptools wheel

echo "[4/4] Installing ttsserver dependencies from the conda interpreter..."
conda run -n "$ENV_NAME" python -m pip install -r "$SCRIPT_DIR/requirements.txt"

echo
echo "Installation complete. Verify with:"
echo "  conda run -n $ENV_NAME python -c 'import torch, ChatTTS; print(torch.__version__)'"
echo
echo "Start server with:"
echo "  cd $SCRIPT_DIR"
echo "  conda run -n $ENV_NAME python -m uvicorn app:app --host 0.0.0.0 --port 8000"
