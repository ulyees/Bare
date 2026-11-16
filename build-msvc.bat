@echo off
rem Build Bare with MSVC (static CRT, standalone exe).
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
  echo [x] MSVC not found. Install Visual Studio Build Tools with C++ workload.
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
echo [1/2] resources...
rc /nologo /fo app_res.res app.rc
if errorlevel 1 exit /b 1
echo [2/2] compile Bare.exe...
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 Bare.cpp app_res.res /link /SUBSYSTEM:WINDOWS /OUT:Bare.exe user32.lib gdi32.lib comdlg32.lib dwmapi.lib shell32.lib advapi32.lib
if errorlevel 1 exit /b 1
echo OK: Bare.exe
