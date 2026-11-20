@echo off
rem Build Jig Rack with the Visual Studio C++ tools: build\JigRack.exe
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
)
if not defined VS (
    echo Visual Studio with the C++ tools was not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /fo build\jigrack.res src\jigrack.rc || exit /b 1
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE /Fo:build\ src\jigrack.c build\jigrack.res /Fe:build\JigRack.exe /link /SUBSYSTEM:WINDOWS || exit /b 1
echo Built build\JigRack.exe
