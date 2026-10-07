@echo off
title Escape Room Control System - Launcher
color 0A
cls

echo.
echo  ================================================================
echo   [!] ESCAPE ROOM CONTROL SYSTEM - One-Click Launcher
echo  ================================================================
echo.

:: -------------------------------------------------------------------
:: 1. CHECK NODE.JS INSTALLATION
:: -------------------------------------------------------------------
where node >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    color 0E
    echo  [!] Node.js was not detected in system PATH.
    echo.
    where winget >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        echo  [*] Windows Package Manager (winget) is detected!
        echo  [*] Attempting automatic installation of Node.js LTS...
        echo.
        winget install -e --id OpenJS.NodeJS.LTS --accept-package-agreements --accept-source-agreements
        if %ERRORLEVEL% EQU 0 (
            color 0A
            echo.
            echo  ================================================================
            echo   [OK] Node.js has been installed successfully!
            echo   Please CLOSE this window and DOUBLE-CLICK start.bat again
            echo   so Windows refreshes your system PATH environment.
            echo  ================================================================
            echo.
            pause
            exit /b 0
        )
    )

    color 0C
    echo  ================================================================
    echo   [ERROR] Node.js is required to run the Control Server.
    echo  ================================================================
    echo.
    echo   Quick Manual Install:
    echo     1. Download Node.js LTS from: https://nodejs.org
    echo     2. Run installer (leave all defaults checked, including PATH)
    echo     3. Double-click start.bat again once installed.
    echo.
    echo  Opening Node.js download page for you...
    start "" "https://nodejs.org/en/download/"
    echo.
    pause
    exit /b 1
)

:: -------------------------------------------------------------------
:: 2. DISPLAY NODE ENVIRONMENT
:: -------------------------------------------------------------------
echo  [OK] Node.js is installed:
call node --version
echo.

:: -------------------------------------------------------------------
:: 3. INSTALL NPM PACKAGES (First run only)
:: -------------------------------------------------------------------
if not exist "node_modules\" (
    echo  [*] First-time setup detected: Installing required dependencies...
    echo  [*] This takes about 1-2 minutes. Please wait...
    echo.
    call npm install
    if %ERRORLEVEL% NEQ 0 (
        color 0C
        echo.
        echo  [ERROR] 'npm install' failed.
        echo  Please verify your internet connection and try running start.bat again.
        echo.
        pause
        exit /b 1
    )
    echo.
    echo  [OK] Dependencies installed successfully!
    echo.
) else (
    echo  [OK] Dependencies are already installed.
)

:: -------------------------------------------------------------------
:: 4. VERIFY FRONTEND BUILD
:: -------------------------------------------------------------------
if not exist "dist\index.html" (
    echo  [*] Frontend bundle not found. Building dashboard...
    echo.
    call npm run build
    if %ERRORLEVEL% NEQ 0 (
        color 0C
        echo.
        echo  [ERROR] Dashboard build failed.
        pause
        exit /b 1
    )
    echo.
    echo  [OK] Dashboard built successfully.
    echo.
) else (
    echo  [OK] Production dashboard bundle ready.
)

:: -------------------------------------------------------------------
:: 5. WINDOWS FIREWALL CONFIGURATION (Allow TCP 3000, 1883, 1884)
:: -------------------------------------------------------------------
echo.
echo  [*] Checking Windows Firewall rules for LAN/Tablet/ESP32 access...
netsh advfirewall firewall show rule name="EscapeRoom-Control-Ports" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo  [*] Adding firewall rule: TCP Ports 3000, 1883, 1884...
    echo  [*] (If Windows asks for Administrator permission, please click YES)
    powershell -Command "Start-Process netsh -ArgumentList 'advfirewall firewall add rule name=\"EscapeRoom-Control-Ports\" dir=in action=allow protocol=TCP localport=3000,1883,1884 profile=any' -Verb RunAs" >nul 2>&1
    echo  [OK] Windows Firewall rule configured.
) else (
    echo  [OK] Windows Firewall rule already active.
)

:: -------------------------------------------------------------------
:: 6. LAUNCH SERVER & OPEN BROWSER
:: -------------------------------------------------------------------
echo.
echo  ================================================================
echo   STARTING ESCAPE ROOM CONTROL SERVER
echo  ================================================================
echo.
echo   * Host Laptop URL      : http://localhost:3000
echo   * Tablet / Remote URL  : See IP address printed below
echo   * mDNS Hostname        : http://escaperoom.local:3000
echo.
echo   Opening dashboard in default browser in 2 seconds...
echo   To STOP the server at any time, press [Ctrl + C] in this window.
echo  ================================================================
echo.

:: Launch browser in background after short delay
start "" cmd /c "timeout /t 2 /nobreak >nul && start http://localhost:3000"

:: Start the Node.js server (runs in foreground)
node index.js

echo.
echo  Control server stopped.
pause
