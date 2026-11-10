@echo off
rem Find or install Windows-MCP and leave its start command in MCP_CMD.
rem The caller starts the gateway with that command, so the desktop tools
rem are reachable. When Windows-MCP cannot be set up, MCP_CMD stays empty
rem and the gateway runs with the built-in file tools only.
rem This file has no setlocal on purpose: MCP_CMD has to reach the caller.
rem Keep this file ASCII: cmd reads batch files in the OEM code page, and UTF-8
rem Chinese bytes get split into bogus commands there.

rem Find uv. The official installer puts uv in %USERPROFILE%\.local\bin, and that
rem is the directory uv tool install writes the executables to.
set "UV_BIN=%USERPROFILE%\.local\bin"
if exist "%UV_BIN%\uv.exe" set "PATH=%UV_BIN%;%PATH%"
set "UV="
for /f "delims=" %%i in ('where uv 2^>nul') do if not defined UV set "UV=%%i"
if not defined UV goto install_uv

:find_tool
rem Ask uv where it puts the tool executables.
set "TOOL_BIN="
for /f "delims=" %%i in ('uv tool dir --bin 2^>nul') do set "TOOL_BIN=%%i"
if not defined TOOL_BIN set "TOOL_BIN=%UV_BIN%"
if exist "%TOOL_BIN%" set "PATH=%TOOL_BIN%;%PATH%"
rem The windows-mcp executable is the sign that the package is already there.
set "MCP_EXE="
for /f "delims=" %%i in ('where windows-mcp 2^>nul') do if not defined MCP_EXE set "MCP_EXE=%%i"
if not defined MCP_EXE if exist "%TOOL_BIN%\windows-mcp.exe" set "MCP_EXE=%TOOL_BIN%\windows-mcp.exe"
if defined MCP_EXE goto use_tool

echo [mcp] installing the windows-mcp package with uv
uv tool install windows-mcp
if errorlevel 1 goto use_uvx
if not exist "%TOOL_BIN%\windows-mcp.exe" goto use_uvx
set "MCP_EXE=%TOOL_BIN%\windows-mcp.exe"
goto use_tool

:use_uvx
rem uvx downloads the package on first use, so it also works as a fallback.
echo [mcp] falling back to uvx, it fetches the package on first use
set "MCP_CMD=uvx windows-mcp serve"
exit /b 0

:use_tool
echo [mcp] Windows-MCP command: %MCP_EXE%
set MCP_CMD="%MCP_EXE%" serve
exit /b 0

:install_uv
echo [mcp] uv not found, installing it now
rem PowerShell is not always on PATH, so call it by its full path.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=powershell"
"%PS%" -NoProfile -ExecutionPolicy Bypass -Command "iex (irm https://astral.sh/uv/install.ps1)"
if errorlevel 1 (
    echo [mcp] uv install failed, get it from https://astral.sh/uv and run this script again
    exit /b 1
)
set "PATH=%UV_BIN%;%PATH%"
set "UV="
for /f "delims=" %%i in ('where uv 2^>nul') do if not defined UV set "UV=%%i"
if not defined UV (
    echo [mcp] uv is installed but not on PATH
    exit /b 1
)
goto find_tool
