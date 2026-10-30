// util.h - 路径、文件、编码这些杂事。
//
// 只依赖 Windows API 与标准库；不引用 Opus 头文件，也不碰 WebView2。

#pragma once

#include <windows.h>

#include <string>

// DLL 所在目录，不带尾随反斜杠（web\ 就在它下面）。
std::wstring UtilModuleDir(void);

// %APPDATA%\GPSoftware\Directory Opus\User Data\DopusMdViewer
// 皮肤、config.json、logs、WebView2 用户数据同根（票 10、票 09）。
// 这个路径与 log.cpp 里的 kDataSubDir 必须一致。
std::wstring UtilDataDir(void);

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
