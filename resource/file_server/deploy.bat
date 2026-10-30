@echo off
rem Deploy the file server and start it.
rem   deploy.bat                     environment + start
rem   deploy.bat --tts               also fetch the GPT-SoVITS pieces
rem   deploy.bat --tts --check       deploy only, do not start the server
rem   deploy.bat --start-only        skip the deploy steps, just start
rem   deploy.bat --port 8001         listen on another port (default 8000)
rem   deploy.bat --host 127.0.0.1    bind another address (default 0.0.0.0)
rem   deploy.bat --autoload          warm the TTS model while the server boots
rem   deploy.bat --cpu               force the CPU torch build
rem
rem GitHub downloads go through gh-proxy by default, because a direct connection
rem to the FFmpeg release is usually cut here (curl exits with 35). Use --no-mirror
rem to go straight to github.com.
setlocal EnableExtensions EnableDelayedExpansion

set "TTS=0"
set "DEPLOY_ONLY=0"
set "START_ONLY=0"
set "AUTOLOAD=0"
set "MIRROR=1"
set "BACKEND=auto"
set "PORTARG="
set "HOSTARG="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--tts" (set "TTS=1" & shift & goto parse)
if /i "%~1"=="--check" (set "DEPLOY_ONLY=1" & shift & goto parse)
if /i "%~1"=="--start-only" (set "START_ONLY=1" & shift & goto parse)
if /i "%~1"=="--autoload" (set "AUTOLOAD=1" & shift & goto parse)
if /i "%~1"=="--mirror" (set "MIRROR=1" & shift & goto parse)
if /i "%~1"=="--no-mirror" (set "MIRROR=0" & shift & goto parse)
if /i "%~1"=="--cpu" (set "BACKEND=cpu" & shift & goto parse)
if /i "%~1"=="--cu126" (set "BACKEND=cu126" & shift & goto parse)
if /i "%~1"=="--cu130" (set "BACKEND=cu130" & shift & goto parse)
if /i "%~1"=="--port" (
    if "%~2"=="" (echo --port needs a value 1>&2 & exit /b 2)
    set "PORTARG=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--host" (
    if "%~2"=="" (echo --host needs a value 1>&2 & exit /b 2)
    set "HOSTARG=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--help" (call :usage & exit /b 0)
echo unknown option: %~1 1>&2
call :usage 1>&2
exit /b 2

:parsed
if "%START_ONLY%"=="1" if "%DEPLOY_ONLY%"=="1" (
    echo --start-only and --check contradict each other 1>&2
    exit /b 2
)

rem Step 1: environment. setup_env.ps1 picks the torch build that fits the GPU.
if "%START_ONLY%"=="0" (
    echo ==^> environment
    if "%TTS%"=="1" (
        powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_env.ps1" -Torch %BACKEND% -Tts
    ) else (
        powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_env.ps1" -Torch %BACKEND%
    )
    if errorlevel 1 exit /b 1
)

set "PY=%~dp0.venv\Scripts\python.exe"
if not exist "%PY%" (
    echo no virtualenv here; run setup_env.ps1 first 1>&2
    exit /b 1
)

rem Step 2: the blobs that stay out of git. Idempotent, so re-running is cheap.
if "%TTS%"=="1" if "%START_ONLY%"=="0" (
    echo ==^> third-party pieces
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

cd /d "%~dp0"
echo ==^> starting on %HOST%:%PORT%
"%PY%" main.py
exit /b %ERRORLEVEL%

:usage
echo usage: deploy.bat [--tts] [--check] [--start-only] [--port N] [--host ADDR]
echo                   [--autoload] [--cpu^|--cu126^|--cu130] [--mirror^|--no-mirror]
exit /b 0
