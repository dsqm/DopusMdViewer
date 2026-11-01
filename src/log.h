// log.h - append-only diagnostic log for DopusMdViewer.
// Off by default; the "记录日志" setting turns on file and debugger output.
#pragma once

#include <windows.h>

// 全局开关：配置加载/保存时同步。关闭时 LogLine 直接返回，不建目录、不写盘。
void LogSetEnabled(bool enabled);

// printf-style logging. Appends a CRLF. Never throws, never fails hard:
// if the file cannot be written the message still reaches the debugger.
void LogLine(const wchar_t* fmt, ...);

// 同 LogLine，但无视开关：读不到配置时就无从得知用户的日志意图，
// 这类启动现场只有绕过开关才留得下记录。
void LogLineAlways(const wchar_t* fmt, ...);
