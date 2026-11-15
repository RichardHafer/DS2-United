@echo off
rem The United edition of MarkerPatch: MarkerPatch 3.2.0 with third_party\MarkerPatch-United\united.patch.
rem Output: build\markerpatch-united\dinput8.dll (the release packer takes it from there).
setlocal
pushd "%~dp0"
set "WORK=build\markerpatch-united"
set "SRC=%WORK%\src"
if not exist "%SRC%\.git" (
    git clone --quiet --depth 1 --branch 3.2.0 https://github.com/Wemino/MarkerPatch "%SRC%"
    if errorlevel 1 goto :fail
)
git -C "%SRC%" checkout --quiet -- .
git -C "%SRC%" apply --ignore-whitespace "%~dp0third_party\MarkerPatch-United\united.patch"
if errorlevel 1 goto :fail
python -I tools\embed_markerpatch_ini.py third_party\MarkerPatch-United\MarkerPatch.ini "%SRC%\src\united_default_ini.h"
if errorlevel 1 goto :fail
for /f "usebackq delims=" %%m in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%m"
if not defined MSBUILD goto :fail
"%MSBUILD%" "%SRC%\MarkerPatch.sln" -p:Configuration=Release -p:Platform=x86 -m:4 -nologo -v:minimal
if errorlevel 1 goto :fail
copy /y "%SRC%\bin\Release\dinput8.dll" "%WORK%\dinput8.dll" >nul
echo Built %WORK%\dinput8.dll
popd
exit /b 0
:fail
echo MarkerPatch United edition build FAILED
popd
exit /b 1
