#include "util.h"

#include <strsafe.h>
#include <winver.h>

#include <vector>

namespace {

const wchar_t kDataSubDir[] = L"GPSoftware\\Directory Opus\\User Data\\DopusMdViewer";

// dopus.exe 的未公开导出；本机所有内置 Viewer 也用 GetProcAddress 按名字取它。
using PFN_RESOLVEALIASPATH = BOOL(WINAPI*)(LPCWSTR, LPWSTR, UINT);

INIT_ONCE g_dataDirOnce = INIT_ONCE_STATIC_INIT;
wchar_t g_dataDir[MAX_PATH] = L"";

bool IsAbsoluteFilePath(const wchar_t* path) {
  if (!path || !path[0]) return false;
  if (path[0] == L'\\' && path[1] == L'\\') return true;  // UNC、\\?\ 长路径
  const bool drive = ((path[0] >= L'A' && path[0] <= L'Z') ||
                      (path[0] >= L'a' && path[0] <= L'z')) &&
                     path[1] == L':';
  return drive && (path[2] == L'\\' || path[2] == L'/');
}

void BuildDefaultDataDir(wchar_t* buf, size_t cch) {
  buf[0] = L'\0';
  wchar_t appData[MAX_PATH];
  const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", appData, ARRAYSIZE(appData));
  if (n == 0 || n >= ARRAYSIZE(appData)) return;
  ::StringCchPrintfW(buf, cch, L"%s\\%s", appData, kDataSubDir);
}

BOOL CALLBACK InitDataDir(PINIT_ONCE, PVOID, PVOID*) {
  const HMODULE host = ::GetModuleHandleW(nullptr);
  const auto resolve = host ? reinterpret_cast<PFN_RESOLVEALIASPATH>(
                                  ::GetProcAddress(host, "ResolveAliasPath"))
                            : nullptr;
  wchar_t alias[64];
  wchar_t resolved[MAX_PATH];
  if (resolve &&
      SUCCEEDED(::StringCchPrintfW(alias, ARRAYSIZE(alias), L"/%s", kDataAliasName)) &&
      resolve(alias, resolved, static_cast<UINT>(ARRAYSIZE(resolved))) &&
      IsAbsoluteFilePath(resolved)) {
    ::StringCchCopyW(g_dataDir, ARRAYSIZE(g_dataDir), resolved);
    return TRUE;
  }
  BuildDefaultDataDir(g_dataDir, ARRAYSIZE(g_dataDir));
  return TRUE;
}

bool TryDecode(const char* data, int size, UINT codePage, DWORD flags, std::wstring* out) {
  const int need = ::MultiByteToWideChar(codePage, flags, data, size, nullptr, 0);
  if (need <= 0) return false;

  std::wstring text(static_cast<size_t>(need), L'\0');
  const int got = ::MultiByteToWideChar(codePage, flags, data, size, &text[0], need);
  if (got != need) return false;

  *out = std::move(text);
  return true;
}

}  // namespace

std::wstring UtilModuleDir(void) {
  HMODULE hmod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&UtilModuleDir), &hmod)) {
    return std::wstring();
  }

  wchar_t szPath[MAX_PATH * 2];
  const DWORD n = ::GetModuleFileNameW(hmod, szPath, ARRAYSIZE(szPath));
  if (n == 0 || n >= ARRAYSIZE(szPath)) return std::wstring();

  std::wstring dir(szPath, n);
  const size_t pos = dir.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return std::wstring();
  dir.resize(pos);
  return dir;
}

std::wstring UtilWebDir(void) {
  const std::wstring moduleDir = UtilModuleDir();
  if (moduleDir.empty()) return std::wstring();
  return UtilJoin(moduleDir, L"DopusMdViewer");
}

void UtilDataDirInto(wchar_t* buf, size_t cch) {
  if (!buf || cch == 0) return;
  ::InitOnceExecuteOnce(&g_dataDirOnce, InitDataDir, nullptr, nullptr);
  ::StringCchCopyW(buf, cch, g_dataDir);
}

std::wstring UtilDataDir(void) {
  wchar_t buf[MAX_PATH];
  UtilDataDirInto(buf, ARRAYSIZE(buf));
  return std::wstring(buf);
}

std::wstring UtilJoin(const std::wstring& a, const wchar_t* b) {
  std::wstring out = a;
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out += L'\\';
  if (b) out += b;
  return out;
}

bool UtilEnsureDir(const std::wstring& dir) {
  if (dir.empty()) return false;

  if (::CreateDirectoryW(dir.c_str(), nullptr)) return true;

  const DWORD err = ::GetLastError();
  if (err == ERROR_ALREADY_EXISTS) return true;

  if (err == ERROR_PATH_NOT_FOUND) {
    // 逐级创建父目录后重试一次
    const std::wstring parent = UtilParentDir(dir);
    if (!parent.empty() && UtilEnsureDir(parent)) {
      return ::CreateDirectoryW(dir.c_str(), nullptr) != FALSE ||
             ::GetLastError() == ERROR_ALREADY_EXISTS;
    }
  }
  return false;
}

bool UtilFileExists(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attr = ::GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool UtilReadFileBytes(const std::wstring& path, size_t maxBytes, std::string* out) {
  if (!out) return false;

  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER size = {};
  if (!::GetFileSizeEx(h, &size)) {
    ::CloseHandle(h);
    return false;
  }
  if (size.QuadPart < 0 || static_cast<ULONGLONG>(size.QuadPart) > maxBytes) {
    ::CloseHandle(h);
    return false;
  }

  std::string buf(static_cast<size_t>(size.QuadPart), '\0');
  if (!buf.empty()) {
    DWORD read = 0;
    if (!::ReadFile(h, &buf[0], static_cast<DWORD>(buf.size()), &read, nullptr) ||
        read != buf.size()) {
      ::CloseHandle(h);
      return false;
    }
  }

  ::CloseHandle(h);
  *out = std::move(buf);
  return true;
}

bool UtilDecodeText(const std::string& bytes, std::wstring* out) {
  const char* p = bytes.data();
  int size = static_cast<int>(bytes.size());
  if (size < 0) return false;

  if (size >= 3 && static_cast<unsigned char>(p[0]) == 0xEF &&
      static_cast<unsigned char>(p[1]) == 0xBB && static_cast<unsigned char>(p[2]) == 0xBF) {
    p += 3;
    size -= 3;
  }
  if (size == 0) {
    if (out) out->clear();
    return true;
  }

  std::wstring text;
  if (TryDecode(p, size, CP_UTF8, MB_ERR_INVALID_CHARS, &text)) {
    if (out) *out = std::move(text);
    return true;
  }
  if (TryDecode(p, size, 54936 /* GB18030 */, 0, &text)) {
    if (out) *out = std::move(text);
    return true;
  }
  return false;
}

std::wstring UtilParentDir(const std::wstring& path) {
  const size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return std::wstring();
  if (pos == 0) return path.substr(0, 1);
  return path.substr(0, pos);
}

const wchar_t* UtilLeafName(const wchar_t* path) {
  if (!path) return L"";
  const wchar_t* leaf = path;
  for (const wchar_t* p = path; *p; ++p) {
    if (*p == L'\\' || *p == L'/') leaf = p + 1;
  }
  return leaf;
}

std::wstring UtilFileVersion(const std::wstring& path) {
  if (path.empty()) return std::wstring();

  DWORD dummy = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &dummy);
  if (size == 0) return std::wstring();
  std::vector<unsigned char> buf(size);
  if (!::GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return std::wstring();

  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) {
    return std::wstring();
  }
  wchar_t text[64];
  ::StringCchPrintfW(text, ARRAYSIZE(text), L"%u.%u.%u.%u",
                     HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                     HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
  return text;
}

std::wstring UtilModuleVersion(void) {
  HMODULE hmod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&UtilModuleVersion), &hmod)) {
    return std::wstring();
  }
  wchar_t szPath[MAX_PATH * 2];
  const DWORD n = ::GetModuleFileNameW(hmod, szPath, ARRAYSIZE(szPath));
  if (n == 0 || n >= ARRAYSIZE(szPath)) return std::wstring();

  DWORD dummy = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(szPath, &dummy);
  if (size == 0) return std::wstring();
  std::vector<unsigned char> buf(size);
  if (!::GetFileVersionInfoW(szPath, 0, size, buf.data())) return std::wstring();

  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) {
    return std::wstring();
  }
  wchar_t text[64];
  ::StringCchPrintfW(text, ARRAYSIZE(text), L"%u.%u.%u",
                     HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                     HIWORD(info->dwFileVersionLS));
  return text;
}
