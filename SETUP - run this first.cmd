@echo off
rem Builds the mod's Skate 3 data from your own copy of Skate 3 (Xbox 360 .iso or extracted game folder).
rem Run it once after unpacking the mod, and again if you move to a different copy of Skate 3.
rem You can also drag your Skate 3 .iso (or its default.xex) onto this file.
title FN Skate 3 setup
cd /d "%~dp0"
"%~dp0setup\python\python.exe" "%~dp0setup\fnskate_setup.py" %1
echo.
pause
