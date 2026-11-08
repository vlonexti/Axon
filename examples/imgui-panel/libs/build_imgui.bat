@echo off
rem Builds Dear ImGui + cimgui (its C API) + rlImGui (its raylib backend) into one static library:
rem   libs\imgui\lib\imgui.lib   and the C headers in   libs\imgui\include
rem Sources: imgui 1.92.7 (docking), cimgui 0e533fd, rlImGui tag Raylib_5_5.
setlocal
set HERE=%~dp0
set SRC=%HERE%imgui-src
set OUT=%HERE%imgui
if not exist "%OUT%\lib" mkdir "%OUT%\lib"
if not exist "%OUT%\include" mkdir "%OUT%\include"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

where cl >nul 2>nul
if errorlevel 1 (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
)
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

rem /MD matches raylib's prebuilt library and Axon's MSVC builds
cl /nologo /c /std:c++17 /EHsc /O2 /MD /W0 /DCIMGUI_NO_EXPORT /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS=1 ^
   /I"%SRC%\cimgui" /I"%SRC%\cimgui\imgui" /I"%SRC%\rlImGui" /I"%HERE%raylib\include" /Fo"%OUT%\obj\\" ^
   "%SRC%\cimgui\imgui\imgui.cpp" "%SRC%\cimgui\imgui\imgui_draw.cpp" "%SRC%\cimgui\imgui\imgui_tables.cpp" ^
   "%SRC%\cimgui\imgui\imgui_widgets.cpp" "%SRC%\cimgui\imgui\imgui_demo.cpp" "%SRC%\cimgui\cimgui.cpp" ^
   "%SRC%\rlImGui\rlImGui.cpp"
if errorlevel 1 exit /b 1
lib /nologo /OUT:"%OUT%\lib\imgui.lib" "%OUT%\obj\*.obj"
if errorlevel 1 exit /b 1
copy /y "%SRC%\cimgui\cimgui.h" "%OUT%\include\" >nul
copy /y "%SRC%\rlImGui\rlImGui.h" "%OUT%\include\" >nul
if not exist "%OUT%\include\extras" mkdir "%OUT%\include\extras"
copy /y "%SRC%\rlImGui\extras\IconsFontAwesome6.h" "%OUT%\include\extras\" >nul
rmdir /s /q "%OUT%\obj"
echo Built %OUT%\lib\imgui.lib
