@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /LD shim.c /link /DEF:shim.def /OUT:oo2core_9_win64.dll "F:\apex_builds\m2299605425033557595\tools_src\rsx\src\thirdparty\oodle\oo2core_x64.lib"
