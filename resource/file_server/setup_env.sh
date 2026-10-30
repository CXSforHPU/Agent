#!/usr/bin/env bash
# Select the torch build that matches this machine, then sync the uv environment.
#   ./setup_env.sh              # probe the GPU and install accordingly
#   ./setup_env.sh cpu          # force the CPU build
#   ./setup_env.sh cu130        # force Blackwell (CUDA 13.0) build
#   ./setup_env.sh --tts        # also install the GPT-SoVITS inference stack
#   ./setup_env.sh --print      # only show the uv command it would run
set -euo pipefail
cd "$(dirname "$0")"

backend="auto"
print_only=false
groups=()
for arg in "$@"; do
    case "$arg" in
        --print | -n) print_only=true ;;
        --tts) groups+=(tts) ;;
        auto | cpu | cu126 | cu130) backend="$arg" ;;
        *)
            echo "usage: $0 [auto|cpu|cu126|cu130] [--tts] [--print]" >&2
            exit 2
            ;;
    esac
done

command -v uv >/dev/null || { echo "uv is not on PATH: https://docs.astral.sh/uv/getting-started/installation/" >&2; exit 1; }

# uv cannot resolve hardware, so the choice happens here: compute capability decides
# which dependency group is synced. cu126 wheels stop at sm_90, Blackwell needs cu130.
compute_cap() {
    if command -v nvidia-smi >/dev/null 2>&1; then
        local caps
        caps=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null |
            tr -d ' "' | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -V | tail -1 || true)
        # Hybrid GPUs list several entries; the fastest one sets the build.
        [[ -n "$caps" ]] && { echo "$caps"; return; }
    fi
    if command -v lspci >/dev/null 2>&1 && lspci 2>/dev/null | grep -qi 'nvidia'; then
        # Old drivers omit compute_cap; assume anything pre-Blackwell, which cu126 covers.
        if lspci 2>/dev/null | grep -qiE 'rtx 50|b[0-9]00|blackwell'; then echo 10.0; else echo 7.5; fi
        return
    fi
    echo ""
}

if [[ "$backend" == "auto" ]]; then
    cap=$(compute_cap)
    if [[ -z "$cap" ]]; then
        backend="cpu"
        echo "No NVIDIA GPU detected -> CPU build"
    elif awk "BEGIN{exit !($cap >= 10.0)}"; then
        backend="cu130"
        echo "GPU compute capability $cap -> CUDA 13.0 build (Blackwell)"
    else
        backend="cu126"
        echo "GPU compute capability $cap -> CUDA 12.6 build"
    fi
fi

if [[ "$backend" == "cpu" ]]; then
    # cpu is the default group, so a bare sync selects it.
    argv=(sync)
    group=cpu
else
    group="gpu-$backend"
    # The GPU groups conflict with cpu, so the default group must be turned off first.
    argv=(sync --no-default-groups --group "$group")
fi
# tts is orthogonal to the backend groups and simply rides along.
for g in "${groups[@]+${groups[@]}}"; do
    argv+=(--group "$g")
    group="$group+$g"
done

echo "uv ${argv[*]}"
if [[ "$print_only" == true ]]; then exit 0; fi
uv "${argv[@]}"

# Git Bash on Windows puts the project environment under Scripts/.
if [[ -x .venv/Scripts/python.exe ]]; then
    python=.venv/Scripts/python.exe
else
    python=.venv/bin/python
fi

"$python" - <<PY
import torch
print("torch", torch.__version__, "| cuda available:", torch.cuda.is_available())
if "$backend" != "cpu" and not torch.cuda.is_available():
    print("WARNING: a GPU build was installed but CUDA is unavailable (driver missing or too old)")
PY
