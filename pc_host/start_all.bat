@echo off
REM ===================================================================
REM  Korvo_kk  PC side  --  UNIFIED LAUNCHER
REM
REM  One click does both:
REM    [1/3] NetEase CloudMusic -> (re)start with CEF remote debug 9222
REM          so the host can read the REAL playback position (P1/F43).
REM          Idempotent: if 9222 is already listening, CloudMusic is
REM          NOT touched, so currently playing music is never cut off.
REM    [2/3] pre-flight checks (venv python, project root)
REM    [3/3] pc_host.main -> PySide6 host (call / record / pcnow / sys)
REM
REM  This file is the SINGLE SOURCE OF TRUTH.
REM  The desktop shortcut  %USERPROFILE%\Desktop\ESP32_Voice_Host.bat
REM  is a 3-line wrapper that just calls this file.
REM  Run it directly, or via the desktop icon - identical result.
REM
REM  Opt-out: change SKIP_NETEASE to 1 below to never touch CloudMusic
REM           (e.g. when you are using Spotify instead).
REM
REM  NOTE: pure ASCII on purpose - avoids cmd codepage garbling.
REM ===================================================================
setlocal
title Korvo PC Host

REM Put the real Windows tools FIRST. If this script is ever launched from
REM Git Bash / MSYS, /usr/bin shadows find.exe, timeout.exe and curl.exe with
REM POSIX versions that ignore /I and /t - the cloudmusic detection and all
REM the waits would silently break. System32-first is the normal Windows
REM order anyway, so this only removes the pollution.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%PATH%"

set "SKIP_NETEASE=0"

set "PY=C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
set "CM=C:\Program Files\Netease\CloudMusic\cloudmusic.exe"
if not exist "%CM%" set "CM=C:\Program Files (x86)\Netease\CloudMusic\cloudmusic.exe"

REM project root = parent of this script's folder (pc_host\)
pushd "%~dp0.."
set "ROOT=%CD%"

echo ============================================================
echo   Korvo PC Host
echo   root = %ROOT%
echo ============================================================
echo.

REM ===================================================================
REM  [1/3] CloudMusic + CDP 9222
REM  NOTE: --noproxy "*" is REQUIRED. Clash/TUN sets http_proxy, which
REM        otherwise hijacks the 127.0.0.1 health probe and makes a
REM        perfectly healthy CDP look dead.
REM ===================================================================
if "%SKIP_NETEASE%"=="1" (
  echo [1/3] [SKIP] SKIP_NETEASE=1 - CloudMusic left alone.
  goto :step2
)

set "CDP_UP=0"
curl -s -m 2 --noproxy "*" http://127.0.0.1:9222/json/version >nul 2>&1
if not errorlevel 1 set "CDP_UP=1"

if "%CDP_UP%"=="1" (
  echo [1/3] CDP 9222 already up -^> keep running CloudMusic as is.
  goto :step2
)

if not exist "%CM%" (
  echo [1/3] [SKIP] CloudMusic not found at:
  echo         %CM%
  echo         Edit the CM= line in this file if it was installed elsewhere.
  goto :step2
)

set "CM_RUN=0"
tasklist /FI "IMAGENAME eq cloudmusic.exe" 2>nul | find /I "cloudmusic.exe" >nul && set "CM_RUN=1"

if "%CM_RUN%"=="1" (
  echo [1/3] CloudMusic is running WITHOUT the debug flag.
  echo       Restarting it now - current playback WILL be interrupted.
  taskkill /IM cloudmusic.exe >nul 2>&1
  timeout /t 2 /nobreak >nul
  taskkill /F /IM cloudmusic.exe >nul 2>&1
  timeout /t 3 /nobreak >nul
) else (
  echo [1/3] CloudMusic not running -^> starting with debug flag.
)

echo       launching: "%CM%" --remote-debugging-port=9222
start "" "%CM%" --remote-debugging-port=9222
timeout /t 5 /nobreak >nul

curl -s -m 3 --noproxy "*" http://127.0.0.1:9222/json/version >nul 2>&1
if errorlevel 1 (
  echo       [WARN] CDP 9222 not reachable after 5s.
  echo              Playback position falls back to local interpolation;
  echo              the lyric anchor stays at the "track change" instant,
  echo              so lyrics may sit at a fixed offset for that song.
) else (
  echo       [ OK ] CDP 9222 up - real playback position available.
)

REM ===================================================================
REM  [2/3] pre-flight
REM ===================================================================
:step2
echo.
if not exist "%PY%" (
  echo [2/3] [ERR] venv python not found:
  echo             %PY%
  echo             Install it, or fix the PY= line in this file.
  popd
  pause
  exit /b 1
)
if not exist "%ROOT%\pc_host\main.py" (
  echo [2/3] [ERR] pc_host\main.py not found under:
  echo             %ROOT%
  popd
  pause
  exit /b 1
)
echo [2/3] pre-flight OK.

REM ===================================================================
REM  [3/3] pc_host
REM ===================================================================
echo [3/3] starting pc_host.main ...
echo.
echo       ------------------------------------------------------------
echo        extra args are forwarded, e.g.:   start_all.bat --device 192.168.4.1
echo        press Ctrl+C in this window to stop the host
echo       ------------------------------------------------------------
echo.

"%PY%" -m pc_host.main %*
set "RC=%ERRORLEVEL%"

echo.
echo ============================================================
echo   pc_host exited, code = %RC%
echo   leave this window open to read the log, or close it.
echo ============================================================
popd
pause
