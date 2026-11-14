@echo off
setlocal
call "%~dp0..\tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0..\"
if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /W4 /WX /Febuild\mpnative_tests.exe /Fobuild\mpnative_tests.obj tests\mpnative_tests.cpp
if %errorlevel% neq 0 (popd & exit /b 1)
build\mpnative_tests.exe
set result=%errorlevel%
popd
exit /b %result%
