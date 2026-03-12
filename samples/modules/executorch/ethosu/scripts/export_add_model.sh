#!/bin/bash
# Export an ExecuTorch "add" model delegated to Ethos-U85-256.
#
# Runs inside a Docker container (ubuntu:22.04 + Python 3.12) using
# ExecuTorch's own setup.sh to install all ARM backend dependencies
# (tosa-tools, Vela compiler, etc.).
#
# Usage:
#   ./export_add_model.sh [output_dir]
#
# The .pte file is written to output_dir (default: src/models/).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SAMPLE_DIR="$(dirname "$SCRIPT_DIR")"
ZEPHYR_BASE="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
ET_DIR="$(cd "$ZEPHYR_BASE/../optional/modules/lib/executorch" && pwd)"
OUTPUT_DIR="${1:-$SAMPLE_DIR/src/models}"

mkdir -p "$OUTPUT_DIR"

echo "=== ExecuTorch add model export for Ethos-U85-256 ==="
echo "ExecuTorch source: $ET_DIR"
echo "Output directory:  $OUTPUT_DIR"

docker run --rm \
  -v "$ET_DIR:/executorch:ro" \
  -v "$OUTPUT_DIR:/output" \
  -w /work \
  ubuntu:22.04 \
  bash -c '
set -eu

export DEBIAN_FRONTEND=noninteractive

echo "--- Installing system packages ---"
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  git cmake build-essential python3 python3-pip python3-venv \
  python3-dev wget ca-certificates >/dev/null 2>&1

echo "--- Creating venv ---"
python3 -m venv /work/venv
. /work/venv/bin/activate
pip install -q --upgrade pip

echo "--- Installing PyTorch (CPU) + ExecuTorch Python deps ---"
pip install -q --no-cache-dir torch --index-url https://download.pytorch.org/whl/cpu
pip install -q --no-cache-dir \
  flatbuffers pyyaml sympy tabulate typing-extensions numpy packaging \
  ruamel.yaml pandas lark hydra-core omegaconf mpmath torchao

echo "--- Copying ExecuTorch source ---"
cp -a /executorch /work/executorch
cd /work/executorch
git config --global --add safe.directory /work/executorch

echo "--- Running ExecuTorch ARM setup (installs tosa-tools, Vela, etc.) ---"
# setup.sh installs tosa-tools, Vela, and optionally the baremetal
# toolchain + FVPs.  We only need the Python tooling for model export,
# so patch the defaults to skip the large toolchain/FVP downloads.
sed -i "s/^enable_baremetal_toolchain=1/enable_baremetal_toolchain=0/" examples/arm/setup.sh
sed -i "s/^enable_fvps=1/enable_fvps=0/" examples/arm/setup.sh
bash examples/arm/setup.sh --i-agree-to-the-contained-eula

echo "--- Sourcing setup_path.sh ---"
source examples/arm/arm-scratch/setup_path.sh

echo "--- Building flatc from ExecuTorch third-party ---"
cmake -B /work/flatc-build \
  -DFLATBUFFERS_BUILD_TESTS=OFF \
  -DFLATBUFFERS_BUILD_FLATLIB=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  /work/executorch/third-party/flatbuffers
cmake --build /work/flatc-build --target flatc -j$(nproc)
export FLATC_EXECUTABLE=/work/flatc-build/flatc

echo "--- Setting up ExecuTorch Python path ---"
# Skip the heavy C++ build (pip install -e .) — we only need the pure
# Python parts for model export.  Add source tree to PYTHONPATH so
# "import executorch.backends.arm..." resolves.
mkdir -p /work/et_pkg/executorch
for d in backends codegen devtools exir export extension runtime kernels; do
  [ -d "/work/executorch/$d" ] && ln -sf "/work/executorch/$d" "/work/et_pkg/executorch/$d"
done
touch /work/et_pkg/executorch/__init__.py
export PYTHONPATH="/work/et_pkg:/work/executorch:${PYTHONPATH:-}"

echo "--- Exporting add model with Ethos-U85-256 delegation ---"
python -m examples.arm.aot_arm_compiler \
  -m add \
  -d \
  -q \
  -t ethos-u85-256 \
  --memory_mode Sram_Only \
  -o /output

echo "--- Done ---"
ls -la /output/*.pte 2>/dev/null || echo "WARNING: No .pte file found"
'

echo ""
echo "Output:"
ls -la "$OUTPUT_DIR"/*.pte 2>/dev/null || echo "ERROR: No .pte file produced"
