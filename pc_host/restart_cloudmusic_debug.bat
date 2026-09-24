@echo off
REM ============================================================
REM  Restart NetEase Cloud Music with CEF remote debugging ON.
REM  Purpose: P1 probe (.workbuddy\probe_cef_cdp.py) needs
REM           --remote-debugging-port=9222 to read currentTime.
REM  WARNING: this CLOSES the currently playing music.
REM
REM  DAILY USE: click the desktop "ESP32_Voice_Host.bat" instead -
REM             it calls pc_host\start_all.bat, which does this restart
REM             AND starts the host, and skips the restart entirely if
REM             9222 is already up. Keep this file for the times you
REM             only want to cycle CloudMusic alone.
REM
REM  (Pure ASCII on purpose: avoid codepage garbling.)
REM ============================================================
setlocal
set EXE=C:\Program Files\Netease\CloudMusic\cloudmusic.exe

if not exist "%EXE%" (
  echo [ERR] not found: %EXE%
  echo       Edit the EXE= line in this file to your install path.
  pause
  exit /b 1
)

echo [1/3] Closing cloudmusic.exe ...
taskkill /IM cloudmusic.exe >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /F /IM cloudmusic.exe >nul 2>&1
timeout /t 3 /nobreak >nul

echo [2/3] Starting with --remote-debugging-port=9222 ...
start "" "%EXE%" --remote-debugging-port=9222
timeout /t 5 /nobreak >nul

echo [3/3] Verifying http://127.0.0.1:9222/json/version ...
REM --noproxy is REQUIRED: Clash/TUN sets http_proxy and would otherwise
REM hijack the loopback probe, printing a bogus failure.
curl -s -m 3 --noproxy "*" http://127.0.0.1:9222/json/version
echo.
echo ------------------------------------------------------------
echo If JSON above looks like {"Browser":"Chrome/..."}  =^> OK
echo Then run the probe:
echo   C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe .workbuddy\probe_cef_cdp.py
echo If empty: this CEF build ignored the flag (P1 not viable).
echo ------------------------------------------------------------
pause
