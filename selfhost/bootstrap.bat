@echo off
rem bootstrap.bat - proves the self-hosted compiler compiles itself.
rem   stage 0: the C++ compiler (axon) builds axs from axs.ax
rem   stage 1: axs compiles axs.ax to C -> axs1.exe
rem   stage 2: axs1 compiles axs.ax again -> axs2.c, which must equal axs1.c
setlocal
cd /d "%~dp0"
set AXON=%~dp0..\bin\axon.exe
if not exist build mkdir build
copy /y axs_rt.h build\ >nul

where cl >nul 2>nul
if errorlevel 1 (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
)
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

echo [stage 0] axon builds axs
"%AXON%" build axs.ax || exit /b 1

echo [stage 1] axs compiles itself
build\axs.exe axs.ax build\axs1.c || exit /b 1
cl /nologo /std:c11 /w /O2 build\axs1.c /Fe:build\axs1.exe /Fo:build\axs1.obj >nul || exit /b 1

echo [stage 2] the result compiles axs again
build\axs1.exe axs.ax build\axs2.c || exit /b 1
fc /b build\axs1.c build\axs2.c >nul
if errorlevel 1 (
    echo FAILED: stage 1 and stage 2 output differ
    exit /b 1
)
echo OK: axs1.c and axs2.c are identical - axs compiles itself
