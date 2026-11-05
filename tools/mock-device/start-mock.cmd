@echo off
rem Double-click to start the local MOCK backend (fake data, never talks to a real robot dog).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mock-device.ps1" %*
pause
