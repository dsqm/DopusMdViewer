@echo off
rem build-encdetect.bat - build tools\encdetect (encoding detection harness, ticket 19).
rem Depends on build\obj-ced (this script calls build-ced.bat if the cache is missing).
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
if not exist "build\obj-ced\.stamp" (
  call "%PROJ%build-ced.bat"
  if errorlevel 1 exit /b 1
)

set "INCLUDE=%MSVC%\include;%SDKINC%\ucrt;%SDKINC%\shared;%SDKINC%\um"
set "LIB=%MSVC%\lib\x64;%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64;%SDK_ROOT%\Lib\%SDK_VER%\um\x64"
if not exist "build\tools" mkdir "build\tools"
if not exist "build\obj-encdetect" mkdir "build\obj-encdetect"

pushd "%PROJ%"
rem /utf-8 必带：源码含中文注释，否则按 936 读会报一堆假语法错误。
"%CL%" /nologo /c /MT /O2 /W4 /EHsc /utf-8 /I "%CED%" /I "%PROJ%src" "/Fo:build\obj-encdetect\\" "tools\encdetect\*.cpp" "src\ced_wrap.cpp" "src\encoding_detect.cpp"
if errorlevel 1 goto :fail

"%CL%" /nologo /MT /O2 /EHsc "build\obj-encdetect\*.obj" "build\obj-ced\*.obj" /Fe"build\tools\encdetect.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

popd
echo.
echo [OK] %PROJ%build\tools\encdetect.exe
exit /b 0

:fail
popd
echo.
echo [FAIL]
exit /b 1
