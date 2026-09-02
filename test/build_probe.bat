@echo off
setlocal
set "VCVARS="
for %%d in ("%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools" "%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools") do (
    if not defined VCVARS if exist "%%~d\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%%~d\VC\Auxiliary\Build\vcvarsall.bat"
)
if not defined VCVARS (
    echo vcvarsall.bat not found
    exit /b 1
)
call "%VCVARS%" x86 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /EHsc /O2 /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Fo:out\ /Fe:out\cybertalk_probe.exe cybertalk_probe.cpp ..\src\stl_engine.cpp ole32.lib user32.lib advapi32.lib shell32.lib
exit /b %errorlevel%
