@echo off
chcp 936 >nul
rem 皮肤编辑器发布包：只把 build\tools 下的 exe 与 assets 打进 zip。
rem 发布前先跑 python tools\skinedit\build.py 全量重编，让版本号与页面资产对齐。
rem 运行时生成物（WebView2 缓存、ini、日志）与其它开发工具一律不进包。
setlocal
set "HERE=%~dp0"
for %%I in ("%HERE%..\..") do set "ROOT=%%~fI"
set "SRC=%ROOT%\build\tools"
set "DIST=%ROOT%\build\dist"
set "STAGE=%ROOT%\build\_pack_stage"
set "TAR=%SystemRoot%\System32\tar.exe"

if not exist "%SRC%\skinedit.exe" echo 找不到 skinedit.exe，请先跑全量构建。 & goto :fail
if not exist "%SRC%\assets\shell.html" echo 找不到 assets 页面，请先跑全量构建。 & goto :fail
if not exist "%TAR%" echo 缺少 tar.exe，需要 Windows 10 1803 或更高版本。 & goto :fail

set "VER=portable"
if exist "%ROOT%\build\obj-skinedit\version.rc.inc" (
  for /f "usebackq tokens=3" %%V in (`findstr MDV_VER_STRING "%ROOT%\build\obj-skinedit\version.rc.inc"`) do set "VER=%%~V"
)
set "OUT=%DIST%\skinedit-%VER%.zip"

if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" 2>nul
if not exist "%DIST%" mkdir "%DIST%" 2>nul
copy /y "%SRC%\skinedit.exe" "%STAGE%" >nul
if errorlevel 1 goto :fail
xcopy /e /i /q /y "%SRC%\assets" "%STAGE%\assets" >nul
if errorlevel 1 goto :fail

rem 随包说明：程序目录必须可写，运行时用系统已装的常青版 WebView2 Runtime。
> "%STAGE%\README.txt" echo 皮肤编辑器（DopusMdViewer 皮肤编辑器工具）
>> "%STAGE%\README.txt" echo.
>> "%STAGE%\README.txt" echo 运行要求
>> "%STAGE%\README.txt" echo 1 解压到一个可写的文件夹，不要放在 C:\Program Files。
>> "%STAGE%\README.txt" echo   程序会在自身目录创建 WebView2 缓存、skinedit.ini 和 skinedit.log。
>> "%STAGE%\README.txt" echo 2 需要 Windows 已安装 Microsoft Edge WebView2 Runtime（常青版）。
>> "%STAGE%\README.txt" echo   Windows 11 与已装 Edge 的 Windows 10 通常自带；若提示运行时不可用，
>> "%STAGE%\README.txt" echo   请到 https://developer.microsoft.com/microsoft-edge/webview2/ 下载安装。
>> "%STAGE%\README.txt" echo.
>> "%STAGE%\README.txt" echo 使用
>> "%STAGE%\README.txt" echo 双击 skinedit.exe 启动。assets\skins 是四套只读模板，改完用另存为存成自己的 .css。
>> "%STAGE%\README.txt" echo 皮肤默认保存到 DOpus 插件的用户皮肤目录；没装插件时该列表为空，
>> "%STAGE%\README.txt" echo 可用命令行参数 --skins 指定皮肤目录，用 --web 指定资产目录。
>> "%STAGE%\README.txt" echo assets\LICENSES 是随包分发的第三方组件许可声明，请一并保留。

if exist "%OUT%" del /f /q "%OUT%"
"%TAR%" -a -cf "%OUT%" -C "%STAGE%" skinedit.exe assets README.txt
if errorlevel 1 goto :fail
rmdir /s /q "%STAGE%"
echo 完成：%OUT%
pause
exit /b 0

:fail
if exist "%STAGE%" rmdir /s /q "%STAGE%"
echo 打包失败。
pause
exit /b 1
