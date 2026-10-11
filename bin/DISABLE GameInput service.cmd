@echo off
rem Turns off Windows' built-in GameInput Service (the Windows 10 controller FPS-drop bug).
rem Right-click this file and choose "Run as administrator". Undo: "ENABLE GameInput service.cmd".
net session >nul 2>&1
if errorlevel 1 (
  echo Please right-click this file and choose "Run as administrator".
  pause
  exit /b 1
)
sc config GameInputSvc start= disabled
taskkill /F /IM GameInputSvc.exe >nul 2>&1
sc stop GameInputSvc
echo.
sc query GameInputSvc
echo.
echo Done. GameInput Service is off. Restart the PC, then play with the controller.
pause
