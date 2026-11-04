@echo off
chcp 936 >nul
setlocal

rem ============================================================
rem  DopusMdViewer - 手工构建脚本（x64 / UNICODE / 静态 CRT）
rem  用法：双击本文件，构建 web\ 前端并输出 bin\DopusMdViewer.dll
rem  依赖：sdk\webview2（WebView2 SDK，随工程分发）
rem        ..\SDK\Headers（Directory Opus 查看器插件头文件）
rem        webbuild\node_modules（npm install 一次；构建用内置 esbuild.exe，不需要 node）
rem ============================================================

rem ---- Locate VS (vswhere) and newest Windows SDK ----
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_ROOT="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%i"
)
if not defined VS_ROOT echo   vswhere found no Visual Studio with C++ tools & goto :fail
if not exist "%VS_ROOT%\VC\Tools\MSVC" echo   MSVC tools not found under %VS_ROOT% & goto :fail

rem pick the newest installed MSVC and Windows SDK
set "MSVC_VER="
for /f "delims=" %%v in ('dir /b /ad "%VS_ROOT%\VC\Tools\MSVC"') do set "MSVC_VER=%%v"
set "SDK_VER="
for /f "delims=" %%v in ('dir /b /ad "%SDK_ROOT%\Include" ^| findstr /r "^10\." ') do set "SDK_VER=%%v"
if not defined MSVC_VER echo   no MSVC toolset found & goto :fail
if not defined SDK_VER echo   no Windows SDK 10 found & goto :fail
echo   VS=%VS_ROOT%
echo   MSVC=%MSVC_VER%
echo   SDK=%SDK_VER%

set "PROJ=%~dp0"
set "MSVC=%VS_ROOT%\VC\Tools\MSVC\%MSVC_VER%"
set "SDKINC=%SDK_ROOT%\Include\%SDK_VER%"
set "SDKLIB=%SDK_ROOT%\Lib\%SDK_VER%"
set "CL_EXE=%MSVC%\bin\Hostx64\x64\cl.exe"
set "LINK_EXE=%MSVC%\bin\Hostx64\x64\link.exe"
set "RC=%SDK_ROOT%\bin\%SDK_VER%\x64\rc.exe"
set "DUMPBIN=%MSVC%\bin\Hostx64\x64\dumpbin.exe"
set "W2INC=%PROJ%sdk\webview2\include"
set "W2LIB=%PROJ%sdk\webview2\x64\WebView2LoaderStatic.lib"
set "OPUSINC=%PROJ%..\SDK\Headers"
set "ESBUILD=%PROJ%webbuild\node_modules\@esbuild\win32-x64\esbuild.exe"

echo.
echo [1/8] 检查工具链...
if not exist "%CL_EXE%"        echo   找不到 cl.exe   %CL_EXE%        & goto :fail
if not exist "%LINK_EXE%"      echo   找不到 link.exe %LINK_EXE%      & goto :fail
if not exist "%RC%"        echo   找不到 rc.exe   %RC%        & goto :fail
if not exist "%W2INC%\WebView2.h" echo   缺少 WebView2 SDK 头文件 %W2INC% & goto :fail
if not exist "%W2LIB%"     echo   缺少 WebView2LoaderStatic.lib %W2LIB% & goto :fail
if not exist "%OPUSINC%\viewer plugins.h" echo   缺少 Opus 插件头文件 %OPUSINC% & goto :fail
if not exist "%PROJ%webbuild\src\app.js" echo   缺少 webbuild\src\app.js & goto :fail
if not exist "%PROJ%webbuild\src\mermaid.entry.mjs" echo   缺少 webbuild\src\mermaid.entry.mjs & goto :fail
if not exist "%ESBUILD%" echo   缺少 esbuild，先在 webbuild 跑 npm install & goto :fail
if not exist "%PROJ%webbuild\node_modules\katex\dist\katex.min.css" echo   缺少 KaTeX 资源，先在 webbuild 跑 npm install & goto :fail
if not exist "%PROJ%tools\gen-version.ps1" echo   缺少 tools\gen-version.ps1 & goto :fail

rem 源文件是 UTF-8 无 BOM 且含中文注释：/utf-8 不可省，否则按 936 解读会报一堆假语法错误
set "INCLUDE=%MSVC%\include;%SDKINC%\ucrt;%SDKINC%\shared;%SDKINC%\um;%SDKINC%\winrt;%OPUSINC%;%W2INC%;%PROJ%third_party;%PROJ%third_party\compact_enc_det"
set "LIB=%MSVC%\lib\x64;%SDKLIB%\ucrt\x64;%SDKLIB%\um\x64"

pushd "%PROJ%"
if not exist "bin" mkdir "bin"
if not exist "build\obj" mkdir "build\obj"

echo.
echo [2/8] 构建前端...
rem 参数与 webbuild\build.mjs 保持一致
"%ESBUILD%" webbuild\src\app.js --bundle --outfile=web\app.js --format=iife --target=chrome110 --minify --legal-comments=none --charset=utf8 --log-level=warning
if errorlevel 1 goto :fail
"%ESBUILD%" webbuild\src\mermaid.entry.mjs --bundle --outfile=web\mermaid.js --format=iife --global-name=mermaid --target=chrome110 --minify --legal-comments=none --charset=utf8 --log-level=warning
if errorlevel 1 goto :fail
copy /y "webbuild\node_modules\katex\dist\katex.min.css" "web\katex.css" >nul
if errorlevel 1 goto :fail
if not exist "web\fonts" mkdir "web\fonts"
xcopy /y /q "webbuild\node_modules\katex\dist\fonts\*.woff2" "web\fonts\" >nul
if errorlevel 1 goto :fail

echo.
echo [3/8] 编译源文件...
rem 通配符收集 src 下全部 .cpp，新增文件不用改脚本
"%CL_EXE%" /nologo /c /MT /O2 /W4 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /utf-8 /DPCRE2_STATIC /Ithird_party\pcre2\include "/Fo:build\obj\\" "src\*.cpp"
if errorlevel 1 goto :fail

echo.
echo [4/8] 生成版本号并编译版本资源...
rem 版本号取构建时刻，写到 build\obj\version.rc.inc，不入库
powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJ%tools\gen-version.ps1"
if errorlevel 1 goto :fail
"%RC%" /nologo "/fo%PROJ%build\obj\resource.res" "%PROJ%resource.rc"
if errorlevel 1 goto :fail

echo.
echo [5/8] PCRE2 ...
call "%PROJ%build-pcre2.bat"
if errorlevel 1 goto :fail

echo.
echo [6/8] CED (encoding detect) ...
call "%PROJ%build-ced.bat"
if errorlevel 1 goto :fail

echo.
echo [7/8] 链接 DLL...
"%LINK_EXE%" /nologo /DLL /MACHINE:X64 /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /OPT:REF /OPT:ICF ^
  "/DEF:%PROJ%DopusMdViewer.def" "/OUT:%PROJ%bin\DopusMdViewer.dll" "/IMPLIB:%PROJ%build\obj\DopusMdViewer.lib" ^
  "build\obj\*.obj" "build\obj\resource.res" "build\obj-ced\*.obj" "build\obj-pcre2\8\*.obj" "build\obj-pcre2\16\*.obj" "build\obj-pcre2\common\*.obj" ^
  "%W2LIB%" kernel32.lib user32.lib gdi32.lib ole32.lib oleaut32.lib ^
  shell32.lib shlwapi.lib advapi32.lib psapi.lib version.lib comctl32.lib windowscodecs.lib
if errorlevel 1 goto :fail

echo.
echo [8/8] 同步 web 资源...
rem robocopy 返回 0-7 都算成功（1 表示有文件被复制），只有 >=8 才是失败
robocopy "%PROJ%web" "%PROJ%bin\DopusMdViewer" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :fail

popd
echo.
echo [OK] %PROJ%bin\DopusMdViewer.dll
echo 导出表（应为 7 个 DVP_ 函数）：
"%DUMPBIN%" /exports "%PROJ%bin\DopusMdViewer.dll" | findstr /i "DVP_ number of"
echo.
pause
exit /b 0

:fail
popd
echo.
echo [失败] 见上方错误信息。
pause
exit /b 1
