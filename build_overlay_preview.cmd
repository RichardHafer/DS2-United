@echo off
:: The supply terminal's panel drawn by its own code on a real Direct3D 9 device
:: (no game): BMP previews in build\overlay_preview\ and a state restore check.
setlocal
call "%~dp0tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0"
if not exist build\overlay_preview mkdir build\overlay_preview
cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /Ithird_party tests\coop_overlay_preview.cpp /Fobuild\overlay_preview\
if %errorlevel% neq 0 exit /b 1
cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /Ithird_party third_party\safetyhook\safetyhook.cpp /Fobuild\overlay_preview\
if %errorlevel% neq 0 exit /b 1
cl /nologo /c /O2 /MT /Ithird_party third_party\safetyhook\Zydis.c /Fobuild\overlay_preview\
if %errorlevel% neq 0 exit /b 1
link /nologo /OUT:build\overlay_preview\coop_overlay_preview.exe build\overlay_preview\coop_overlay_preview.obj build\overlay_preview\safetyhook.obj build\overlay_preview\Zydis.obj d3d9.lib user32.lib gdi32.lib kernel32.lib
if %errorlevel% neq 0 exit /b 1
build\overlay_preview\coop_overlay_preview.exe build\overlay_preview
set result=%errorlevel%
popd
exit /b %result%
