@echo off
rem Deploy the file server and the MCP gateway, then start both.
rem   deploy.bat                     environment + start
rem   deploy.bat --start LIST        start only these components
rem   deploy.bat --install LIST      install only these components' dependencies
rem   deploy.bat --status            show each component's dependency status
rem   deploy.bat --tts               also fetch the GPT-SoVITS pieces
rem   deploy.bat --tts --check       deploy only, do not start the server
rem   deploy.bat --start-only        skip the deploy steps, just start
rem   deploy.bat --print             only show the uv command, change nothing
rem   deploy.bat --port 8001         listen on another port (default 8000)
rem   deploy.bat --host 127.0.0.1    bind another address (default 0.0.0.0)
rem   deploy.bat --speaker NAME      voice used when a request omits "speaker"
rem   deploy.bat --autoload          warm the TTS model while the server boots
rem   deploy.bat --cpu               force the CPU torch build
rem   deploy.bat --no-mcp            do not start the MCP gateway
rem   deploy.bat --mcp-port 8765     MCP gateway port (default 8765)
rem   deploy.bat --json              --status prints JSON instead of text
rem
rem Components: file-server, mcp, tts, stt
rem   --start and --install take a comma separated list of those names.
rem
rem GitHub downloads go through gh-proxy by default, because a direct connection
rem to the FFmpeg release is usually cut here (curl exits with 35). Use --no-mirror
rem to go straight to github.com.
setlocal EnableExtensions EnableDelayedExpansion

set "TTS=0"
set "DEPLOY_ONLY=0"
set "START_ONLY=0"
set "PRINT=0"
set "AUTOLOAD=0"
set "MIRROR=1"
set "BACKEND=auto"
set "PORTARG="
set "HOSTARG="
set "SPEAKER="
set "MCP=1"
set "MCP_PORT=8765"
set "STATUS=0"
set "JSONOUT=0"
set "START_LIST="
set "INSTALL_LIST="
set "PENDING="
set "INSTALL_COMPONENTS="
set "START_COMPONENTS="
set "INSTALL_TTS=0"
set "INSTALL_STT=0"
set "INSTALL_MCP=0"
set "START_TTS=0"
set "START_STT=0"
set "START_MCP=0"
set "PY=%~dp0.venv\Scripts\python.exe"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--tts" (set "TTS=1" & shift & goto parse)
if /i "%~1"=="--check" (set "DEPLOY_ONLY=1" & shift & goto parse)
if /i "%~1"=="--start-only" (set "START_ONLY=1" & shift & goto parse)
if /i "%~1"=="--print" (set "PRINT=1" & shift & goto parse)
if /i "%~1"=="--autoload" (set "AUTOLOAD=1" & shift & goto parse)
if /i "%~1"=="--status" (set "STATUS=1" & shift & goto parse)
if /i "%~1"=="--json" (set "JSONOUT=1" & shift & goto parse)
if /i "%~1"=="--start" (
    if "%~2"=="" (echo --start needs a value 1>&2 & exit /b 2)
    set "START_LIST=%~2" & set "PENDING=start" & shift & shift & goto parse
)
if /i "%~1"=="--install" (
    if "%~2"=="" (echo --install needs a value 1>&2 & exit /b 2)
    set "INSTALL_LIST=%~2" & set "PENDING=install" & shift & shift & goto parse
)
if /i "%~1"=="--mirror" (set "MIRROR=1" & shift & goto parse)
if /i "%~1"=="--no-mirror" (set "MIRROR=0" & shift & goto parse)
if /i "%~1"=="--no-mcp" (set "MCP=0" & shift & goto parse)
if /i "%~1"=="--cpu" (set "BACKEND=cpu" & shift & goto parse)
if /i "%~1"=="--cu126" (set "BACKEND=cu126" & shift & goto parse)
if /i "%~1"=="--cu130" (set "BACKEND=cu130" & shift & goto parse)
if /i "%~1"=="--mcp-port" (
    if "%~2"=="" (echo --mcp-port needs a value 1>&2 & exit /b 2)
    set "MCP_PORT=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--port" (
    if "%~2"=="" (echo --port needs a value 1>&2 & exit /b 2)
    set "PORTARG=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--host" (
    if "%~2"=="" (echo --host needs a value 1>&2 & exit /b 2)
    set "HOSTARG=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--speaker" (
    if "%~2"=="" (echo --speaker needs a value 1>&2 & exit /b 2)
    set "SPEAKER=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--help" (call :usage & exit /b 0)
rem cmd splits a comma separated list into separate arguments, so the names
rem after the first one arrive one by one. A bare component name continues the
rem list of the flag before it.
if not "%PENDING%"=="" (
    call :extend_list "%~1"
    if not errorlevel 1 (shift & goto parse)
    echo --%PENDING%: unknown component '%~1' ^(file-server, mcp, tts, stt^) 1>&2
    exit /b 2
)
echo unknown option: %~1 1>&2
call :usage 1>&2
exit /b 2

:parsed
if "%START_ONLY%"=="1" if "%DEPLOY_ONLY%"=="1" (
    echo --start-only and --check contradict each other 1>&2
    exit /b 2
)

rem cmd hands a comma separated list over as separate arguments, so the names
rem after the first one arrive one by one. Rebuild both lists from their tokens,
rem then a stray comma or space does not matter.
call :clean_list "%INSTALL_LIST%"
set "INSTALL_LIST=%NL%"
call :clean_list "%START_LIST%"
set "START_LIST=%NL%"

rem What to install: --install wins, --tts keeps the old meaning, otherwise just
rem the file server itself.
if "%INSTALL_LIST%"=="" (
    if "%TTS%"=="1" (set "INSTALL_COMPONENTS=file-server,tts") else (set "INSTALL_COMPONENTS=file-server")
) else (
    set "INSTALL_COMPONENTS=%INSTALL_LIST%"
)
call :validate_list "%INSTALL_COMPONENTS%" --install
if errorlevel 1 exit /b 2

rem What to start: --start wins, otherwise everything. --no-mcp drops the gateway.
if "%START_LIST%"=="" (
    set "START_COMPONENTS=file-server,mcp,tts,stt"
) else (
    set "START_COMPONENTS=%START_LIST%"
)
call :validate_list "%START_COMPONENTS%" --start
if errorlevel 1 exit /b 2
if "%MCP%"=="0" call :drop_component mcp

rem The file server is the ground the other components stand on, it is always in.
call :has_component "%INSTALL_COMPONENTS%" file-server
if errorlevel 1 set "INSTALL_COMPONENTS=file-server,%INSTALL_COMPONENTS%"
call :has_component "%START_COMPONENTS%" file-server
if errorlevel 1 set "START_COMPONENTS=file-server,%START_COMPONENTS%"

call :has_component "%INSTALL_COMPONENTS%" tts
if not errorlevel 1 set "INSTALL_TTS=1"
call :has_component "%INSTALL_COMPONENTS%" stt
if not errorlevel 1 set "INSTALL_STT=1"
call :has_component "%INSTALL_COMPONENTS%" mcp
if not errorlevel 1 set "INSTALL_MCP=1"
call :has_component "%START_COMPONENTS%" tts
if not errorlevel 1 set "START_TTS=1"
call :has_component "%START_COMPONENTS%" stt
if not errorlevel 1 set "START_STT=1"
call :has_component "%START_COMPONENTS%" mcp
if not errorlevel 1 set "START_MCP=1"

if "%PRINT%"=="1" (
    echo components to install: %INSTALL_COMPONENTS%
    echo components to start: %START_COMPONENTS%
    call :sync_env
    exit /b 0
)

rem The status report stands on its own, it needs no install step.
if "%STATUS%"=="1" (
    if not exist "%PY%" (
        echo no virtualenv here; run deploy.bat without --start-only 1>&2
        exit /b 1
    )
    if "%JSONOUT%"=="1" (
        "%PY%" "%~dp0components.py" status --json
    ) else (
        "%PY%" "%~dp0components.py" status
    )
    exit /b 0
)

rem Step 1: environment. Build the venv and report what torch ended up in it.
if "%START_ONLY%"=="0" (
    echo ==^> environment
    call :sync_env
    if errorlevel 1 exit /b 1
)

if not exist "%PY%" (
    echo no virtualenv here; run deploy.bat without --start-only 1>&2
    exit /b 1
)

rem Step 2: the blobs that stay out of git. Idempotent, so re-running is cheap.
rem Every component has its own step, so one broken piece does not take the rest down.
if "%START_ONLY%"=="0" (
    if "%INSTALL_TTS%"=="1" (
        echo ==^> tts pieces
        if "%MIRROR%"=="1" (
            "%PY%" "%~dp0fetch_tts.py" dicts voice assets --mirror
        ) else (
            "%PY%" "%~dp0fetch_tts.py" dicts voice assets
        )
        if errorlevel 1 exit /b 1
        "%PY%" "%~dp0fetch_tts.py" check
        if errorlevel 1 exit /b 1

        rem uv sync prunes whatever the lock does not mention. g2pk2's Korean path needs
        rem eunjeon, and eunjeon imports pkg_resources, which setuptools 84 dropped.
        rem The directory can survive the prune as an empty namespace package, so ask the
        rem metadata, not the import machinery.
        "%PY%" -c "import importlib.metadata as m; m.version('eunjeon')" >nul 2>&1
        if errorlevel 1 (
            echo ==^> restoring the Korean extras ^(eunjeon, setuptools^<81^)
            uv pip install --python "%PY%" eunjeon "setuptools<81"
            if errorlevel 1 exit /b 1
        )
        call :clear_probe_cache
    )

    if "%INSTALL_STT%"=="1" (
        echo ==^> stt pieces
        rem whisper itself comes with the core dependencies, only the model file has
        rem to be fetched, and it lands in the user cache.
        "%PY%" -c "import os, whisper; whisper.load_model(os.getenv('STT_MODEL', 'tiny')); print('whisper model ready:', os.getenv('STT_MODEL', 'tiny'))"
        if errorlevel 1 exit /b 1
        call :clear_probe_cache
    )

    if "%INSTALL_MCP%"=="1" (
        echo ==^> mcp pieces
        call "%~dp0mcp\setup_windows_mcp.bat"
        if errorlevel 1 echo [mcp] Windows-MCP is not ready, the gateway runs without the desktop tools
        where node >nul 2>&1
        if errorlevel 1 echo [mcp] node not found, the MCP gateway needs Node.js 18 or newer 1>&2
    )
)

if "%DEPLOY_ONLY%"=="1" (
    echo ==^> deploy done, server not started
    exit /b 0
)

rem Step 3: start the server.
if not "%PORTARG%"=="" set "PORT=%PORTARG%"
if not "%HOSTARG%"=="" set "HOST=%HOSTARG%"
if "%AUTOLOAD%"=="1" set "TTS_AUTOLOAD=1"
if "%PORT%"=="" set "PORT=8000"
if "%HOST%"=="" set "HOST=0.0.0.0"
if not "%SPEAKER%"=="" set "TTS_SPEAKER=%SPEAKER%"

rem The server registers the endpoints of the components that are on, so an
rem unselected component answers 404 instead of failing halfway through a request.
if "%START_TTS%"=="1" (set "SERVER_ENABLE_TTS=1") else (set "SERVER_ENABLE_TTS=0")
if "%START_STT%"=="1" (set "SERVER_ENABLE_STT=1") else (set "SERVER_ENABLE_STT=0")

echo ==^> components: %START_COMPONENTS%

rem Step 4: start the MCP gateway in the background. The device talks
rem to this one; the file server only stores the pictures and the audio.
if "%START_MCP%"=="0" goto start_file_server
call "%~dp0mcp\setup_windows_mcp.bat"
if errorlevel 1 (
    echo [mcp] Windows-MCP is not ready, the gateway runs without the desktop tools
    set "MCPHEAD="
    set "MCPTAIL="
    goto launch_mcp
)
set "MCPHEAD=--windows-mcp"
set "MCPTAIL=--windows-mcp-cmd %MCP_CMD%"

:launch_mcp
set "PIDFILE=%~dp0mcp_gateway.pid"
rem A cmd redirect to nul cannot open its target on this box, so the stale pid
rem file goes away through the venv python.
"%PY%" -c "import os,sys; p=sys.argv[1]; os.path.exists(p) and os.remove(p)" "%PIDFILE%"
start "" /b cmd /c ""%PY%" -u "%~dp0mcp\mcp_gateway.py" --host 0.0.0.0 --port %MCP_PORT% --root "%USERPROFILE%" --file-server "http://127.0.0.1:%PORT%" --pid-file "%PIDFILE%" %MCPHEAD% %MCPTAIL% >> "%~dp0mcp_gateway.log" 2>&1"
rem The gateway writes the pid file once it listens. A cmd redirect to nul
rem cannot open its target on this box, so wait with the venv python.
set /a GW_WAIT=0
:wait_mcp
set "GW_PID="
if exist "%PIDFILE%" for /f "usebackq delims=" %%p in ("%PIDFILE%") do set "GW_PID=%%p"
if defined GW_PID goto got_mcp
set /a GW_WAIT+=1
if %GW_WAIT% GEQ 60 goto no_mcp
"%PY%" -c "import time; time.sleep(0.5)"
goto wait_mcp

:got_mcp
echo ==^> the MCP gateway is up on port %MCP_PORT%, pid !GW_PID!, log %~dp0mcp_gateway.log
echo ==^> device side: PKG_AGENT_MCP_SERVER_URL=http://PC-IP:%MCP_PORT%/mcp
goto start_file_server

:no_mcp
echo the MCP gateway did not start, see %~dp0mcp_gateway.log

:start_file_server
cd /d "%~dp0"
echo ==^> starting on %HOST%:%PORT%
"%PY%" main.py
set "SRV_CODE=%ERRORLEVEL%"
rem The file server is down, so the gateway goes down with it.
if defined GW_PID call :stop_mcp
exit /b %SRV_CODE%

rem taskkill /T cannot walk the tree on this box, so PowerShell does it. The
rem gateway spawns the MCP server, and that child has to go down as well.
:stop_mcp
powershell -NoProfile -Command "$all = Get-CimInstance Win32_Process; function Stop-Tree($id) { foreach ($p in $all | Where-Object { $_.ParentProcessId -eq $id }) { Stop-Tree $p.ProcessId; Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue } }; Stop-Tree !GW_PID!; Stop-Process -Id !GW_PID! -Force -ErrorAction SilentlyContinue"
exit /b 0

rem Build the venv. uv cannot resolve hardware, so the choice happens here:
rem compute capability decides which dependency group gets synced. cu126 wheels
rem stop at sm_90, Blackwell needs cu130.
:sync_env
where uv >nul 2>&1
if errorlevel 1 (
    echo uv is not on PATH. See https://docs.astral.sh/uv/getting-started/installation/ 1>&2
    exit /b 1
)

if "%BACKEND%"=="auto" (
    set "DETECTED="
    for /f "usebackq delims=" %%b in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$caps = & nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>$null | ForEach-Object { if ($_ -match '[0-9]+(\.[0-9]+)?') { [double]$Matches[0] } }; if ($caps) { $m = ($caps | Measure-Object -Maximum).Maximum; if ($m -ge 10.0) { [Console]::Error.WriteLine('GPU compute capability ' + $m + ' -> CUDA 13.0 build (Blackwell)'); 'cu130' } else { [Console]::Error.WriteLine('GPU compute capability ' + $m + ' -> CUDA 12.6 build'); 'cu126' } } else { $n = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'NVIDIA' }; if ($n) { if (($n.Name -join ' ') -match 'RTX 50|B\d00|Blackwell') { [Console]::Error.WriteLine('NVIDIA adapter detected -> CUDA 13.0 build (Blackwell)'); 'cu130' } else { [Console]::Error.WriteLine('NVIDIA adapter detected -> CUDA 12.6 build'); 'cu126' } } else { [Console]::Error.WriteLine('No NVIDIA GPU detected -> CPU build'); 'cpu' } }"`) do set "DETECTED=%%b"
    if "!DETECTED!"=="" set "DETECTED=cpu"
    set "BACKEND=!DETECTED!"
)

rem cpu is the default group; the GPU groups conflict with it, so the default has
rem to be turned off explicitly before one of them can be enabled.
if "%BACKEND%"=="cpu" (
    set "UVARGS=sync"
) else (
    set "UVARGS=sync --no-default-groups --group gpu-%BACKEND%"
)
rem tts is orthogonal to the backend groups and simply rides along.
rem uv sync prunes every package the selected groups do not mention, so a bare
rem deploy would strip an assembled TTS environment. librosa only enters the venv
rem through the tts group, so ask the venv whether that group is installed.
rem Python opens nul itself: a cmd-level redirect can fail to open its target and
rem leave errorlevel at 0, which would read as "installed".
if "%INSTALL_TTS%"=="0" if exist "%PY%" (
    "%PY%" -c "import sys, os; sys.stdout = sys.stderr = open(os.devnull, 'w'); import importlib.metadata as m; m.version('librosa')"
    if not errorlevel 1 (
        set "INSTALL_TTS=1"
        echo ==^> tts group already installed, keeping it. Remove it with a hand-run uv sync.
    )
)
if "%INSTALL_TTS%"=="1" set "UVARGS=!UVARGS! --group tts"

echo uv !UVARGS!
if "%PRINT%"=="1" exit /b 0

uv !UVARGS!
if errorlevel 1 exit /b 1

set "TTS_BACKEND=%BACKEND%"
"%PY%" -c "import os, torch; print('torch', torch.__version__, '| cuda available:', torch.cuda.is_available()); print('WARNING: a GPU build was installed but CUDA is unavailable (driver missing or too old)') if (not (os.environ.get('TTS_BACKEND') == 'cpu') and not torch.cuda.is_available()) else None"
exit /b 0

rem A bare component name continues the list of the flag before it.
:extend_list
set "EL_NAME=%~1"
if /i "%EL_NAME%"=="file-server" goto extend_ok
if /i "%EL_NAME%"=="mcp" goto extend_ok
if /i "%EL_NAME%"=="tts" goto extend_ok
if /i "%EL_NAME%"=="stt" goto extend_ok
exit /b 1
:extend_ok
if /i "%PENDING%"=="start" (set "START_LIST=!START_LIST!,%EL_NAME%") else (set "INSTALL_LIST=!INSTALL_LIST!,%EL_NAME%")
exit /b 0

rem One list, rebuilt from its tokens. Empty pieces are dropped.
:clean_list
set "CL_OUT="
for %%C in (%~1) do (
    if defined CL_OUT (set "CL_OUT=!CL_OUT!,%%C") else (set "CL_OUT=%%C")
)
set "NL=!CL_OUT!"
exit /b 0

rem Does the list hold the name? errorlevel 0 means yes.
:has_component
set "HC_LIST=%~1"
set "HC_NAME=%~2"
set "HC=1"
for %%C in (%HC_LIST:,= %) do (
    if /i "%%C"=="!HC_NAME!" set "HC=0"
)
if "%HC%"=="0" exit /b 0
exit /b 1

rem Every name in the list has to be a known component.
:validate_list
set "VL=%~1"
set "VFLAG=%~2"
for %%C in (%VL:,= %) do (
    if not "%%C"=="file-server" if not "%%C"=="mcp" if not "%%C"=="tts" if not "%%C"=="stt" (
        echo %VFLAG%: unknown component '%%C' ^(file-server, mcp, tts, stt^) 1>&2
        exit /b 2
    )
)
exit /b 0

rem Take a component out of the start list.
:drop_component
set "DC_NAME=%~1"
set "DC_KEEP="
for %%C in (!START_COMPONENTS:,= !) do (
    if not /i "%%C"=="!DC_NAME!" (
        if defined DC_KEEP (set "DC_KEEP=!DC_KEEP!,%%C") else (set "DC_KEEP=%%C")
    )
)
set "START_COMPONENTS=!DC_KEEP!"
exit /b 0

rem The probe caches its findings, so a finished install has to drop the stale file.
:clear_probe_cache
"%PY%" -c "import os,sys; p=sys.argv[1]; os.path.exists(p) and os.remove(p)" "%~dp0.components_cache.json"
exit /b 0

:usage
echo usage: deploy.bat [--tts] [--check] [--start-only] [--print] [--port N] [--host ADDR]
echo                   [--speaker NAME]
echo                   [--autoload] [--cpu^|--cu126^|--cu130] [--mirror^|--no-mirror]
echo                   [--no-mcp] [--mcp-port N]
echo                   [--start LIST^] [--install LIST^] [--status] [--json]
echo components: file-server, mcp, tts, stt
exit /b 0
