@echo off
setlocal EnableDelayedExpansion

echo CyberTalk SAPI5 build
echo.

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD_X86=%ROOT%\build_x86"
set "BUILD_X64=%ROOT%\build_x64"
set "OUT=%ROOT%\output"
set "LOG=%ROOT%\build.log"

set "VCVARS="
for %%d in ("%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools" "%ProgramFiles%\Microsoft Visual Studio\2022\Community" "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools") do (
    if not defined VCVARS if exist "%%~d\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%%~d\VC\Auxiliary\Build\vcvarsall.bat"
)
if not defined VCVARS (
    echo ERROR: Visual Studio 2019/2022 with the C++ tools was not found.
    exit /b 1
)

set "ISCC="
for %%d in ("%LOCALAPPDATA%\Programs\Inno Setup 6" "%ProgramFiles(x86)%\Inno Setup 6" "%ProgramFiles%\Inno Setup 6") do (
    if not defined ISCC if exist "%%~d\ISCC.exe" set "ISCC=%%~d\ISCC.exe"
)

echo Using %VCVARS%
if defined ISCC (echo Using %ISCC%) else (echo Inno Setup not found - installer will be skipped)
echo.

echo === x86 build (host, SAPI DLL, configuration utility, tools) ===
call "%VCVARS%" x86 >nul
cmake -A Win32 -S "%ROOT%" -B "%BUILD_X86%" || exit /b 1
cmake --build "%BUILD_X86%" --config Release || exit /b 1

echo === x64 build (SAPI DLL) ===
call "%VCVARS%" x64 >nul
cmake -A x64 -S "%ROOT%" -B "%BUILD_X64%" || exit /b 1
cmake --build "%BUILD_X64%" --config Release || exit /b 1

echo === staging %OUT% ===
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" "%OUT%\x64" "%OUT%\engine" "%OUT%\tools"
copy /Y "%BUILD_X86%\bin\Release\CyberTalkHost.exe" "%OUT%\" >nul || exit /b 1
copy /Y "%BUILD_X86%\bin\Release\CyberTalkSAPI.dll" "%OUT%\" >nul || exit /b 1
copy /Y "%BUILD_X86%\bin\Release\CyberTalkConfig.exe" "%OUT%\" >nul || exit /b 1
copy /Y "%BUILD_X64%\bin\Release\CyberTalkSAPI.dll" "%OUT%\x64\" >nul || exit /b 1
copy /Y "%BUILD_X86%\bin\Release\cybertalk_probe.exe" "%OUT%\tools\" >nul
copy /Y "%BUILD_X86%\bin\Release\sapi_test.exe" "%OUT%\tools\sapi_test_x86.exe" >nul
copy /Y "%BUILD_X64%\bin\Release\sapi_test.exe" "%OUT%\tools\sapi_test_x64.exe" >nul
for %%f in (STLTTS.EXE TTSAPI.DLL MSAMPLES.DLL FSAMPLES.DLL NSAMPLES.DLL) do (
    copy /Y "%ROOT%\bin\%%f" "%OUT%\engine\" >nul || exit /b 1
)
copy /Y "%ROOT%\README.md" "%OUT%\README.txt" >nul
copy /Y "%ROOT%\LICENSE" "%OUT%\LICENSE.txt" >nul
copy /Y "%ROOT%\CREDITS.md" "%OUT%\CREDITS.txt" >nul

if defined ISCC (
    echo === installer ===
    "%ISCC%" /O"%OUT%" "%ROOT%\installer\cybertalk.iss" || exit /b 1
    echo.
    echo Installer: %OUT%\CyberTalkSAPI_Setup.exe
)
echo Build completed successfully.
endlocal
