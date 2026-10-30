#!/usr/bin/env bash
# Deploy the file server and start it.
#   bash deploy.sh                   # environment + start
#   bash deploy.sh --tts             # also fetch the GPT-SoVITS pieces
#   bash deploy.sh --tts --check     # deploy only, do not start the server
#   bash deploy.sh --start-only      # skip the deploy steps, just start
#   bash deploy.sh --print           # only show the uv command, change nothing
#   bash deploy.sh --port 8001       # listen on another port (default 8000)
#   bash deploy.sh --host 127.0.0.1  # bind another address (default 0.0.0.0)
#   bash deploy.sh --autoload        # warm the TTS model while the server boots
#   bash deploy.sh --mirror          # route github.com downloads through gh-proxy
#   bash deploy.sh --cpu             # force the CPU torch build
set -euo pipefail
cd "$(dirname "$0")"

tts=false
deploy_only=false
start_only=false
print_only=false
autoload=false
mirror=false
backend=""
port=""
host=""

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tts) tts=true; shift ;;
        --check) deploy_only=true; shift ;;
        --start-only) start_only=true; shift ;;
        --print) print_only=true; shift ;;
        --autoload) autoload=true; shift ;;
        --mirror) mirror=true; shift ;;
        --cpu | --cu126 | --cu130) backend="${1#--}"; shift ;;
        --port) port="${2:-}"; [[ -n "$port" ]] || { echo "--port needs a value" >&2; exit 2; }; shift 2 ;;
        --host) host="${2:-}"; [[ -n "$host" ]] || { echo "--host needs a value" >&2; exit 2; }; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$start_only" == true && "$deploy_only" == true ]]; then
    echo "--start-only and --check contradict each other" >&2
    exit 2
fi

# uv cannot resolve hardware, so the choice happens here: compute capability
# decides which dependency group is synced. cu126 wheels stop at sm_90,
# Blackwell needs cu130.
compute_cap() {
    if command -v nvidia-smi >/dev/null 2>&1; then
        local caps
        caps=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null |
            tr -d ' "' | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -V | tail -1 || true)
        # Hybrid GPUs list several entries; the fastest one sets the build.
        [[ -n "$caps" ]] && { echo "$caps"; return; }
    fi
    if command -v lspci 2>/dev/null && lspci 2>/dev/null | grep -qi 'nvidia'; then
        # Old drivers omit compute_cap; assume anything pre-Blackwell, which cu126 covers.
        if lspci 2>/dev/null | grep -qiE 'rtx 50|b[0-9]00|blackwell'; then echo 10.0; else echo 7.5; fi
        return
    fi
    echo ""
}

# Step 1: environment. Build the venv and report what torch ended up in it.
sync_env() {
    command -v uv >/dev/null || { echo "uv is not on PATH: https://docs.astral.sh/uv/getting-started/installation/" >&2; exit 1; }

    if [[ "$backend" == "" ]]; then
        local cap
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

    local argv group
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
    if [[ "$tts" == true ]]; then
        argv+=(--group tts)
        group="$group+tts"
    fi

    echo "uv ${argv[*]}"
    [[ "$print_only" == true ]] && return 0
    uv "${argv[@]}"

    # Git Bash on Windows puts the project environment under Scripts/.
    local python
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
}

if [[ "$print_only" == true ]]; then
    sync_env
    exit 0
fi

if [[ "$start_only" == false ]]; then
    echo "==> environment"
    sync_env
fi

if [[ -x .venv/Scripts/python.exe ]]; then
    python=.venv/Scripts/python.exe
elif [[ -x .venv/bin/python ]]; then
    python=.venv/bin/python
else
    echo "no virtualenv here; run deploy.sh without --start-only" >&2
    exit 1
fi

# Step 2: the blobs that stay out of git. Idempotent, so re-running is cheap.
if [[ "$tts" == true && "$start_only" == false ]]; then
    echo "==> third-party pieces"
    fetch_args=()
    [[ "$mirror" == true ]] && fetch_args+=(--mirror)
    "$python" fetch_tts.py dicts voice assets "${fetch_args[@]+${fetch_args[@]}}"
    "$python" fetch_tts.py check

    # uv sync prunes whatever the lock does not mention. g2pk2's Korean path needs
    # eunjeon, and eunjeon imports pkg_resources, which setuptools 84 dropped.
    # The directory can survive the prune as an empty namespace package, so ask the
    # metadata, not the import machinery.
    if ! "$python" -c "import importlib.metadata as m; m.version('eunjeon')" >/dev/null 2>&1; then
        echo "==> restoring the Korean extras (eunjeon, setuptools<81)"
        uv pip install --python "$python" eunjeon "setuptools<81"
    fi
fi

if [[ "$deploy_only" == true ]]; then
    echo "==> deploy done, server not started"
    exit 0
fi

# Step 3: start the server.
[[ -n "$port" ]] && export PORT="$port"
[[ -n "$host" ]] && export HOST="$host"
[[ "$autoload" == true ]] && export TTS_AUTOLOAD=1

echo "==> starting on ${HOST:-0.0.0.0}:${PORT:-8000}"
exec "$python" main.py
