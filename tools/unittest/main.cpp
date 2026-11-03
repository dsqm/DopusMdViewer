// main.cpp - unittest.exe 入口。
//
// 启动时把 APPDATA / LOCALAPPDATA 重定向到 %TEMP%\mdv-unittest-<pid>\，
// config / 日志 / WebView2 目录全部落在隔离区，绝不碰真实用户配置
// （与 hosttest 的隔离手法一致）。目录按 PID 取名，无需清理残留。

#include "test.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace unittest {

// ---- 断言状态（测试单线程，无锁）----

namespace {

int g_assertFailures = 0;  // 当前测试内的断言失败数
int g_totalFailures = 0;   // 全程累计

std::string ToAscii(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (wchar_t c : text) {
    const unsigned int u = static_cast<unsigned int>(c);
    if (c >= 0x20 && c < 0x7F) {
      out += static_cast<char>(c);
    } else if (c == L'\n') {
      out += "\\n";
    } else if (c == L'\r') {
      out += "\\r";
    } else if (c == L'\t') {
      out += "\\t";
    } else if (u < 0x10000) {
      char buf[16];
      sprintf_s(buf, "\\u%04X", u);
      out += buf;
    } else {
      out += "\\U?";
    }
  }
  return out;
}

}  // namespace

int Failures(void) { return g_assertFailures; }

void RecordFailure(const char* file, int line, const std::string& what) {
  ++g_assertFailures;
  ++g_totalFailures;
  const char* slash = std::strrchr(file, '\\');
  if (!slash) slash = std::strrchr(file, '/');
  printf("    %s:%d: %s\n", slash ? slash + 1 : file, line, what.c_str());
}

void Check(bool cond, const char* file, int line, const std::string& what) {
  if (!cond) RecordFailure(file, line, what);
}

void CheckEqInt(long long got, long long want, const char* file, int line, const char* expr) {
  if (got == want) return;
  char buf[256];
  sprintf_s(buf, "%s = %lld (want %lld)", expr, got, want);
  RecordFailure(file, line, buf);
}

void CheckEqStr(const std::wstring& got, const wchar_t* want, const char* file, int line,
                const char* expr) {
  const std::wstring wwant(want ? want : L"");
  if (got == wwant) return;
  RecordFailure(file, line, expr);
  printf("      got  \"%s\"\n      want \"%s\"\n", ToAscii(got).c_str(),
         ToAscii(wwant).c_str());
}

void CheckContains(const std::wstring& hay, const wchar_t* needle, const char* file, int line,
                   const char* expr) {
  if (hay.find(needle) != std::wstring::npos) return;
  RecordFailure(file, line, expr);
  printf("      hay  \"%s\"\n      want substring \"%s\"\n", ToAscii(hay).c_str(),
         ToAscii(needle).c_str());
}

// ---- 文件与编码助手 ----

const std::wstring& TempRoot(void) {
  static std::wstring root;
  if (root.empty()) {
    wchar_t temp[MAX_PATH];
    const DWORD n = ::GetTempPathW(ARRAYSIZE(temp), temp);
    root = (n > 0 && n < ARRAYSIZE(temp)) ? std::wstring(temp, n) : std::wstring(L"C:\\Temp\\");
    root += L"mdv-unittest";
  }
  return root;
}

namespace {

// 逐级建目录（框架自用，不依赖被测的 UtilEnsureDir）。
void EnsureDirRecursive(const std::wstring& dir) {
  if (dir.empty()) return;
  if (::CreateDirectoryW(dir.c_str(), nullptr)) return;
  if (::GetLastError() == ERROR_ALREADY_EXISTS) return;
  const size_t pos = dir.find_last_of(L"\\/");
  if (pos != std::wstring::npos && pos > 3) EnsureDirRecursive(dir.substr(0, pos));
  ::CreateDirectoryW(dir.c_str(), nullptr);
}

}  // namespace

std::wstring MakeTempDir(const char* tag) {
  static long seq = 0;
  wchar_t buf[64];
  swprintf_s(buf, L"\\%hs-%03ld", tag, ::InterlockedIncrement(&seq));
  const std::wstring dir = TempRoot() + buf;
  EnsureDirRecursive(dir);
  return dir;
}

bool WriteBytes(const std::wstring& path, const void* data, size_t bytes) {
  const size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return false;
  EnsureDirRecursive(path.substr(0, pos));

  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = ::WriteFile(h, data, static_cast<DWORD>(bytes), &written, nullptr);
  ::CloseHandle(h);
  return ok != FALSE && written == bytes;
}

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
  return WriteBytes(path, bytes.data(), bytes.size());
}

std::string Narrow(const wchar_t* text) {
  const std::wstring w(text ? text : L"");
  if (w.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr,
                          nullptr);
  }
  return out;
}

std::string EncodeWith(const std::wstring& text, UINT codepage) {
  if (text.empty()) return std::string();
  const int n = ::WideCharToMultiByte(codepage, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) {
    ::WideCharToMultiByte(codepage, 0, text.data(), static_cast<int>(text.size()), &out[0], n,
                          nullptr, nullptr);
  }
  return out;
}

// ---- 运行全部 ----

int RunAll(int* assertFailures) {
  int failedCases = 0;
  int ran = 0;
  for (const TestCase& t : Tests()) {
    char name[128];
    sprintf_s(name, "%s.%s", t.suite, t.name);
    g_assertFailures = 0;

    LARGE_INTEGER t0;
    LARGE_INTEGER t1;
    ::QueryPerformanceCounter(&t0);
    printf("[ RUN  ] %s\n", name);
    t.fn();
    ::QueryPerformanceCounter(&t1);

    LARGE_INTEGER freq;
    ::QueryPerformanceFrequency(&freq);
    const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                      static_cast<double>(freq.QuadPart ? freq.QuadPart : 1);

    if (g_assertFailures > 0) {
      ++failedCases;
      printf("[ FAIL ] %s (%.1f ms)\n", name, ms);
    } else {
      printf("[   ok ] %s (%.1f ms)\n", name, ms);
    }
    ++ran;
  }
  if (assertFailures) *assertFailures = g_totalFailures;
  printf("------------------------------------------------------------\n");
  printf("%d tests, %d failed, %d failed asserts\n", ran, failedCases, g_totalFailures);
  return failedCases;
}

}  // namespace unittest

// ---- 环境隔离与入口 ----

namespace {

void RedirectEnvToTemp(void) {
  wchar_t pid[16];
  swprintf_s(pid, L"%lu", static_cast<unsigned long>(::GetCurrentProcessId()));
  const std::wstring base = unittest::TempRoot() + L"-" + pid;

  // 环境变量必须在任何 Util* / Config* 调用之前设置：UtilDataDir 是 InitOnce 缓存。
  ::SetEnvironmentVariableW(L"APPDATA", (base + L"\\appdata").c_str());
  ::SetEnvironmentVariableW(L"LOCALAPPDATA", (base + L"\\localappdata").c_str());
}

}  // namespace

int main(void) {
  RedirectEnvToTemp();

  int assertFailures = 0;
  const int failed = unittest::RunAll(&assertFailures);
  return failed > 0 ? 1 : 0;
}
