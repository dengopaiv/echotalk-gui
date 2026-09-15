@echo off
rem Build the EchoTalk GUI: one self-contained exe over the library in src\.
rem
rem Usage:  gui-native\build.cmd [x64|arm64]      (default x64; there is no 32-bit build)
rem
rem Two compiler passes, as in the Votrax SC-01 ROM GUI. The library and the
rem vendored emulators compile first with their own, quieter flags -- they are
rem the port and third-party code, and their five MSVC warnings are known and
rem left alone (see notes\msvc_build.md). Only the GUI itself is held to /W4 /WX.
rem
rem The same objects also link tools\say.c into say-<arch>.exe, so the command
rem line the GUI's "Copy as say command line" produces can be run straight away.
rem
rem The Textalker images are NOT compiled in. See gui-native\README.md.
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
set "VCARCH="
if /i "%ARCH%"=="x64" set "VCARCH=x64"
if /i "%ARCH%"=="arm64" set "VCARCH=x64_arm64"
if not defined VCARCH (
    echo ERROR: architecture must be x64 or arm64. No 32-bit builds are made here.
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo ERROR: vswhere.exe not found - is Visual Studio installed?
    exit /b 1
)
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo ERROR: no MSVC C++ toolset found. Install "Desktop development with C++".
    exit /b 1
)

set "OUT=%HERE%build\obj-%ARCH%"
if not exist "%OUT%" mkdir "%OUT%"
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul || exit /b 1
cd /d "%OUT%"

echo === compiling the library and emulators (%ARCH%) ===
cl /nologo /c /std:c11 /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /I "%ROOT%\src" /I "%ROOT%\third_party\tms5220_core" ^
   "%ROOT%\src\echotalk.c" ^
   "%ROOT%\src\text_prep.c" ^
   "%ROOT%\src\chunker.c" ^
   "%ROOT%\src\resample.c" ^
   "%ROOT%\third_party\fake6502\fake6502.c" ^
   "%ROOT%\third_party\tms5220_core\tms5220_core.c" ^
   "%ROOT%\third_party\tms5220_core\tms5220_reset.c" || exit /b 1
set "LIBOBJS=echotalk.obj text_prep.obj chunker.obj resample.obj fake6502.obj tms5220_core.obj tms5220_reset.obj"

echo === compiling resources (%ARCH%) ===
rc /nologo /fo "%OUT%\echotalk_gui.res" /i "%HERE%." "%HERE%echotalk_gui.rc" || exit /b 1

echo === compiling the GUI and linking (%ARCH%) ===
cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /WX /DUNICODE /D_UNICODE ^
   /I "%ROOT%\src" /I "%HERE%." ^
   "%HERE%echotalk_gui.cpp" %LIBOBJS% ^
   /Fe:"%HERE%build\echotalk_gui-%ARCH%.exe" ^
   /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO "%OUT%\echotalk_gui.res" || exit /b 1

echo === linking say (%ARCH%) ===
cl /nologo /std:c11 /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /I "%ROOT%\src" "%ROOT%\tools\say.c" %LIBOBJS% ^
   /Fe:"%HERE%build\say-%ARCH%.exe" /link /INCREMENTAL:NO || exit /b 1

echo.
echo Done: %HERE%build\echotalk_gui-%ARCH%.exe
for %%F in ("%HERE%build\echotalk_gui-%ARCH%.exe") do echo Size: %%~zF bytes
exit /b 0
