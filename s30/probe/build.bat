@echo off
REM Builds s30\probe\probe.dll (x64 Release) with the newest Visual Studio found by vswhere.
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSROOT=%%i
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
set DET=..\..\src\thirdparty\detours\src
cl /nologo /O2 /MT /EHsc /W3 /FI"%~dp0prefix.h" /DWIN32_LEAN_AND_MEAN /D_WINSOCK_DEPRECATED_NO_WARNINGS /Foobj\ ^
	probe.cpp %DET%\detours.cpp %DET%\disasm.cpp %DET%\modules.cpp ^
	/LD /Fe:probe.dll /link /DEBUG /PDB:probe.pdb ws2_32.lib winhttp.lib wininet.lib psapi.lib user32.lib advapi32.lib
exit /b %ERRORLEVEL%
