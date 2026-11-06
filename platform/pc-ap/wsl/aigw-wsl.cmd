@echo off
rem Starts AIGW "PC as AP" on Windows through WSL2. Pass -Stop to stop it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0aigw-wsl.ps1" %*
