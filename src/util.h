// util.h - 路径、文件、编码这些杂事。
//
// 只依赖 Windows API 与标准库；不引用 Opus 头文件，也不引用 WebView2 SDK。

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// DLL 所在目录，不带尾随反斜杠。
std::wstring UtilModuleDir(void);

// 前端资源目录（shell.html / app.js / skins / fonts）：DLL 同级、与插件同名，
// 不用通用的 "web" 名字，免得和 Viewers 里别的插件抢目录。
std::wstring UtilWebDir(void);

// 随插件一起导出的文件（DVP_USBSafe 用）：相对 Viewers\ 的路径，形如
// "DopusMdViewer\web\app.js"。资源目录不存在时返回空列表。
void UtilUsbExportFiles(std::vector<std::wstring>* out);

// 存在这个 Folder Alias 时，数据根改用它指向的文件夹。
// 解析靠 dopus.exe 导出的 ResolveAliasPath（未写进 SDK 头，取不到就回退）。
constexpr wchar_t kDataAliasName[] = L"DopusMdViewerData";

// 数据根目录：优先 /DopusMdViewerData 别名，否则 /dopusdata\User Data\DopusMdViewer。
// 走 Opus 的内置别名而不是 %APPDATA%：U 盘版的 /dopusdata 指向 U 盘，环境变量不会。
// 解析成功一次即固定；失败不缓存，下次调用重试。皮肤、config.json、logs 同根。
// WebView2 缓存默认在本机 %LOCALAPPDATA%\DopusMdViewer\WebView2，可在设置里覆盖。
std::wstring UtilDataDir(void);

// 同上，但不碰标准库：日志初始化可能早于 CRT 就绪。
void UtilDataDirInto(wchar_t* buf, size_t cch);

// U 盘模式：由 DVP_IdentifyW 从 dwInitFlags 的 VPINITF_USB 读到后设一次。
void UtilSetUsbMode(bool usb);
bool UtilIsUsbMode(void);

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

// 实际使用的父目录：用户指定值优先；留空时 U 盘模式按「%TEMP% → 数据目录 →
// 本机 %LOCALAPPDATA%」逐个实测可写性（网络盘直接跳过），非 U 盘用本机默认。
std::wstring UtilWebView2BaseDir(const std::wstring& configuredBase);

// 上一行选了哪一级：config / temp / data / local / default（给日志与设置页提示用）。
std::wstring UtilWebView2BaseSource(const std::wstring& configuredBase);

// 查看器的用户数据目录：<父目录>\WebView2。
std::wstring UtilWebView2UserDataDir(const std::wstring& configuredBase);

// 设置页字体预览的用户数据目录：<父目录>\WebView2-Settings。
// 必须与查看器那份分开：同一个目录不能被两个 Environment 同时使用。
std::wstring UtilWebView2SettingsDir(const std::wstring& configuredBase);

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

// Win32 错误码对应的系统消息（中文系统上就是中文），去掉尾随的换行。
// 取不到（code 为 0、或系统没有这条消息）时返回空串，调用方自己兜底。
std::wstring UtilWin32ErrorText(DWORD code);

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

// dopusrt.exe 的完整路径（Opus 对外唯一的"打开查看器"入口）；找不到返回空串。
// 顺序：宿主 exe 同目录（插件总跑在 dopus.exe 里）→ DLL 的上级目录（Viewers\ 的父目录）
// → %ProgramFiles%\GPSoftware\Directory Opus。便携版靠前两条命中。
std::wstring UtilOpusViewerExe(void);

// 用 Opus 独立查看器显示文件：dopusrt.exe /show "<path>"。
// Viewer Plugin SDK 没有这个能力（DVP_InitEx 只给消息窗口与版本号），Opus 只对外
// 提供 dopusrt 命令行。找不到 dopusrt、文件不存在或进程起不来时返回 false，
// 调用方自行回退到系统默认程序。
bool UtilOpusViewerShow(const std::wstring& path);

// 内置名单（分号分隔，带点），给设置里的「扩展名名单」当初始值与「恢复默认」用。
std::wstring UtilOpusViewerDefaultExts(void);

// 这个扩展名是否该交给 Opus 查看器。list 是用户编辑过的完整名单（分号/逗号/空白
// 分隔，带不带点都认）；**空串表示用内置名单**。名单外的类型交回系统默认程序，
// 免得 Opus 弹「无法显示」。
bool UtilOpusViewerHandlesExt(const wchar_t* ext, const std::wstring& list);
