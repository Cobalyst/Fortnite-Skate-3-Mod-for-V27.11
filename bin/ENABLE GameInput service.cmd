@echo off
rem Puts Windows' built-in GameInput Service back the way it was (starts when needed).
rem Right-click this file and choose "Run as administrator".
net session >nul 2>&1
if errorlevel 1 (
  echo Please right-click this file and choose "Run as administrator".
  pause
  exit /b 1
)
sc config GameInputSvc start= demand
echo Done. GameInput Service is back to Manual (Windows starts it when needed).
pause
