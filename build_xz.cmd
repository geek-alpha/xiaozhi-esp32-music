@echo off
setlocal
cd /d "%~dp0"

rem One-click build/flash for Xingzhi Cube 1.54 (WiFi), board xingzhi-cube-1.54tft-wifi
rem usage: build_xz.cmd           -> build only
rem        build_xz.cmd COM3      -> build, then flash and open serial monitor

set SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.xz-music.defaults

where idf.py >nul 2>&1
if errorlevel 1 (
    echo [x] No ESP-IDF in this window.
    echo     Open "ESP-IDF 5.4 CMD" from the Start menu, then run this script inside it.
    exit /b 1
)

if not exist sdkconfig (
    echo [1/2] First-time config: target=esp32s3, board=xingzhi-cube-1.54tft-wifi
    idf.py set-target esp32s3 || exit /b 1
)

echo [2/2] Build
idf.py build || exit /b 1

if "%~1"=="" (
    echo.
    echo Build OK. To flash: build_xz.cmd COM3
    exit /b 0
)

echo Flash to %~1
idf.py -p %~1 flash monitor
