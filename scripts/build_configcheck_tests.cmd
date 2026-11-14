@echo off
setlocal
call "%~dp0..\tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0..\"
if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /W4 /WX /Febuild\coop_configcheck_tests.exe /Fobuild\coop_configcheck_tests.obj tests\coop_configcheck_tests.cpp
if %errorlevel% neq 0 (popd & exit /b 1)
build\coop_configcheck_tests.exe "D:\SteamLibrary\steamapps\common\Dead Space 2\plugins\DS2United.ini"
set result=%errorlevel%
popd
exit /b %result%
