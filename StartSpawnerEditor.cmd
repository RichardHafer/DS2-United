@echo off
cd /d "%~dp0"
python tools\spawner_editor.py
if errorlevel 1 pause
