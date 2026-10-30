#!/usr/bin/env bash
# Deploy the file server and start it.
#   bash deploy.sh                 # environment + start
#   bash deploy.sh --tts           # also fetch the GPT-SoVITS pieces
#   bash deploy.sh --tts --check   # deploy only, do not start the server
#   bash deploy.sh --start-only    # skip the deploy steps, just start
#   bash deploy.sh --port 8001     # listen on another port (default 8000)
#   bash deploy.sh --host 127.0.0.1  # bind another address (default 0.0.0.0)
#   bash deploy.sh --autoload      # warm the TTS model while the server boots
#   bash deploy.sh --mirror        # route github.com downloads through gh-proxy
#   bash deploy.sh --cpu           # force the CPU torch build
set -euo pipefail
cd "$(dirname "$0")"

tts=false
deploy_only=false
start_only=false
autoload=false
mirror=false
backend=""
port=""
host=""

usage() {
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tts) tts=true; shift ;;
        --check) deploy_only=true; shift ;;
        --start-only) start_only=true; shift ;;
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

# Step 1: environment. setup_env.sh picks the torch build that fits the GPU.
if [[ "$start_only" == false ]]; then
    echo "==> environment"
    env_args=()
    [[ -n "$backend" ]] && env_args+=("$backend")
    [[ "$tts" == true ]] && env_args+=(--tts)
    bash setup_env.sh "${env_args[@]+${env_args[@]}}"
fi

if [[ -x .venv/Scripts/python.exe ]]; then
    python=.venv/Scripts/python.exe
elif [[ -x .venv/bin/python ]]; then
    python=.venv/bin/python
else
    echo "no virtualenv here; run setup_env.sh first" >&2
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
