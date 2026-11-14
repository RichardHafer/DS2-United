@echo off
:: Builds the standalone campaign co-op half (build\coop\DS2United.asi). The whole
:: mod, build\DS2United.asi, comes from build_ds2coop.cmd.
:: Separate target from build_observer.cmd: no randomizer module is compiled in.
setlocal
cd /d "%~dp0..\"

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo Visual Studio with the C++ toolset was not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if %errorlevel% neq 0 exit /b 1

if not exist build\coop mkdir build\coop

cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /DNDEBUG /Ithird_party src\coop_plugin.cpp /Fobuild\coop\
if %errorlevel% neq 0 exit /b 1

:: safetyhook and Zydis are shared sources; compiled into this target's own objects.
cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /DNDEBUG /Ithird_party third_party\safetyhook\safetyhook.cpp /Fobuild\coop\
if %errorlevel% neq 0 exit /b 1
cl /nologo /c /O2 /MT /Ithird_party third_party\safetyhook\Zydis.c /Fobuild\coop\
if %errorlevel% neq 0 exit /b 1

link /nologo /DLL /MAP:build\coop\DS2United.map /OUT:build\coop\DS2United.asi build\coop\coop_plugin.obj build\coop\safetyhook.obj build\coop\Zydis.obj kernel32.lib user32.lib ws2_32.lib
if %errorlevel% neq 0 exit /b 1

echo Built build\coop\DS2United.asi
endlocal
