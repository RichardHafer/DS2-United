@echo off
setlocal
title Dead Space 2, enable small crash dumps

rem For players who send crash reports. Makes Windows keep a small dump when
rem deadspace2.exe crashes: call stacks, the memory they point at and the
rem game's global data, instead of the full 900 MB image. Small enough to
rem send. The key lives under HKLM, so this asks for administrator rights.

set "KEY=HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\deadspace2.exe"
set "FOLDER=C:\DS2CrashDumps"
set "FIND=%SystemRoot%\System32\find.exe"
set "REG=%SystemRoot%\System32\reg.exe"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo No administrator rights. Windows will ask for them now.
    echo Please answer Yes, otherwise nothing happens.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo.
echo Writing the key ...
rem DumpType 0 = custom: data segments, unloaded modules, indirectly
rem referenced memory and thread info (0x1061).
"%REG%" add "%KEY%" /v DumpFolder /t REG_EXPAND_SZ /d "%FOLDER%" /f >nul
if %errorlevel% neq 0 goto failed
"%REG%" add "%KEY%" /v DumpType /t REG_DWORD /d 0 /f >nul
if %errorlevel% neq 0 goto failed
"%REG%" add "%KEY%" /v CustomDumpFlags /t REG_DWORD /d 0x1061 /f >nul
if %errorlevel% neq 0 goto failed
"%REG%" add "%KEY%" /v DumpCount /t REG_DWORD /d 5 /f >nul
if %errorlevel% neq 0 goto failed
if not exist "%FOLDER%" mkdir "%FOLDER%"

echo Reading it back ...
"%REG%" query "%KEY%" /v CustomDumpFlags 2>nul | "%FIND%" "0x1061" >nul
if %errorlevel% neq 0 goto failed
"%REG%" query "%KEY%" /v DumpFolder 2>nul | "%FIND%" /i "%FOLDER%" >nul
if %errorlevel% neq 0 goto failed

echo.
echo ============================================================
echo   DONE. From now on every Dead Space 2 crash lands here:
echo.
echo       %FOLDER%\deadspace2.exe.^<number^>.dmp
echo.
echo   At most five are kept. When the game crashes, zip the
echo   newest .dmp together with plugins\DS2Randomizer.log and
echo   send both.
echo ============================================================
echo.
pause
exit /b 0

:failed
echo.
echo ============================================================
echo   FAILED. The key could not be written or read back.
echo   Usually the Windows prompt was answered with No. Start
echo   this file again and click Yes.
echo ============================================================
echo.
pause
exit /b 1
