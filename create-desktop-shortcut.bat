@echo off
title Create Escape Room Desktop Shortcut
color 0B
cls

echo.
echo  ================================================================
echo   CREATE ESCAPE ROOM DESKTOP SHORTCUT
echo  ================================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ws = New-Object -ComObject WScript.Shell; ^
   $desktop = [System.Environment]::GetFolderPath('Desktop'); ^
   $shortcutPath = Join-Path $desktop 'Escape Room Control.lnk'; ^
   $shortcut = $ws.CreateShortcut($shortcutPath); ^
   $shortcut.TargetPath = Join-Path '%~dp0' 'start.bat'; ^
   $shortcut.WorkingDirectory = '%~dp0'; ^
   $shortcut.Description = 'Escape Room Master Control System'; ^
   $shortcut.Save(); ^
   Write-Host ' [OK] Desktop shortcut created at:' $shortcutPath"

echo.
echo  ================================================================
echo   Done! You can now launch the system directly from your Desktop.
echo  ================================================================
echo.
pause
