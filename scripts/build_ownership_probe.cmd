@echo off
setlocal
call "%~dp0tools\vcvars32.cmd"
if errorlevel 1 exit /b 1
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /W4 /Febuild\ownership_dump_probe.exe /Fobuild\ownership_dump_probe.obj tools\ownership_dump_probe.cpp
set result=%errorlevel%
popd
exit /b %result%
