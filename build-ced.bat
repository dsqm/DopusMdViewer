@echo off
rem build-ced.bat - build vendored compact_enc_det (Apache-2.0, no cmake).
rem Object cache: build\obj-ced\{compact_enc_det,compact_enc_det_hint_code,encodings,languages}.obj
rem Delete that folder or set CED_REBUILD=1 to force a rebuild.
setlocal
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_ROOT="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%i"
)
if not defined VS_ROOT echo   vswhere found no Visual Studio with C++ tools & exit /b 1

rem pick the newest installed MSVC and Windows SDK
set "MSVC_VER="
for /f "delims=" %%v in ('dir /b /ad "%VS_ROOT%\VC\Tools\MSVC"') do set "MSVC_VER=%%v"
set "SDK_VER="
for /f "delims=" %%v in ('dir /b /ad "%SDK_ROOT%\Include" ^| findstr /r "^10\." ') do set "SDK_VER=%%v"
set "PROJ=%~dp0"
set "MSVC=%VS_ROOT%\VC\Tools\MSVC\%MSVC_VER%"
set "SDKINC=%SDK_ROOT%\Include\%SDK_VER%"
set "CL_EXE=%MSVC%\bin\Hostx64\x64\cl.exe"
set "CED=%PROJ%third_party\compact_enc_det"

if not exist "%CL_EXE%" echo   missing cl.exe & exit /b 1
if not exist "%CED%\compact_enc_det\compact_enc_det.h" echo   missing third_party\compact_enc_det & exit /b 1
if exist "build\obj-ced\.stamp" if not "%CED_REBUILD%"=="1" (
  echo   cached
  exit /b 0
)

set "INCLUDE=%MSVC%\include;%SDKINC%\ucrt;%SDKINC%\shared;%SDKINC%\um"
if not exist "build\obj-ced" mkdir "build\obj-ced"

rem /bigobj: compact_enc_det.cc pulls in ~720 KB of generated tables (one huge TU).
rem /W0: third-party code; CED is not warning-clean and we do not patch it.
set FILES=compact_enc_det\compact_enc_det compact_enc_det\compact_enc_det_hint_code util\encodings\encodings util\languages\languages

for %%F in (%FILES%) do (
  echo   %%F
  "%CL_EXE%" /nologo /c /MT /O2 /W0 /EHsc /bigobj /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DNOMINMAX /I "%CED%" /Fo"build\obj-ced\%%~nxF.obj" "%CED%\%%F.cc" || exit /b 1
)

echo done> "build\obj-ced\.stamp"
echo   built 4 objects
exit /b 0
