@echo off
if exist "%~dp0starting_militia\armed-citizens-launcher.exe" (
    "%~dp0starting_militia\armed-citizens-launcher.exe" %*
) else (
    python "%~dp0starting_militia\launch.py" %*
)
if errorlevel 1 pause
