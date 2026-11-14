@echo off
cd /d "%~dp0..\"
python tools\coop_events_live.py %*
pause
