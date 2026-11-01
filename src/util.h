// util.h - 路径、文件、编码这些杂事。
//
// 只依赖 Windows API 与标准库；不引用 Opus 头文件，也不引用 WebView2 SDK。

#pragma once

#include <windows.h>

#include <cstdint>
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
// 进程内只解析一次（改别名后需重启 Opus）；皮肤、config.json、logs 同根。
// WebView2 缓存默认在本机 %LOCALAPPDATA%\DopusMdViewer\WebView2，可在设置里覆盖。
std::wstring UtilDataDir(void);

// 同上，但不碰标准库：日志初始化可能早于 CRT 就绪。
void UtilDataDirInto(wchar_t* buf, size_t cch);

// a + "\\" + b；a 允许带或不带尾随反斜杠。
std::wstring UtilJoin(const std::wstring& a, const wchar_t* b);

// 去首尾空白。
std::wstring UtilTrim(const std::wstring& text);

// 展开 %VAR%；未知变量保持原样。
std::wstring UtilExpandEnv(const std::wstring& text);

// 本地绝对路径：盘符 + 冒号 + 反斜杠；拒绝 UNC、相对路径。
bool UtilIsLocalAbsolutePath(const std::wstring& path);

// 展开环境变量、转反斜杠、解析 . / ..、去尾随分隔符；非法时返回空串。
std::wstring UtilNormalizeLocalDir(const std::wstring& text);

// WebView2 用户数据的默认父目录：%LOCALAPPDATA%\DopusMdViewer。
std::wstring UtilDefaultWebView2BaseDir(void);

// 实际 WebView2 用户数据目录：<配置父目录>\WebView2。
// 配置为空或非法时用默认父目录；完全不可用时返回空串。
std::wstring UtilWebView2UserDataDir(const std::wstring& configuredBase);

// 逐级创建目录；已存在不算失败。
bool UtilEnsureDir(const std::wstring& dir);

bool UtilDirExists(const std::wstring& dir);

// 路径本身是否是 ...\WebView2（大小写不敏感，且必须是本地绝对路径）。
bool UtilIsWebView2UserDataDir(const std::wstring& path);

// 目录递归占用字节数；stopAt > 0 时达到该值即提前返回。
uint64_t UtilDirSize(const std::wstring& dir, uint64_t stopAt = 0);

// 递归删除目录（含只读文件）；不存在视为成功。只用于可再生的缓存目录。
bool UtilRemoveTree(const std::wstring& dir);

// 人类可读的大小："98.0 MB"。
std::wstring UtilFormatSize(uint64_t bytes);

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
