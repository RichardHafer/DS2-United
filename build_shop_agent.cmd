@echo off
:: The co-op plugin built into build\shop_agent\ (objects, DS2United.asi and its map),
:: so a build here never touches build\ or build\coop\.
setlocal
cd /d "%~dp0"

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo Visual Studio with the C++ toolset was not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if %errorlevel% neq 0 exit /b 1

if not exist build\shop_agent mkdir build\shop_agent

cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /DNDEBUG /Ithird_party src\coop_plugin.cpp /Fobuild\shop_agent\
if %errorlevel% neq 0 exit /b 1

cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /DNDEBUG /Ithird_party third_party\safetyhook\safetyhook.cpp /Fobuild\shop_agent\
if %errorlevel% neq 0 exit /b 1
cl /nologo /c /O2 /MT /Ithird_party third_party\safetyhook\Zydis.c /Fobuild\shop_agent\
if %errorlevel% neq 0 exit /b 1

link /nologo /DLL /MAP:build\shop_agent\DS2United.map /OUT:build\shop_agent\DS2United.asi build\shop_agent\coop_plugin.obj build\shop_agent\safetyhook.obj build\shop_agent\Zydis.obj kernel32.lib user32.lib ws2_32.lib
if %errorlevel% neq 0 exit /b 1

echo Built build\shop_agent\DS2United.asi
endlocal
