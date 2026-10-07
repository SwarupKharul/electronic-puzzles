@echo off
title Escape Room Prop Hardware Simulator
color 0E
cls

echo.
echo  ================================================================
echo   ESCAPE ROOM UNIVERSAL PROP SIMULATOR
echo  ================================================================
echo.
echo   Simulates ESP32 hardware props without needing physical microcontrollers.
echo   Make sure the main server (start.bat) is running before using this!
echo.

where node >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    color 0C
    echo  [ERROR] Node.js is not found. Run start.bat first.
    pause
    exit /b 1
)

node test-simulator.js
pause
