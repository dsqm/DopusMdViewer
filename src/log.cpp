// log.cpp - append-only diagnostic log.
//
// Design notes:
//  * The file is opened / appended / closed per line. A viewer plugin can be
//    unloaded at any moment, and a dropped buffered line is worse than a slow one.
//  * A single SRWLOCK serialises writers; SRWLOCK_INIT needs no DllMain work,
//    which matters because DVP_InitEx can arrive before the CRT is warm.
//  * Any failure degrades to OutputDebugStringW only. No exceptions, no asserts.

#include "log.h"

#include <stdarg.h>
#include <strsafe.h>

#include "util.h"

namespace {

// The log lives next to the WebView2 user data, under the data root from
// UtilDataDirInto (which may be overridden by a Folder Alias). %TEMP% is only
// the fallback for the case where that root is unavailable.
const int kMaxLogBytes = 2 * 1024 * 1024;

SRWLOCK g_lock = SRWLOCK_INIT;
wchar_t g_logPath[MAX_PATH];
bool g_pathReady = false;

// Best effort; ERROR_ALREADY_EXISTS is the normal case and is not an error.
// 必须逐级创建：全新机器上 User Data 下的目录树一层都不存在。
void EnsureDir(const wchar_t* pszDir) {
  if (!pszDir || !*pszDir) return;
  wchar_t path[MAX_PATH];
  if (FAILED(::StringCchCopyW(path, ARRAYSIZE(path), pszDir))) return;
  for (wchar_t* p = path + 1; *p; ++p) {
    if (*p != L'\\') continue;
    *p = L'\0';
    ::CreateDirectoryW(path, nullptr);
    *p = L'\\';
  }
  ::CreateDirectoryW(path, nullptr);
}

void FallBackToTemp(wchar_t* pszPath, size_t cchPath) {
  wchar_t tempPath[MAX_PATH];
  DWORD n = ::GetTempPathW(MAX_PATH, tempPath);
  if (n == 0 || n >= MAX_PATH) {
    // Fall back to the current directory rather than giving up on logging.
    ::StringCchCopyW(tempPath, MAX_PATH, L".\\");
  }
  ::StringCchCopyW(pszPath, cchPath, tempPath);
}

void EnsurePathLocked() {
  if (g_pathReady) return;

  wchar_t dataDir[MAX_PATH];
  UtilDataDirInto(dataDir, ARRAYSIZE(dataDir));

  if (!dataDir[0]) {
    FallBackToTemp(g_logPath, MAX_PATH);
  } else {
    wchar_t logDir[MAX_PATH];
    ::StringCchPrintfW(logDir, MAX_PATH, L"%s\\logs", dataDir);
    EnsureDir(dataDir);
    EnsureDir(logDir);
    ::StringCchPrintfW(g_logPath, MAX_PATH, L"%s\\DopusMdViewer.log", logDir);
  }

  g_pathReady = true;
}

// Keep the log bounded: one rotation to *.1, older content is dropped.
void RotateIfOversizedLocked() {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!::GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fad)) return;

  const ULONGLONG size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  if (size < static_cast<ULONGLONG>(kMaxLogBytes)) return;

  wchar_t szOld[MAX_PATH];
  ::StringCchPrintfW(szOld, MAX_PATH, L"%s.1", g_logPath);
  ::MoveFileExW(g_logPath, szOld, MOVEFILE_REPLACE_EXISTING);
}

bool WriteRaw(const char* data, DWORD size) {
  RotateIfOversizedLocked();

  HANDLE h = ::CreateFileW(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER sz;
  sz.QuadPart = 0;
  bool ok = true;

  // Stamp a UTF-8 BOM on a freshly created (empty) log so editors detect the encoding.
  if (::GetFileSizeEx(h, &sz) && sz.QuadPart == 0) {
    static const char kBom[3] = {'\xEF', '\xBB', '\xBF'};
    DWORD written = 0;
    ::WriteFile(h, kBom, 3, &written, nullptr);
  }

  DWORD written = 0;
  if (!::WriteFile(h, data, size, &written, nullptr) || written != size) ok = false;

  ::CloseHandle(h);
  return ok;
}

}  // namespace

void LogEnsureInit(void) {
  ::AcquireSRWLockExclusive(&g_lock);
  EnsurePathLocked();
  ::ReleaseSRWLockExclusive(&g_lock);
}

const wchar_t* LogLastPath(void) {
  LogEnsureInit();
  return g_logPath;
}

void LogLine(const wchar_t* fmt, ...) {
  LogEnsureInit();

  wchar_t body[1024];
  body[0] = L'\0';

  va_list args;
  va_start(args, fmt);
  ::StringCchVPrintfW(body, ARRAYSIZE(body), fmt, args);
  va_end(args);
  body[ARRAYSIZE(body) - 1] = L'\0';

  SYSTEMTIME st;
  ::GetLocalTime(&st);

  wchar_t line[1200];
  ::StringCchPrintfW(line, ARRAYSIZE(line), L"%02u:%02u:%02u.%03u [%lu] %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     static_cast<unsigned long>(::GetCurrentThreadId()), body);

  ::OutputDebugStringW(line);

  int need = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1) return;

  char utf8[2400];
  if (need > static_cast<int>(sizeof(utf8))) return;

  int got = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, static_cast<int>(sizeof(utf8)),
                                  nullptr, nullptr);
  if (got <= 1) return;

  ::AcquireSRWLockExclusive(&g_lock);
  EnsurePathLocked();
  WriteRaw(utf8, static_cast<DWORD>(got - 1));  // drop the terminating NUL
  ::ReleaseSRWLockExclusive(&g_lock);
}
