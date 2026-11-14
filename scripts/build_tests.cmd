@echo off
setlocal
call "%~dp0..\tools\vcvars32.cmd"
if %errorlevel% neq 0 exit /b 1
pushd "%~dp0..\"
if not exist build mkdir build
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\policy_tests.cpp /Fobuild\policy_tests.obj /Febuild\policy_tests.exe
if %errorlevel% neq 0 exit /b 1
build\policy_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++latest /MT /Ithird_party tests\hook_site_tests.cpp /Fobuild\hook_site_tests.obj /Febuild\hook_site_tests.exe
if %errorlevel% neq 0 exit /b 1
build\hook_site_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\extra_spawns_tests.cpp /Fobuild\extra_spawns_tests.obj /Febuild\extra_spawns_tests.exe
if %errorlevel% neq 0 exit /b 1
build\extra_spawns_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\selection_tests.cpp /Fobuild\selection_tests.obj /Febuild\selection_tests.exe
if %errorlevel% neq 0 exit /b 1
build\selection_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\death_message_tests.cpp /Fobuild\death_message_tests.obj /Febuild\death_message_tests.exe
if %errorlevel% neq 0 exit /b 1
build\death_message_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\catalog_tests.cpp /Fobuild\catalog_tests.obj /Febuild\catalog_tests.exe
if %errorlevel% neq 0 exit /b 1
build\catalog_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\component_rebinding_tests.cpp /Fobuild\component_rebinding_tests.obj /Febuild\component_rebinding_tests.exe
if %errorlevel% neq 0 exit /b 1
build\component_rebinding_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\construction_binding_tests.cpp /Fobuild\construction_binding_tests.obj /Febuild\construction_binding_tests.exe
if %errorlevel% neq 0 exit /b 1
build\construction_binding_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\stream_registry_tests.cpp /Fobuild\stream_registry_tests.obj /Febuild\stream_registry_tests.exe
if %errorlevel% neq 0 exit /b 1
build\stream_registry_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\stream_states_tests.cpp /Fobuild\stream_states_tests.obj /Febuild\stream_states_tests.exe
if %errorlevel% neq 0 exit /b 1
build\stream_states_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\global_catalog_tests.cpp /Fobuild\global_catalog_tests.obj /Febuild\global_catalog_tests.exe
if %errorlevel% neq 0 exit /b 1
build\global_catalog_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\global_stream_lifetime_tests.cpp /Fobuild\global_stream_lifetime_tests.obj /Febuild\global_stream_lifetime_tests.exe
if %errorlevel% neq 0 exit /b 1
build\global_stream_lifetime_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\spawn_watchdog_tests.cpp /Fobuild\spawn_watchdog_tests.obj /Febuild\spawn_watchdog_tests.exe
if %errorlevel% neq 0 exit /b 1
build\spawn_watchdog_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\bench_policy_tests.cpp /Fobuild\bench_policy_tests.obj /Febuild\bench_policy_tests.exe
if %errorlevel% neq 0 exit /b 1
build\bench_policy_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\spawner_rules_tests.cpp /Fobuild\spawner_rules_tests.obj /Febuild\spawner_rules_tests.exe
if %errorlevel% neq 0 exit /b 1
build\spawner_rules_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\more_enemies_tests.cpp /Fobuild\more_enemies_tests.obj /Febuild\more_enemies_tests.exe
if %errorlevel% neq 0 exit /b 1
build\more_enemies_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\coop_dementia_tests.cpp /Fobuild\coop_dementia_tests.obj /Febuild\coop_dementia_tests.exe
if %errorlevel% neq 0 exit /b 1
build\coop_dementia_tests.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /MT tests\coop_difficulty_tests.cpp /Fobuild\coop_difficulty_tests.obj /Febuild\coop_difficulty_tests.exe
if %errorlevel% neq 0 exit /b 1
build\coop_difficulty_tests.exe
set result=%errorlevel%
popd
exit /b %result%
