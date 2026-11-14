@echo off
setlocal
call "%~dp0tools\vcvars32.cmd"
if errorlevel 1 exit /b 1
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /W4 /Febuild\coop_template_tests.exe /Fobuild\coop_template_tests.obj tests\coop_template_tests.cpp
if errorlevel 1 exit /b 1
build\coop_template_tests.exe
set result=%errorlevel%
popd
exit /b %result%
