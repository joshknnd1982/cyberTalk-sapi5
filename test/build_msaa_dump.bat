@echo off
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "HERE=%~dp0"
if not exist "%HERE%out" mkdir "%HERE%out"
call "%VCVARS%" x64 >nul
cl /nologo /EHsc /O1 /MT /Fo"%HERE%out\\" /Fe"%HERE%out\msaa_dump.exe" "%HERE%msaa_dump.cpp" || exit /b 1
echo built
