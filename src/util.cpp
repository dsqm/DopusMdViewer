#include "util.h"

#include <strsafe.h>

namespace {

// 与 log.cpp 的 kDataSubDir 保持一致：日志目录由 log.cpp 自己算（它刻意不依赖
// 标准库，避免在 CRT 就绪前被调用），这里是同一路径的第二份定义。
const wchar_t kDataSubDir[] = L"GPSoftware\\Directory Opus\\User Data\\DopusMdViewer";

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

std::wstring UtilDataDir(void) {
  wchar_t szAppData[MAX_PATH];
  const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", szAppData, ARRAYSIZE(szAppData));
  if (n == 0 || n >= ARRAYSIZE(szAppData)) return std::wstring();

  std::wstring dir(szAppData);
  dir += L'\\';
  dir += kDataSubDir;
  return dir;
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
