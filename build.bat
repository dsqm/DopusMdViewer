@echo off
chcp 936 >nul
setlocal

rem ============================================================
rem  DopusMdViewer - 手工构建脚本（x64 / UNICODE / 静态 CRT）
rem  用法：双击本文件，产物 bin\DopusMdViewer.dll
rem  依赖：sdk\webview2（WebView2 SDK，随工程分发）
rem        ..\SDK\Headers（Directory Opus 查看器插件头文件）
rem ============================================================

rem ---- 工具链版本：VS2022 或 Windows SDK 升级后改这两行 ----
set "MSVC_VER=14.44.35207"
set "SDK_VER=10.0.26100.0"
set "VS_ROOT=C:\VS2022"
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"

set "PROJ=%~dp0"
set "MSVC=%VS_ROOT%\VC\Tools\MSVC\%MSVC_VER%"
set "SDKINC=%SDK_ROOT%\Include\%SDK_VER%"
set "SDKLIB=%SDK_ROOT%\Lib\%SDK_VER%"
set "CL=%MSVC%\bin\Hostx64\x64\cl.exe"
set "LINK=%MSVC%\bin\Hostx64\x64\link.exe"
set "RC=%SDK_ROOT%\bin\%SDK_VER%\x64\rc.exe"
set "DUMPBIN=%MSVC%\bin\Hostx64\x64\dumpbin.exe"
set "W2INC=%PROJ%sdk\webview2\include"
set "W2LIB=%PROJ%sdk\webview2\x64\WebView2LoaderStatic.lib"
set "OPUSINC=%PROJ%..\SDK\Headers"

echo.
echo [1/5] 检查工具链...
if not exist "%CL%"        echo   找不到 cl.exe   %CL%        & goto :fail
if not exist "%LINK%"      echo   找不到 link.exe %LINK%      & goto :fail
if not exist "%RC%"        echo   找不到 rc.exe   %RC%        & goto :fail
if not exist "%W2INC%\WebView2.h" echo   缺少 WebView2 SDK 头文件 %W2INC% & goto :fail
if not exist "%W2LIB%"     echo   缺少 WebView2LoaderStatic.lib %W2LIB% & goto :fail
if not exist "%OPUSINC%\viewer plugins.h" echo   缺少 Opus 插件头文件 %OPUSINC% & goto :fail
if not exist "%PROJ%web\app.js" echo   缺少 web\app.js，先在 webbuild 跑 node build.mjs & goto :fail

rem 源文件是 UTF-8 无 BOM 且含中文注释：/utf-8 不可省，否则按 936 解读会报一堆假语法错误
set "INCLUDE=%MSVC%\include;%SDKINC%\ucrt;%SDKINC%\shared;%SDKINC%\um;%SDKINC%\winrt;%OPUSINC%;%W2INC%;%PROJ%third_party"
set "LIB=%MSVC%\lib\x64;%SDKLIB%\ucrt\x64;%SDKLIB%\um\x64"

pushd "%PROJ%"
if not exist "bin\obj" mkdir "bin\obj"

echo.
echo [2/5] 编译源文件...
rem 通配符收集 src 下全部 .cpp，新增文件不用改脚本
"%CL%" /nologo /c /MT /O2 /W4 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /utf-8 "/Fo:bin\obj\\" "src\*.cpp"
if errorlevel 1 goto :fail

echo.
echo [3/5] 编译版本资源...
"%RC%" /nologo "/fo%PROJ%bin\obj\resource.res" "%PROJ%resource.rc"
if errorlevel 1 goto :fail

echo.
echo [4/5] 链接 DLL...
"%LINK%" /nologo /DLL /MACHINE:X64 /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /OPT:REF /OPT:ICF ^
  "/DEF:%PROJ%DopusMdViewer.def" "/OUT:%PROJ%bin\DopusMdViewer.dll" ^
  "bin\obj\*.obj" "bin\obj\resource.res" ^
  "%W2LIB%" kernel32.lib user32.lib gdi32.lib ole32.lib oleaut32.lib ^
  shell32.lib shlwapi.lib advapi32.lib psapi.lib version.lib
if errorlevel 1 goto :fail

echo.
echo [5/5] 同步 web 资源...
rem robocopy 返回 0-7 都算成功（1 表示有文件被复制），只有 >=8 才是失败
robocopy "%PROJ%web" "%PROJ%bin\web" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :fail

popd
echo.
echo [OK] %PROJ%bin\DopusMdViewer.dll
echo 导出表（应为 6 个 DVP_ 函数）：
"%DUMPBIN%" /exports "%PROJ%bin\DopusMdViewer.dll" | findstr /i "DVP_ number of"
echo.
echo 下一步：install.bat 复制到 Opus 的 Viewers 目录
pause
exit /b 0

:fail
popd
echo.
echo [失败] 见上方错误信息。
pause
exit /b 1
