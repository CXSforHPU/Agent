@echo off
setlocal enabledelayedexpansion
rem One-click install and start for the agent console (frontend + backend).
rem Usage:
rem   setup.bat              install deps, build, start both
rem   setup.bat --install    install deps and build only
rem   setup.bat --server     start backend only
rem   setup.bat --client     start frontend only
rem   setup.bat --stop       stop both
rem
rem ASCII only: cmd reads this file in OEM code page 936.

cd /d "%~dp0"
set "WEB=%cd%"
set "SERVER=%WEB%\server"
set "CLIENT=%WEB%\client"
set "JDKDIR=%SERVER%\.jdk"
set "MAVENDIR=%SERVER%\.maven"
set "RUNDIR=%WEB%\.run"
set "JAR=%SERVER%\target\agent-console-server-1.0.0.jar"
set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=all"

if /i "%ACTION%"=="--stop" goto :stop
if /i "%ACTION%"=="--install" goto :install
if /i "%ACTION%"=="--server" goto :server
if /i "%ACTION%"=="--client" goto :client
if /i "%ACTION%"=="all" goto :start_setup
echo Unknown option: %ACTION%
echo Use: setup.bat [--install|--server|--client|--stop]
exit /b 1

:start_setup
call :ensure_jdk || exit /b 1
call :ensure_maven || exit /b 1
call :build_server || exit /b 1
call :install_client
call :start_server || exit /b 1
call :start_client
echo.
echo All ready.
echo   Backend  http://127.0.0.1:8080
echo   Frontend http://127.0.0.1:5173
echo   Stop     setup.bat --stop
exit /b 0

:install
call :ensure_jdk || exit /b 1
call :ensure_maven || exit /b 1
call :build_server || exit /b 1
call :install_client
exit /b 0

:server
call :ensure_jdk || exit /b 1
call :start_server || exit /b 1
exit /b 0

:client
call :start_client || exit /b 1
exit /b 0

:stop
call :kill_port 8080
call :kill_port 5173
echo Stopped.
exit /b 0

rem ---------- toolchain ----------

:ensure_jdk
where java >nul 2>nul
if not errorlevel 1 (
    echo [web] using system java
    exit /b 0
)
if exist "%JDKDIR%" (
    for /d %%D in ("%JDKDIR%\jdk-*") do set "JAVA_HOME=%%D"
)
if defined JAVA_HOME if exist "%JAVA_HOME%\bin\java.exe" (
    echo [web] using bundled JDK: !JAVA_HOME!
    exit /b 0
)
echo [web] downloading portable JDK, about 190 MB
if not exist "%JDKDIR%" mkdir "%JDKDIR%"
if not exist "%JDKDIR%\jdk.zip" (
    curl -fL --retry 3 -o "%JDKDIR%\jdk.zip" "https://mirrors.tuna.tsinghua.edu.cn/Adoptium/17/jdk/x64/windows/OpenJDK17U-jdk_x64_windows_hotspot_17.0.21_9.zip"
    if errorlevel 1 (
        echo [web] JDK download failed, put the zip at %JDKDIR%\jdk.zip and retry
        exit /b 1
    )
)
tar -xf "%JDKDIR%\jdk.zip" -C "%JDKDIR%" || exit /b 1
for /d %%D in ("%JDKDIR%\jdk-*") do set "JAVA_HOME=%%D"
if not defined JAVA_HOME (
    echo [web] no jdk-* folder after extract
    exit /b 1
)
echo [web] JDK ready: !JAVA_HOME!
exit /b 0

:ensure_maven
where mvn >nul 2>nul
if not errorlevel 1 (
    echo [web] using system mvn
    set "MVN=mvn"
    exit /b 0
)
if exist "%MAVENDIR%" (
    for /d %%D in ("%MAVENDIR%\apache-maven-*") do set "MVNHOME=%%D"
)
if defined MVNHOME if exist "%MVNHOME%\bin\mvn.cmd" (
    set "MVN=%MVNHOME%\bin\mvn.cmd"
    echo [web] using bundled Maven: !MVNHOME!
    exit /b 0
)
echo [web] downloading portable Maven
if not exist "%MAVENDIR%" mkdir "%MAVENDIR%"
if not exist "%MAVENDIR%\maven.zip" (
    curl -fL --retry 3 -o "%MAVENDIR%\maven.zip" "https://archive.apache.org/dist/maven/maven-3/3.9.9/binaries/apache-maven-3.9.9-bin.zip"
    if errorlevel 1 (
        echo [web] Maven download failed, put the zip at %MAVENDIR%\maven.zip and retry
        exit /b 1
    )
)
tar -xf "%MAVENDIR%\maven.zip" -C "%MAVENDIR%" || exit /b 1
for /d %%D in ("%MAVENDIR%\apache-maven-*") do set "MVNHOME=%%D"
set "MVN=%MVNHOME%\bin\mvn.cmd"
echo [web] Maven ready: !MVNHOME!
exit /b 0

rem ---------- install ----------

:build_server
echo [web] building backend
call "%MVN%" -q -f "%SERVER%\pom.xml" -DskipTests package
if errorlevel 1 (
    echo [web] backend build failed
    exit /b 1
)
echo [web] jar: %JAR%
exit /b 0

:install_client
if not exist "%CLIENT%\package.json" (
    echo [web] frontend not written yet, skip
    exit /b 0
)
echo [web] installing frontend deps
pushd "%CLIENT%"
call npm install --no-fund --no-audit
popd
exit /b 0

rem ---------- start / stop ----------

:wait_http
rem %1 url, %2 seconds
set /a _n=0
:wait_http_loop
curl -fsS -o nul "%~1" >nul 2>nul
if not errorlevel 1 exit /b 0
set /a _n+=1
if %_n% geq %~2 exit /b 1
ping -n 2 127.0.0.1 >nul
goto :wait_http_loop

:start_server
if not exist "%JAR%" (
    echo [web] backend not built, run setup.bat --install first
    exit /b 1
)
call :wait_http "http://127.0.0.1:8080/api/health" 2
if not errorlevel 1 (
    echo [web] backend already running on 8080
    exit /b 0
)
call :kill_port 8080
if not exist "%RUNDIR%" mkdir "%RUNDIR%"
echo [web] starting backend on 8080, log %RUNDIR%\server.log
start "agent-console-server" /b cmd /c "cd /d "%SERVER%" && "%JAVA_HOME%\bin\java.exe" -jar "%JAR%" > "%RUNDIR%\server.log" 2>&1"
call :wait_http "http://127.0.0.1:8080/api/health" 90
if errorlevel 1 (
    echo [web] backend did not come up in 90s, last lines:
    powershell -NoProfile -Command "Get-Content -Tail 40 '%RUNDIR%\server.log'"
    exit /b 1
)
echo [web] backend ready: http://127.0.0.1:8080
exit /b 0

:start_client
if not exist "%CLIENT%\package.json" (
    echo [web] frontend not written yet, skip
    exit /b 0
)
call :wait_http "http://127.0.0.1:5173" 2
if not errorlevel 1 (
    echo [web] frontend already running on 5173
    exit /b 0
)
call :kill_port 5173
if not exist "%RUNDIR%" mkdir "%RUNDIR%"
echo [web] starting frontend on 5173, log %RUNDIR%\client.log
start "agent-console-client" /b cmd /c "cd /d "%CLIENT%" && npm run dev > "%RUNDIR%\client.log" 2>&1"
call :wait_http "http://127.0.0.1:5173" 90
if errorlevel 1 (
    echo [web] frontend did not come up in 90s, last lines:
    powershell -NoProfile -Command "Get-Content -Tail 40 '%RUNDIR%\client.log'"
    exit /b 1
)
echo [web] frontend ready: http://127.0.0.1:5173
exit /b 0

:kill_port
rem %1 port. netstat columns: proto local foreign state pid
for /f "tokens=5" %%P in ('netstat -ano ^| findstr /c:":%~1 " ^| findstr /c:"LISTENING"') do (
    taskkill /PID %%P /F >nul 2>nul
)
exit /b 0
