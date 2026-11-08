@echo off
setlocal
call "%~dp0tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /W4 /Febuild\coop_client_progress_tests.exe /Fobuild\coop_client_progress_tests.obj tests\coop_client_progress_tests.cpp
if %errorlevel% neq 0 exit /b 1
build\coop_client_progress_tests.exe
set result=%errorlevel%
popd
exit /b %result%
