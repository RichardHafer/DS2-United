@echo off
setlocal
rem Builds build\DS2RandomizerConfig.exe with the C# compiler that ships with
rem Windows (.NET Framework 4.x). No SDK needed. Logo.png and App.ico come from
rem tools\RandomizerConfig\make_assets.py.
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo csc.exe from .NET Framework 4 not found
    exit /b 1
)
pushd "%~dp0"
if not exist build mkdir build
set "SRC=tools\RandomizerConfig"
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ /warn:4 /warnaserror+ ^
    /out:build\DS2RandomizerConfig.exe ^
    /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll ^
    /resource:%SRC%\Recommended.ini,DS2RandomizerConfig.Recommended.ini ^
    /resource:%SRC%\Logo.png,DS2RandomizerConfig.Logo.png ^
    /resource:%SRC%\App.ico,DS2RandomizerConfig.App.ico ^
    /win32icon:%SRC%\App.ico /win32manifest:%SRC%\app.manifest ^
    %SRC%\RandomizerConfig.cs %SRC%\Model.cs %SRC%\Schema.cs %SRC%\IniDocument.cs %SRC%\Editors.cs %SRC%\Views.cs %SRC%\Theme.cs %SRC%\SelfTest.cs
set result=%errorlevel%
popd
exit /b %result%
