@echo off
:: Builds the loose-file mod loader (DS2ModLoader.asi). No hooks, no safetyhook:
:: it only calls the filesystem manager's own SetSearchPath on new storage.
setlocal
cd /d "%~dp0..\"

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo Visual Studio with the C++ toolset was not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if %errorlevel% neq 0 exit /b 1

if not exist build\modloader mkdir build\modloader

cl /nologo /c /O2 /W4 /EHsc /std:c++latest /MT /DNDEBUG src\modloader.cpp /Fobuild\modloader\
if %errorlevel% neq 0 exit /b 1

link /nologo /DLL /MAP:build\DS2ModLoader.map /OUT:build\DS2ModLoader.asi build\modloader\modloader.obj kernel32.lib
if %errorlevel% neq 0 exit /b 1

echo Built build\DS2ModLoader.asi
endlocal
