@echo off
rem Builds axon.exe (and axonc.exe) with MSVC. Usage: build.bat [debug]
setlocal
set ROOT=%~dp0
set SRC=%ROOT%axonc\src
set OUT=%ROOT%bin
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

where cl >nul 2>nul
if errorlevel 1 (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
)
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

set FLAGS=/nologo /std:c++20 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /O2 /MT
if "%1"=="debug" set FLAGS=/nologo /std:c++20 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Od /Zi /MTd

cl %FLAGS% /Fo"%OUT%\obj\\" /Fe"%OUT%\axon.exe" "%SRC%\*.cpp" /link /NOLOGO
if errorlevel 1 exit /b 1
copy /y "%OUT%\axon.exe" "%OUT%\axonc.exe" >nul
echo Built %OUT%\axon.exe
