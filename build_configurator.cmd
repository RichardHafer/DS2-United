@echo off
setlocal
rem Builds build\DS2UnitedConfig.exe with the C# compiler that ships with
rem Windows (.NET Framework 4.x). No SDK needed. Logo.png and App.ico come from
rem tools\RandomizerConfig\make_coop_assets.py. The repository's DS2United.ini,
rem DS2Survival.ini and DS2BotProfiles.ini are built in as templates, its
rem DS2Randomizer.ini only for the comments of keys Recommended.ini lacks.
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
    /out:build\DS2UnitedConfig.exe ^
    /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll ^
    /resource:%SRC%\Recommended.ini,DS2CoopConfig.Recommended.ini ^
    /resource:DS2United.ini,DS2CoopConfig.Template.DS2United.ini ^
    /resource:DS2Survival.ini,DS2CoopConfig.Template.DS2Survival.ini ^
    /resource:DS2BotProfiles.ini,DS2CoopConfig.Template.DS2BotProfiles.ini ^
    /resource:DS2Randomizer.ini,DS2CoopConfig.Notes.DS2Randomizer.ini ^
    /resource:%SRC%\Logo.png,DS2CoopConfig.Logo.png ^
    /resource:%SRC%\App.ico,DS2CoopConfig.App.ico ^
    /win32icon:%SRC%\App.ico /win32manifest:%SRC%\app.manifest ^
    %SRC%\RandomizerConfig.cs %SRC%\Model.cs %SRC%\Schema.cs %SRC%\Curated.cs %SRC%\Templates.cs %SRC%\Merge.cs %SRC%\Expert.cs ^
    %SRC%\IniDocument.cs %SRC%\Editors.cs %SRC%\Views.cs %SRC%\Theme.cs %SRC%\SelfTest.cs
set result=%errorlevel%
popd
exit /b %result%
