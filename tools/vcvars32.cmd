@echo off
rem Finds the Visual Studio 2022 x86 build environment, whichever edition is
rem installed, and loads it. Called by every build_*.cmd next to this folder.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem VS's own vcvars scripts call vswhere.exe by name; without it on PATH they
rem print "vswhere.exe" not found on every build.
if exist "%VSWHERE%" for %%d in ("%VSWHERE%") do set "PATH=%%~dpd;%PATH%"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR if exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" (
  call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
  exit /b %errorlevel%
)
for %%e in (Professional Community Enterprise BuildTools) do (
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars32.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars32.bat" >nul
    exit /b %errorlevel%
  )
)
echo Visual Studio 2022 with the C++ x86 tools was not found.
exit /b 1
