@echo off
setlocal
call "%~dp0tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0"
if not exist build mkdir build
rem The generated headers are rebuilt from the extracted game data when it is
rem present (analysis\catalog). Without it the committed headers are used as they are.
if exist analysis\catalog\entities.jsonl (python tools\embed_enemy_catalog.py & if errorlevel 1 exit /b 1)
if exist analysis\catalog\entities.jsonl (python tools\embed_global_targets.py & if errorlevel 1 exit /b 1)
if exist analysis\catalog\entities.jsonl (python tools\embed_stream_requirements.py & if errorlevel 1 exit /b 1)
if exist analysis\catalog\entities.jsonl (python tools\embed_death_messages.py & if errorlevel 1 exit /b 1)
if exist analysis\catalog\entities.jsonl (python tools\embed_bench_catalog.py & if errorlevel 1 exit /b 1)
cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /DNDEBUG /Ithird_party src\observer.cpp third_party\safetyhook\safetyhook.cpp /Fobuild\
if %errorlevel% neq 0 exit /b 1
cl /nologo /c /O2 /MT /Ithird_party third_party\safetyhook\Zydis.c /Fobuild\
if %errorlevel% neq 0 exit /b 1
link /nologo /DLL /OUT:build\DS2RandomizerObserver.asi build\observer.obj build\safetyhook.obj build\Zydis.obj kernel32.lib user32.lib
set result=%errorlevel%
popd
exit /b %result%
