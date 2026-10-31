@echo off
rem build-ced.bat - build vendored compact_enc_det (Apache-2.0, no cmake).
rem Object cache: bin\obj-ced\{compact_enc_det,compact_enc_det_hint_code,encodings,languages}.obj
rem Delete that folder or set CED_REBUILD=1 to force a rebuild.
setlocal
set "MSVC_VER=14.44.35207"
set "SDK_VER=10.0.26100.0"
set "VS_ROOT=C:\VS2022"
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"

set "PROJ=%~dp0"
set "MSVC=%VS_ROOT%\VC\Tools\MSVC\%MSVC_VER%"
set "SDKINC=%SDK_ROOT%\Include\%SDK_VER%"
set "CL=%MSVC%\bin\Hostx64\x64\cl.exe"
set "CED=%PROJ%third_party\compact_enc_det"

if not exist "%CL%" echo   missing cl.exe & exit /b 1
if not exist "%CED%\compact_enc_det\compact_enc_det.h" echo   missing third_party\compact_enc_det & exit /b 1
if exist "bin\obj-ced\.stamp" if not "%CED_REBUILD%"=="1" (
  echo   cached
  exit /b 0
)

set "INCLUDE=%MSVC%\include;%SDKINC%\ucrt;%SDKINC%\shared;%SDKINC%\um"
if not exist "bin\obj-ced" mkdir "bin\obj-ced"

rem /bigobj: compact_enc_det.cc pulls in ~720 KB of generated tables (one huge TU).
rem /W0: third-party code; CED is not warning-clean and we do not patch it.
set FILES=compact_enc_det\compact_enc_det compact_enc_det\compact_enc_det_hint_code util\encodings\encodings util\languages\languages

for %%F in (%FILES%) do (
  echo   %%F
  "%CL%" /nologo /c /MT /O2 /W0 /EHsc /bigobj /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DNOMINMAX /I "%CED%" /Fo"bin\obj-ced\%%~nxF.obj" "%CED%\%%F.cc" || exit /b 1
)

echo done> "bin\obj-ced\.stamp"
echo   built 4 objects
exit /b 0
