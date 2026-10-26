#!/usr/bin/env bash
set -euo pipefail

ENV_NAME="${1:-chattts}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if ! command -v conda >/dev/null 2>&1; then
  echo "ERROR: conda not found. Please install Miniconda/Anaconda first." >&2
  exit 1
fi

if ! conda env list | awk '{print $1}' | grep -qx "$ENV_NAME"; then
  echo "[1/6] Creating conda env '$ENV_NAME' with Python 3.11..."
  conda create -n "$ENV_NAME" python=3.11 -y
else
  echo "[1/6] Conda env '$ENV_NAME' already exists."
fi

echo "[2/6] Verifying interpreter and architecture..."
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

echo "[3/6] Upgrading pip tooling inside '$ENV_NAME'..."
conda run -n "$ENV_NAME" python -m pip install --upgrade pip setuptools wheel

echo "[4/6] Installing binary-only Intel macOS compatibility stack..."
# 这些包在 Intel macOS + Python 3.11 上都有现成 wheel。
# 强制 binary-only，避免 pip 静默退回 numba/llvmlite 源码编译并要求本机 LLVM/C++ SDK。
conda run -n "$ENV_NAME" python -m pip install \
  --only-binary=:all: \
  "numpy==1.26.4" \
  "llvmlite==0.42.0" \
  "numba==0.59.1" \
  "torch==2.2.2" \
  "torchaudio==2.2.2"

echo "[5/6] Pinning Hugging Face stack compatible with ChatTTS 0.2.5 + torch 2.2.2..."
# ChatTTS 0.2.5 only declares transformers>=4.41.1, so pip may otherwise install Transformers 5.x.
# Transformers 5.x removed/changed the generation exports ChatTTS imports and may require newer torch.
conda run -n "$ENV_NAME" python -m pip install \
  "transformers==4.41.2" \
  "tokenizers==0.19.1" \
  "huggingface-hub>=0.23.0,<1.0"

echo "[6/6] Installing ChatTTS server dependencies..."
conda run -n "$ENV_NAME" python -m pip install -r "$SCRIPT_DIR/requirements.txt"

echo
echo "Verifying the complete stack..."
conda run -n "$ENV_NAME" python - <<'PY'
import platform
import sys
import torch
import numba
import llvmlite
import transformers
import tokenizers

print("python       :", sys.version.split()[0])
print("machine      :", platform.machine())
print("torch        :", torch.__version__)
print("numba        :", numba.__version__)
print("llvmlite     :", llvmlite.__version__)
print("transformers :", transformers.__version__)
print("tokenizers   :", tokenizers.__version__)

from transformers.generation import TopKLogitsWarper, TopPLogitsWarper
print("generation API: OK")

import ChatTTS
print("ChatTTS import : OK")
PY

echo
echo "Installation complete."
echo "Start server with:"
echo "  cd $SCRIPT_DIR"
echo "  conda run -n $ENV_NAME python -m uvicorn app:app --host 0.0.0.0 --port 8000"
