// log.h - append-only diagnostic log for DopusMdViewer.
// Every line goes to %TEMP%\DopusMdViewer.log and to OutputDebugStringW.
#pragma once

#include <windows.h>

// Resolve the log path (%TEMP%\DopusMdViewer.log) once. Safe to call from any thread.
void LogEnsureInit(void);

// printf-style logging. Always appends a CRLF. Never throws, never fails hard:
// if the file cannot be written the message still reaches the debugger.
void LogLine(const wchar_t* fmt, ...);

// Wide string helpers used by the log/status paths.
const wchar_t* LogLastPath(void);
