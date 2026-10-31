@echo off
rem build-pcre2.bat - build vendored PCRE2 10.45 for both widths (static, JIT).
rem Object cache: bin\obj-pcre2\{8,16,common}. Delete that folder to force a rebuild.
setlocal
set "MSVC_VER=14.44.35207"
set "SDK_VER=10.0.26100.0"
set "VS_ROOT=C:\VS2022"
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"

set "PROJ=%~dp0"
set "MSVC=%VS_ROOT%\VC\Tools\MSVC\%MSVC_VER%"
set "SDKInC=%SDK_ROOT%\Include\%SDK_VER%"
set "CL=%MSVC%\bin\Hostx64\x64\cl.exe"
set "PCRE=%PROJ%third_party\pcre2"

if not exist "%CL%" echo   missing cl.exe & exit /b 1
if not exist "%PCRE%\include\pcre2.h" echo   missing third_party\pcre2 & exit /b 1
if exist "bin\obj-pcre2\16\.stamp" if not "%PCRE2_REBUILD%"=="1" (
  echo   cached
  exit /b 0
)

set "InCLUDE=%MSVC%\include;%SDKInC%\ucrt;%SDKInC%\shared;%SDKInC%\um;%PCRE%\include;%PCRE%\src"
if not exist "bin\obj-pcre2" mkdir "bin\obj-pcre2"
if not exist "bin\obj-pcre2\chartables.c" copy /y "%PCRE%\src\pcre2_chartables.c.dist" "bin\obj-pcre2\chartables.c" >nul

rem pcre2_jit_match/misc 由 pcre2_jit_compile.c 包含，pcre2_ucptables 由 pcre2_tables.c 包含，
rem 不能单独编译；pcre2_chkdint 只有宽 8/16 都用同一份符号，单独编一次。
set FILES=pcre2_auto_possess pcre2_compile pcre2_compile_class pcre2_config pcre2_context pcre2_convert pcre2_dfa_match pcre2_error pcre2_extuni pcre2_find_bracket pcre2_jit_compile pcre2_maketables pcre2_match pcre2_match_data pcre2_newline pcre2_ord2utf pcre2_pattern_info pcre2_script_run pcre2_serialize pcre2_string_utils pcre2_study pcre2_substitute pcre2_substring pcre2_tables pcre2_ucd pcre2_valid_utf pcre2_xclass

for %%W in (8 16) do (
  if not exist "bin\obj-pcre2\%%W" mkdir "bin\obj-pcre2\%%W"
  for %%F in (%FILES%) do (
    "%CL%" /nologo /c /MT /O2 /W0 /D_CRT_SECURE_nO_WARnInGS /DPCRE2_STATIC /DHAVE_COnFIG_H /DPCRE2_CODE_UnIT_WIDTH=%%W /I "%PCRE%\include" /I "%PCRE%\src" /Fo"bin\obj-pcre2\%%W\%%F.obj" "%PCRE%\src\%%F.c" || exit /b 1
  )
  "%CL%" /nologo /c /MT /O2 /W0 /D_CRT_SECURE_nO_WARnInGS /DPCRE2_STATIC /DHAVE_COnFIG_H /DPCRE2_CODE_UnIT_WIDTH=%%W /I "%PCRE%\include" /I "%PCRE%\src" /Fo"bin\obj-pcre2\%%W\chartables.obj" "bin\obj-pcre2\chartables.c" || exit /b 1
)

if not exist "bin\obj-pcre2\common" mkdir "bin\obj-pcre2\common"
"%CL%" /nologo /c /MT /O2 /W0 /D_CRT_SECURE_nO_WARnInGS /DPCRE2_STATIC /DHAVE_COnFIG_H /DPCRE2_CODE_UnIT_WIDTH=8 /I "%PCRE%\include" /I "%PCRE%\src" /Fo"bin\obj-pcre2\common\pcre2_chkdint.obj" "%PCRE%\src\pcre2_chkdint.c" || exit /b 1

echo done> "bin\obj-pcre2\16\.stamp"
echo   built width 8 + 16
exit /b 0
