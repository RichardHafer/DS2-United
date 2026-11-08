@echo off
:: Builds build\DS2United.asi (Dead Space 2 United): the randomizer and the co-op plugin in one module.
:: It replaces DS2RandomizerObserver.asi, the old co-op-only DS2United.asi and HaferSprawl.asi
:: (its name until 17.10.2026); keep exactly one of them in plugins.
:: build_observer.cmd and build_coop.cmd still build the halves on their own for tests.
setlocal
call "%~dp0tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0"
if not exist build\ds2coop mkdir build\ds2coop
if exist analysis\catalog\entities.jsonl (python tools\embed_enemy_catalog.py & if errorlevel 1 goto failed)
if exist analysis\catalog\entities.jsonl (python tools\embed_global_targets.py & if errorlevel 1 goto failed)
if exist analysis\catalog\entities.jsonl (python tools\embed_stream_requirements.py & if errorlevel 1 goto failed)
if exist analysis\catalog\entities.jsonl (python tools\embed_death_messages.py & if errorlevel 1 goto failed)
if exist analysis\catalog\entities.jsonl (python tools\embed_bench_catalog.py & if errorlevel 1 goto failed)
python tools\stamp_owner.py
if %errorlevel% neq 0 goto failed
cl /nologo /c /O2 /Z7 /W4 /EHsc /std:c++latest /MT /DNDEBUG /DDS2COOP_MODULE /Ithird_party src\observer.cpp src\coop_plugin.cpp src\ds2coop_main.cpp /Fobuild\ds2coop\
if %errorlevel% neq 0 goto failed
cl /nologo /c /O2 /Z7 /W4 /EHsc /std:c++latest /MT /DNDEBUG /Ithird_party third_party\safetyhook\safetyhook.cpp /Fobuild\ds2coop\
if %errorlevel% neq 0 goto failed
cl /nologo /c /O2 /Z7 /MT /Ithird_party third_party\safetyhook\Zydis.c /Fobuild\ds2coop\
if %errorlevel% neq 0 goto failed
link /nologo /DLL /DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF /PDB:build\DS2United.pdb /PDBALTPATH:DS2United.pdb /MAP:build\DS2United.map /OUT:build\DS2United.asi build\ds2coop\ds2coop_main.obj build\ds2coop\observer.obj build\ds2coop\coop_plugin.obj build\ds2coop\safetyhook.obj build\ds2coop\Zydis.obj kernel32.lib user32.lib ws2_32.lib
if %errorlevel% neq 0 goto failed
python tools\archive_coop_symbols.py
if %errorlevel% neq 0 goto failed
echo Built build\DS2United.asi
popd
exit /b 0
:failed
set result=%errorlevel%
if "%result%"=="0" set result=1
popd
exit /b %result%
