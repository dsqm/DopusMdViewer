// util.h - 路径、文件、编码这些杂事。
//
// 只依赖 Windows API 与标准库；不引用 Opus 头文件，也不碰 WebView2。

#pragma once

#include <windows.h>

#include <string>

// DLL 所在目录，不带尾随反斜杠。
std::wstring UtilModuleDir(void);

// 前端资源目录（shell.html / app.js / skins / fonts）：DLL 同级、与插件同名，
// 不用通用的 "web" 名字，免得和 Viewers 里别的插件抢目录。
std::wstring UtilWebDir(void);

// 存在这个 Folder Alias 时，数据根改用它指向的文件夹。
// 解析靠 dopus.exe 导出的 ResolveAliasPath（未写进 SDK 头，取不到就回退）。
constexpr wchar_t kDataAliasName[] = L"DopusMdViewerData";

// 数据根目录：优先 /DopusMdViewerData 别名，失败时回退
// %APPDATA%\GPSoftware\Directory Opus\User Data\DopusMdViewer。
// 进程内只解析一次（改别名后需重启 Opus）；皮肤、config.json、logs、WebView2 同根。
std::wstring UtilDataDir(void);

// 同上，但不碰标准库：日志初始化可能早于 CRT 就绪。
void UtilDataDirInto(wchar_t* buf, size_t cch);

// a + "\\" + b；a 允许带或不带尾随反斜杠。
std::wstring UtilJoin(const std::wstring& a, const wchar_t* b);

// 逐级创建目录；已存在不算失败。
bool UtilEnsureDir(const std::wstring& dir);

bool UtilFileExists(const std::wstring& path);

// 读整个文件。超过 maxBytes 直接失败（调用方用它挡住病态文件）。
bool UtilReadFileBytes(const std::wstring& path, size_t maxBytes, std::string* out);

// 去 BOM 后按 UTF-8 解码；非法字节再按 GB18030 试一次（记事本另存为 ANSI 很常见）。
// 两者都不成立时返回 false，out 保持不动。
bool UtilDecodeText(const std::string& bytes, std::wstring* out);

// 取父目录（不含尾随反斜杠）；没有分隔符时返回空串。
std::wstring UtilParentDir(const std::wstring& path);

// 文件名部分。
const wchar_t* UtilLeafName(const wchar_t* path);

// 本 DLL 的版本资源（"1.2.3"）；读不到返回空串。
std::wstring UtilModuleVersion(void);

// 任意可执行文件/模块的版本资源（"1.2.3.4"）；不存在或没有版本资源时返回空串。
std::wstring UtilFileVersion(const std::wstring& path);
