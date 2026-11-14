@echo off
setlocal
call "%~dp0..\tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0..\"
if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /W4 /Febuild\coop_death_ack_tests.exe /Fobuild\coop_death_ack_tests.obj tests\coop_death_ack_tests.cpp
if %errorlevel% neq 0 exit /b 1
build\coop_death_ack_tests.exe
set result=%errorlevel%
popd
exit /b %result%
