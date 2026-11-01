// log.h - append-only diagnostic log for DopusMdViewer.
// Off by default; the "记录日志" setting turns on file and debugger output.
#pragma once

#include <windows.h>

// 全局开关：配置加载/保存时同步。关闭时 LogLine 直接返回，不建目录、不写盘。
void LogSetEnabled(bool enabled);

// printf-style logging. Appends a CRLF. Never throws, never fails hard:
// if the file cannot be written the message still reaches the debugger.
void LogLine(const wchar_t* fmt, ...);
