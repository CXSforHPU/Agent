#!/usr/bin/env bash
# Deploy the file server and the MCP gateway, then start both.
#   bash deploy.sh                   # environment + start
#   bash deploy.sh --start LIST      # start only these components
#   bash deploy.sh --install LIST    # install only these components' dependencies
#   bash deploy.sh --status          # show each component's dependency status
#   bash deploy.sh --tts             # also fetch the GPT-SoVITS pieces
#   bash deploy.sh --tts --check     # deploy only, do not start the server
#   bash deploy.sh --start-only      # skip the deploy steps, just start
#   bash deploy.sh --print           # only show the uv command, change nothing
#   bash deploy.sh --port 8001       # listen on another port (default 8000)
#   bash deploy.sh --host 127.0.0.1  # bind another address (default 0.0.0.0)
#   bash deploy.sh --autoload        # warm the TTS model while the server boots
#   bash deploy.sh --speaker NAME    # default voice for /tts requests
#   bash deploy.sh --mirror          # route github.com downloads through gh-proxy
#   bash deploy.sh --cpu             # force the CPU torch build
#   bash deploy.sh --no-mcp          # do not start the MCP gateway
#   bash deploy.sh --mcp-port 8765   # MCP gateway port (default 8765)
#   bash deploy.sh --json            # --status prints JSON instead of text
#
# Components: file-server, mcp, tts, stt
#   --start and --install take a comma separated list of those names.
set -euo pipefail
cd "$(dirname "$0")"

ALL_COMPONENTS="file-server,mcp,tts,stt"

tts=false
deploy_only=false
start_only=false
print_only=false
autoload=false
status_only=false
json_out=false
speaker=""
mirror=false
backend=""
port=""
host=""
mcp=true
mcp_port=8765
start_list=""
install_list=""

usage() {
    sed -n '/^#   bash/,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tts) tts=true; shift ;;
        --check) deploy_only=true; shift ;;
        --start-only) start_only=true; shift ;;
        --print) print_only=true; shift ;;
        --autoload) autoload=true; shift ;;
        --status) status_only=true; shift ;;
        --json) json_out=true; shift ;;
        --start) start_list="${2:-}"; [[ -n "$start_list" ]] || { echo "--start needs a value" >&2; exit 2; }; shift 2 ;;
        --install) install_list="${2:-}"; [[ -n "$install_list" ]] || { echo "--install needs a value" >&2; exit 2; }; shift 2 ;;
        --speaker) speaker="${2:-}"; [[ -n "$speaker" ]] || { echo "--speaker needs a value" >&2; exit 2; }; shift 2 ;;
        --mirror) mirror=true; shift ;;
        --cpu | --cu126 | --cu130) backend="${1#--}"; shift ;;
        --no-mcp) mcp=false; shift ;;
        --mcp-port) mcp_port="${2:-}"; [[ -n "$mcp_port" ]] || { echo "--mcp-port needs a value" >&2; exit 2; }; shift 2 ;;
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

has_component() {
    [[ ",$1," == *",$2,"* ]]
}

drop_component() {
    local list="$1" name="$2" out="" item
    local IFS=','
    for item in $list; do
        if [[ -z "$item" ]]; then
            continue
        fi
        if [[ "$item" == "$name" ]]; then
            continue
        fi
        if [[ -z "$out" ]]; then out="$item"; else out="$out,$item"; fi
    done
    printf '%s' "$out"
}

validate_components() {
    local list="$1" flag="$2" comp
    local IFS=','
    for comp in $list; do
        case "$comp" in
            file-server | mcp | tts | stt) ;;
            *) echo "$flag: unknown component '$comp' (file-server, mcp, tts, stt)" >&2; exit 2 ;;
        esac
    done
}

# What to install: --install wins, --tts keeps the old meaning, otherwise just
# the file server itself.
if [[ -n "$install_list" ]]; then
    install_components="$install_list"
elif [[ "$tts" == true ]]; then
    install_components="file-server,tts"
else
    install_components="file-server"
fi
validate_components "$install_components" "--install"

# What to start: --start wins, otherwise everything. --no-mcp drops the gateway.
if [[ -n "$start_list" ]]; then
    start_components="$start_list"
else
    start_components="$ALL_COMPONENTS"
fi
validate_components "$start_components" "--start"
if [[ "$mcp" == false ]]; then
    start_components="$(drop_component "$start_components" mcp)"
fi

# The file server is the ground the other components stand on, it is always in.
if ! has_component "$install_components" file-server; then
    install_components="file-server,$install_components"
fi
if ! has_component "$start_components" file-server; then
    start_components="file-server,$start_components"
fi

install_tts=false
install_stt=false
install_mcp=false
if has_component "$install_components" tts; then install_tts=true; fi
if has_component "$install_components" stt; then install_stt=true; fi
if has_component "$install_components" mcp; then install_mcp=true; fi

start_tts=false
start_stt=false
start_mcp=false
if has_component "$start_components" tts; then start_tts=true; fi
if has_component "$start_components" stt; then start_stt=true; fi
if has_component "$start_components" mcp; then start_mcp=true; fi

# The probe caches its findings, so a finished install has to drop the stale file.
clear_probe_cache() {
    rm -f .components_cache.json
}

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
    # uv sync prunes every package the selected groups do not mention, so a bare
    # deploy would strip an assembled TTS environment. librosa only enters the venv
    # through the tts group, so ask the venv whether that group was installed.
    if [[ "$install_tts" != true ]]; then
        local probe
        if [[ -x .venv/Scripts/python.exe ]]; then
            probe=.venv/Scripts/python.exe
        else
            probe=.venv/bin/python
        fi
        if [[ -x "$probe" ]] &&
            "$probe" -c "import sys, os; sys.stdout = sys.stderr = open(os.devnull, 'w'); import importlib.metadata as m; m.version('librosa')" 2>/dev/null; then
            install_tts=true
            echo "==> tts group already installed, keeping it. Remove it with a hand-run uv sync."
        fi
    fi
    if [[ "$install_tts" == true ]]; then
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
    echo "components to install: $install_components"
    echo "components to start: $start_components"
    sync_env
    exit 0
fi

# The status report stands on its own, it needs no install step.
if [[ "$status_only" == true ]]; then
    if [[ -x .venv/Scripts/python.exe ]]; then
        status_python=.venv/Scripts/python.exe
    elif [[ -x .venv/bin/python ]]; then
        status_python=.venv/bin/python
    else
        echo "no virtualenv here; run deploy.sh without --start-only" >&2
        exit 1
    fi
    if [[ "$json_out" == true ]]; then
        "$status_python" components.py status --json
    else
        "$status_python" components.py status
    fi
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
# Every component has its own step, so one broken piece does not take the rest down.
if [[ "$start_only" == false ]]; then
    if [[ "$install_tts" == true ]]; then
        echo "==> tts pieces"
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
        clear_probe_cache
    fi

    if [[ "$install_stt" == true ]]; then
        echo "==> stt pieces"
        # whisper itself comes with the core dependencies, only the model file has
        # to be fetched, and it lands in the user cache.
        "$python" -c "import os, whisper; whisper.load_model(os.getenv('STT_MODEL', 'tiny')); print('whisper model ready:', os.getenv('STT_MODEL', 'tiny'))"
        clear_probe_cache
    fi

    if [[ "$install_mcp" == true ]]; then
        echo "==> mcp pieces"
        if command -v node >/dev/null 2>&1; then
            echo "node $(node --version) -> $(command -v node)"
        else
            echo "node not found, the MCP gateway needs Node.js 18 or newer" >&2
        fi
        if command -v windows-mcp >/dev/null 2>&1; then
            echo "windows-mcp -> $(command -v windows-mcp)"
        else
            echo "windows-mcp not on PATH, run mcp/setup_windows_mcp.bat on Windows" >&2
        fi
    fi
fi

if [[ "$deploy_only" == true ]]; then
    echo "==> deploy done, server not started"
    exit 0
fi

# Step 3: the MCP gateway. The device talks to this one; the file server only
# stores the pictures and the audio that come back from the tools.
[[ -n "$port" ]] && export PORT="$port"
[[ -n "$host" ]] && export HOST="$host"
[[ "$autoload" == true ]] && export TTS_AUTOLOAD=1
[[ -n "$speaker" ]] && export TTS_SPEAKER="$speaker"

# The server registers the endpoints of the components that are on, so an
# unselected component answers 404 instead of failing halfway through a request.
if [[ "$start_tts" == true ]]; then export SERVER_ENABLE_TTS=1; else export SERVER_ENABLE_TTS=0; fi
if [[ "$start_stt" == true ]]; then export SERVER_ENABLE_STT=1; else export SERVER_ENABLE_STT=0; fi

echo "==> components: $start_components"

if [[ "$start_mcp" == true ]]; then
    echo "==> starting the MCP gateway on port $mcp_port"
    # pid 文件跟 bat 那份对齐，控制台停网关时按 pid 找进程
    mcp_pid_file="$PWD/mcp_gateway.pid"
    "$python" mcp/mcp_gateway.py --host 0.0.0.0 --port "$mcp_port" \
        --root "$HOME" --file-server "http://127.0.0.1:${PORT:-8000}" \
        --pid-file "$mcp_pid_file" \
        >> mcp_gateway.log 2>&1 &
    mcp_pid=$!
    # The file server runs in the foreground, so the gateway has to go down with it.
    # The gateway spawns the MCP server as a child, so the children go down first.
    trap 'for child in $(pgrep -P "$mcp_pid" 2>/dev/null); do kill "$child" 2>/dev/null || true; done; kill "$mcp_pid" 2>/dev/null || true' EXIT
fi

# Step 4: start the file server.
echo "==> starting on ${HOST:-0.0.0.0}:${PORT:-8000}"
"$python" main.py
