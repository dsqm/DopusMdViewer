@echo off
rem build-pcre2.bat - build vendored PCRE2 10.45 for both widths (static, JIT).
rem Object cache: build\obj-pcre2\{8,16,common}. Delete that folder to force a rebuild.
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
set "PCRE=%PROJ%third_party\pcre2"

if not exist "%CL_EXE%" echo   missing cl.exe & exit /b 1
if not exist "%PCRE%\include\pcre2.h" echo   missing third_party\pcre2 & exit /b 1
rem Cache: build\obj-pcre2 is only reused while it is newer than every
rem input (sources, headers, config.h); otherwise an edited PCRE2 config or
rem source silently keeps the old objects. PCRE2_REBUILD=1 forces a rebuild.
set "STAMP=build\obj-pcre2\16\.stamp"
set "PCRE2_CACHED="
if exist "%STAMP%" if not "%PCRE2_REBUILD%"=="1" set "PCRE2_CACHED=1"
if defined PCRE2_CACHED (
  setlocal enabledelayedexpansion
  for %%S in ("%STAMP%") do set "STAMPTIME=%%~tS"
  for %%F in ("%PCRE%\src\*.c" "%PCRE%\src\*.h" "%PCRE%\include\*.h") do (
    if not defined STALE if "%%~tF" GTR "!STAMPTIME!" set "STALE=1"
  )
  if not defined STALE (
    endlocal
    echo   cached
    exit /b 0
  )
  endlocal
  echo   cached objects are older than the sources, rebuilding
)

set "INCLUDE=%MSVC%\include;%SDKINC%\ucrt;%SDKINC%\shared;%SDKINC%\um;%PCRE%\include;%PCRE%\src"
if not exist "build\obj-pcre2" mkdir "build\obj-pcre2"
if not exist "build\obj-pcre2\chartables.c" copy /y "%PCRE%\src\pcre2_chartables.c.dist" "build\obj-pcre2\chartables.c" >nul

rem pcre2_jit_match/misc 由 pcre2_jit_compile.c 包含，pcre2_ucptables 由 pcre2_tables.c 包含，
rem 不能单独编译；pcre2_chkdint 只有宽 8/16 都用同一份符号，单独编一次。
set FILES=pcre2_auto_possess pcre2_compile pcre2_compile_class pcre2_config pcre2_context pcre2_convert pcre2_dfa_match pcre2_error pcre2_extuni pcre2_find_bracket pcre2_jit_compile pcre2_maketables pcre2_match pcre2_match_data pcre2_newline pcre2_ord2utf pcre2_pattern_info pcre2_script_run pcre2_serialize pcre2_string_utils pcre2_study pcre2_substitute pcre2_substring pcre2_tables pcre2_ucd pcre2_valid_utf pcre2_xclass

for %%W in (8 16) do (
  if not exist "build\obj-pcre2\%%W" mkdir "build\obj-pcre2\%%W"
  for %%F in (%FILES%) do (
    "%CL_EXE%" /nologo /c /MT /O2 /W0 /D_CRT_SECURE_NO_WARNINGS /DPCRE2_STATIC /DHAVE_CONFIG_H /DPCRE2_CODE_UNIT_WIDTH=%%W /I "%PCRE%\include" /I "%PCRE%\src" /Fo"build\obj-pcre2\%%W\%%F.obj" "%PCRE%\src\%%F.c" || exit /b 1
  )
  "%CL_EXE%" /nologo /c /MT /O2 /W0 /D_CRT_SECURE_NO_WARNINGS /DPCRE2_STATIC /DHAVE_CONFIG_H /DPCRE2_CODE_UNIT_WIDTH=%%W /I "%PCRE%\include" /I "%PCRE%\src" /Fo"build\obj-pcre2\%%W\chartables.obj" "build\obj-pcre2\chartables.c" || exit /b 1
)

if not exist "build\obj-pcre2\common" mkdir "build\obj-pcre2\common"
"%CL_EXE%" /nologo /c /MT /O2 /W0 /D_CRT_SECURE_NO_WARNINGS /DPCRE2_STATIC /DHAVE_CONFIG_H /DPCRE2_CODE_UNIT_WIDTH=8 /I "%PCRE%\include" /I "%PCRE%\src" /Fo"build\obj-pcre2\common\pcre2_chkdint.obj" "%PCRE%\src\pcre2_chkdint.c" || exit /b 1

echo done> "build\obj-pcre2\16\.stamp"
echo   built width 8 + 16
exit /b 0
