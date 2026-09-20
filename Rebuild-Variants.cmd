@echo off
rem Rebuilds the variant pool, the skin model streams, the generated headers and the plugin.
rem Run after editing variants\ds2_variants.json or a skin's DDS files.
cd /d "%~dp0"
python tools\build_variants.py --write || goto :fail
python tools\verify_skin_pool.py || goto :fail
python tools\embed_stream_requirements.py || goto :fail
call "%~dp0build_observer.cmd" || goto :fail
echo.
echo Done. Copy build\DS2RandomizerObserver.asi to the game's plugins folder and
echo analysis\runtime_stream\ds2_randomizer_*.str and .toc next to deadspace2.exe (game closed).
exit /b 0
:fail
echo.
echo FAILED, nothing installed.
exit /b 1
