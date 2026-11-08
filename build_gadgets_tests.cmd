@echo off
setlocal
call "%~dp0tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /W4 /Febuild\coop_gadgets_tests.exe /Fobuild\coop_gadgets_tests.obj tests\coop_gadgets_tests.cpp
if %errorlevel% neq 0 exit /b 1
build\coop_gadgets_tests.exe
set result=%errorlevel%
popd
exit /b %result%
