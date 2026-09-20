@echo off
setlocal
call "%~dp0tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\schematic_policy_tests.cpp /Fobuild\schematic_policy_tests.obj /Febuild\schematic_policy_tests.exe
if %errorlevel% neq 0 exit /b 1
build\schematic_policy_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /c /W4 /WX /EHsc /std:c++latest /MT tests\schematic_runtime_compile.cpp /Fobuild\schematic_runtime_compile.obj
set result=%errorlevel%
popd
exit /b %result%
