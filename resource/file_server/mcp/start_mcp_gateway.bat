@echo off
rem Start the MCP gateway for the RT-Thread Agent.
rem Usage: start_mcp_gateway.bat [port] [file tool root] [extra gateway args]
rem Port defaults to 8765, root defaults to the user profile.
rem Extra args go to mcp_gateway.py.
rem Windows-MCP is plugged in automatically. When uv or the windows-mcp package
rem is missing, the script installs them before the gateway starts.
rem Pass --no-windows-mcp to keep the desktop tools out.
rem Keep this file ASCII: cmd reads batch files in the OEM code page, and UTF-8
rem Chinese bytes get split into bogus commands there.
setlocal EnableExtensions
cd /d "%~dp0"

set PORT=8765
set ROOT=%USERPROFILE%
set EXTRA=
set MCP_HEAD=
set MCP_TAIL=
set WANT_WINDOWS_MCP=1
set HAVE_MCP_CMD=0

rem The port and the root are positional, the rest goes to the gateway. An
rem argument that starts with a dash is never taken as the port or the root.
echo(%~1| findstr /r "^[0-9][0-9]*$" >/dev/null
if errorlevel 1 goto collect
set PORT=%~1
shift
if "%~1"=="" goto collect
echo(%~1| findstr /r "^-" >/dev/null
if not errorlevel 1 goto collect
set ROOT=%~1
shift

:collect
if "%~1"=="" goto run
if /i "%~1"=="--no-windows-mcp" set WANT_WINDOWS_MCP=0
if /i "%~1"=="--windows-mcp-cmd" set HAVE_MCP_CMD=1
set EXTRA=%EXTRA% %1
shift
goto collect

:run
if "%WANT_WINDOWS_MCP%"=="0" goto start_gateway
set MCP_HEAD=--windows-mcp
if "%HAVE_MCP_CMD%"=="1" goto start_gateway
call "%~dp0setup_windows_mcp.bat"
if errorlevel 1 (
    set MCP_HEAD=
    goto start_gateway
)
set MCP_TAIL=--windows-mcp-cmd %MCP_CMD%

:start_gateway
echo [mcp] listening on port %PORT%, file tool root %ROOT%
echo [mcp] device side: PKG_AGENT_MCP_SERVER_URL=http://PC-IP:%PORT%/mcp
python -u mcp_gateway.py --host 0.0.0.0 --port %PORT% --root "%ROOT%" %MCP_HEAD% %EXTRA% %MCP_TAIL%
endlocal
exit /b 0
