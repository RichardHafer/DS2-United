@echo off
rem Builds the enabled variant packs (variants\packs) into the pool and the model stream,
rem then the plugin. Run after changing a pack. See variants\README.md.
cd /d "%~dp0..\"
python -I tools\variant_pack.py check || goto :fail
python -I tools\variant_pack.py build --write || goto :fail
call "%~dp0..\build_ds2coop.cmd"
if %errorlevel% neq 0 goto :fail
echo.
echo Done. With the game closed: build\DS2United.asi into plugins\, and
echo analysis\runtime_stream\ds2_randomizer_pool.str/.toc and ds2_randomizer_models.str/.toc
echo into the game's randomizer\ folder.
exit /b 0
:fail
echo.
echo FAILED, nothing installed.
exit /b 1
