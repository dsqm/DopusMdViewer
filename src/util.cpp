#include "util.h"

#include <strsafe.h>
#include <string.h>  // _wcsicmp：扩展名白名单的大小写不敏感比较
#include <winver.h>

#include <vector>

namespace {

// 数据根 = Opus 自己的数据根 + 固定子路径。走 /dopusdata 内置别名而不是
// %APPDATA%：普通安装两者等价，U 盘版只有别名指向 U 盘，环境变量永远指向宿主机。
// /DopusMdViewerData 是留给用户的自定义覆盖，优先级最高。
const wchar_t kDataSubPath[] = L"User Data\\DopusMdViewer";
const wchar_t kOpusDataAlias[] = L"dopusdata";

// dopus.exe 的未公开导出；本机所有内置 Viewer 也用 GetProcAddress 按名字取它。
using PFN_RESOLVEALIASPATH = BOOL(WINAPI*)(LPCWSTR, LPWSTR, UINT);

// 解析成功一次就固定下来；失败**不缓存**——DVP_Init 可能早于别名系统就绪，
// 那一刻失败不代表后面也失败，留着下次调用重试比一辈子用错目录强。
SRWLOCK g_dataLock = SRWLOCK_INIT;
bool g_dataReady = false;
wchar_t g_dataDir[MAX_PATH] = L"";

// U 盘模式：DVP_IdentifyW 从 dwInitFlags 的 VPINITF_USB 读到，供缓存目录选择使用。
volatile LONG g_usbMode = 0;

bool IsAbsoluteFilePath(const wchar_t* path) {
  if (!path || !path[0]) return false;
  if (path[0] == L'\\' && path[1] == L'\\') return true;  // UNC、\\?\ 长路径
  const bool drive = ((path[0] >= L'A' && path[0] <= L'Z') ||
                      (path[0] >= L'a' && path[0] <= L'z')) &&
                     path[1] == L':';
  return drive && (path[2] == L'\\' || path[2] == L'/');
}

// 用 dopus.exe 的 ResolveAliasPath 解析 "/<name>"。成功返回绝对路径。
bool ResolveAlias(const wchar_t* name, wchar_t* out, size_t cch) {
  const HMODULE host = ::GetModuleHandleW(nullptr);
  const auto resolve = host ? reinterpret_cast<PFN_RESOLVEALIASPATH>(
                                  ::GetProcAddress(host, "ResolveAliasPath"))
                            : nullptr;
  if (!resolve) return false;

  wchar_t alias[64];
  if (FAILED(::StringCchPrintfW(alias, ARRAYSIZE(alias), L"/%s", name))) return false;

  wchar_t resolved[MAX_PATH];
  resolved[0] = L'\0';
  if (!resolve(alias, resolved, static_cast<UINT>(ARRAYSIZE(resolved)))) return false;
  if (!IsAbsoluteFilePath(resolved)) return false;
  return SUCCEEDED(::StringCchCopyW(out, cch, resolved));
}

// 两级：/DopusMdViewerData → /dopusdata\User Data\DopusMdViewer。都失败返回 false。
bool TryResolveDataDir(wchar_t* out, size_t cch) {
  // 用户覆盖别名：只要解析出绝对路径就认，目录本身由使用方按需创建。
  if (ResolveAlias(kDataAliasName, out, cch)) return true;

  wchar_t root[MAX_PATH];
  if (!ResolveAlias(kOpusDataAlias, root, ARRAYSIZE(root))) return false;
  // 根必须真实存在：解析出来的路径不存在，多半是别名系统还没就绪，留给下次重试。
  if (!UtilDirExists(root)) return false;

  wchar_t sub[MAX_PATH];
  if (FAILED(::StringCchPrintfW(sub, ARRAYSIZE(sub), L"%s\\%s", root, kDataSubPath))) {
    return false;
  }
  return SUCCEEDED(::StringCchCopyW(out, cch, sub));
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

namespace {

// 递归收集目录下的文件；rel 是相对 Viewers\ 的前缀，空目录直接跳过。
void CollectExportFiles(const std::wstring& dir, const std::wstring& rel,
                        std::vector<std::wstring>* out) {
  WIN32_FIND_DATAW fd = {};
  HANDLE h = ::FindFirstFileW(UtilJoin(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    const std::wstring child = UtilJoin(dir, fd.cFileName);
    const std::wstring childRel = UtilJoin(rel, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      CollectExportFiles(child, childRel, out);
    } else {
      out->push_back(childRel);
    }
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
}

}  // namespace

void UtilUsbExportFiles(std::vector<std::wstring>* out) {
  if (!out) return;
  out->clear();
  const std::wstring root = UtilWebDir();
  if (root.empty() || !UtilDirExists(root)) return;
  // 资源目录与插件同名，正好是相对 Viewers\ 的第一层。
  CollectExportFiles(root, L"DopusMdViewer", out);
}

namespace {

// Plugin Support API 的导出（SDK 的 "plugin support.h"）：dopus.exe 直接导出，
// 和 ResolveAliasPath 一样按名取，但这个写进了 SDK 文档。
using PFN_ISUSBINSTALL = BOOL(WINAPI*)(void);

}  // namespace

int UtilOpusIsUsbInstall(void) {
  static const auto isUsb =
      reinterpret_cast<PFN_ISUSBINSTALL>([] {
        const HMODULE host = ::GetModuleHandleW(nullptr);
        return host ? ::GetProcAddress(host, "IsUSBInstall") : nullptr;
      }());
  if (!isUsb) return -1;  // 取不到：未知，别当成「不是 U 盘」
  return isUsb() ? 1 : 0;
}

void UtilDataDirInto(wchar_t* buf, size_t cch) {
  if (!buf || cch == 0) return;

  ::AcquireSRWLockExclusive(&g_dataLock);
  if (!g_dataReady) {
    wchar_t resolved[MAX_PATH];
    if (TryResolveDataDir(resolved, ARRAYSIZE(resolved))) {
      ::StringCchCopyW(g_dataDir, ARRAYSIZE(g_dataDir), resolved);
      g_dataReady = true;
    }
  }
  ::StringCchCopyW(buf, cch, g_dataDir);
  ::ReleaseSRWLockExclusive(&g_dataLock);
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

std::wstring UtilTrim(const std::wstring& text) {
  size_t a = 0;
  size_t b = text.size();
  while (a < b && (text[a] == L' ' || text[a] == L'\t' || text[a] == L'\r' || text[a] == L'\n')) {
    ++a;
  }
  while (b > a &&
         (text[b - 1] == L' ' || text[b - 1] == L'\t' || text[b - 1] == L'\r' ||
          text[b - 1] == L'\n')) {
    --b;
  }
  return text.substr(a, b - a);
}

std::wstring UtilExpandEnv(const std::wstring& text) {
  if (text.empty()) return text;
  const DWORD need = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
  if (need == 0) return text;
  std::wstring out(need, L'\0');
  const DWORD got = ::ExpandEnvironmentStringsW(text.c_str(), &out[0], need);
  if (got == 0 || got > need) return text;
  out.resize(got - 1);
  return out;
}

bool UtilIsLocalAbsolutePath(const std::wstring& path) {
  if (path.size() < 3) return false;
  if (!((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) {
    return false;
  }
  if (path[1] != L':') return false;
  return path[2] == L'\\' || path[2] == L'/';
}

std::wstring UtilNormalizeLocalDir(const std::wstring& text) {
  std::wstring s = UtilTrim(UtilExpandEnv(text));
  if (s.empty() || !UtilIsLocalAbsolutePath(s)) return std::wstring();
  for (wchar_t& ch : s) {
    if (ch == L'/') ch = L'\\';
  }

  const DWORD need = ::GetFullPathNameW(s.c_str(), 0, nullptr, nullptr);
  if (need == 0) return std::wstring();
  std::wstring out(need, L'\0');
  const DWORD got = ::GetFullPathNameW(s.c_str(), need, &out[0], nullptr);
  if (got == 0 || got >= need) return std::wstring();
  out.resize(got);

  while (out.size() > 3 && out.back() == L'\\') out.pop_back();
  if (!UtilIsLocalAbsolutePath(out)) return std::wstring();
  return out;
}

std::wstring UtilDefaultWebView2BaseDir(void) {
  wchar_t localAppData[MAX_PATH];
  const DWORD n =
      ::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
  if (n > 0 && n < ARRAYSIZE(localAppData)) {
    return UtilJoin(std::wstring(localAppData, n), L"DopusMdViewer");
  }
  return UtilDataDir();
}

// %TEMP%\DopusMdViewer。GetTempPathW 自带尾随反斜杠，UtilJoin 会吃掉。
std::wstring UtilTempWebView2BaseDir(void) {
  wchar_t tempPath[MAX_PATH];
  const DWORD n = ::GetTempPathW(ARRAYSIZE(tempPath), tempPath);
  if (n == 0 || n >= ARRAYSIZE(tempPath)) return std::wstring();
  return UtilJoin(std::wstring(tempPath, n), L"DopusMdViewer");
}

void UtilSetUsbMode(bool usb) { ::InterlockedExchange(&g_usbMode, usb ? 1 : -1); }

namespace {

// Opus 程序目录（本 DLL 在 <程序目录>\Viewers\）是否在可移动盘上。
bool ProgramDirIsRemovable() {
  const std::wstring dir = UtilParentDir(UtilModuleDir());
  if (dir.size() < 3 || dir[1] != L':') return false;
  const wchar_t root[4] = {dir[0], dir[1], dir[2], L'\0'};
  return ::GetDriveTypeW(root) == DRIVE_REMOVABLE;
}

}  // namespace

bool UtilIsUsbMode(void) {
  const LONG cached = ::InterlockedCompareExchange(&g_usbMode, 0, 0);
  if (cached != 0) return cached > 0;

  // 两条判据取并集：
  //  1. SDK 的 IsUSBInstall()——Opus 自己说「从 U 盘运行」；
  //  2. Opus 程序目录在可移动盘上——手工把插件/程序拷到 U 盘时，第 1 条不一定为真。
  // 只看 Identify 的 VPINITF_USB 不够：那一位 Opus 未必置位，而且 Identify 可能晚于
  // 第一次需要缓存路径的时刻。
  const bool usb = (UtilOpusIsUsbInstall() == 1) || ProgramDirIsRemovable();
  ::InterlockedExchange(&g_usbMode, usb ? 1 : -1);
  return usb;
}

namespace {

// 网络盘直接排除：WebView2 明确说不要把用户数据放 network drive（慢、可能崩、可能丢）。
bool IsRemotePath(const std::wstring& path) {
  if (path.size() < 3 || path[1] != L':') return false;
  const wchar_t root[4] = {path[0], path[1], path[2], L'\0'};
  return ::GetDriveTypeW(root) == DRIVE_REMOTE;
}

// 能不能真拿来当 WebView2 的用户数据目录：建得出来 + 写得进去。
// 只查存在性是靠不住的——配额满、ACL 只给读、磁盘只读都会在 WebView2 启动时才炸。
bool IsUsableCacheBase(const std::wstring& dir) {
  if (dir.empty() || IsRemotePath(dir)) return false;
  if (!UtilEnsureDir(dir)) return false;

  wchar_t name[64];
  if (FAILED(::StringCchPrintfW(name, ARRAYSIZE(name), L".mdv-probe-%llu",
                                static_cast<unsigned long long>(::GetTickCount64())))) {
    return false;
  }
  const std::wstring probe = UtilJoin(dir, name);
  HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  ::CloseHandle(h);
  return true;
}

// 自动选路：TEMP → 数据目录（U 盘）→ 本机 LOCALAPPDATA。进程内只探一次。
SRWLOCK g_cacheBaseLock = SRWLOCK_INIT;
bool g_cacheBaseReady = false;
wchar_t g_cacheBase[MAX_PATH] = L"";
wchar_t g_cacheBaseSource[8] = L"";

// 调用方必须已持有 g_cacheBaseLock（独占）。
void ResolveCacheBaseLocked() {
  if (g_cacheBaseReady) return;

  g_cacheBase[0] = L'\0';
  g_cacheBaseSource[0] = L'\0';

  const std::wstring temp = UtilTempWebView2BaseDir();
  if (IsUsableCacheBase(temp)) {
    ::StringCchCopyW(g_cacheBase, ARRAYSIZE(g_cacheBase), temp.c_str());
    ::StringCchCopyW(g_cacheBaseSource, ARRAYSIZE(g_cacheBaseSource), L"temp");
  } else if (IsUsableCacheBase(UtilDataDir())) {
    ::StringCchCopyW(g_cacheBase, ARRAYSIZE(g_cacheBase), UtilDataDir().c_str());
    ::StringCchCopyW(g_cacheBaseSource, ARRAYSIZE(g_cacheBaseSource), L"data");
  } else {
    const std::wstring local = UtilNormalizeLocalDir(UtilDefaultWebView2BaseDir());
    if (!local.empty()) {
      ::StringCchCopyW(g_cacheBase, ARRAYSIZE(g_cacheBase), local.c_str());
      ::StringCchCopyW(g_cacheBaseSource, ARRAYSIZE(g_cacheBaseSource), L"local");
    }
  }
  g_cacheBaseReady = true;
}

}  // namespace

std::wstring UtilWebView2BaseDir(const std::wstring& configuredBase) {
  // 用户指定优先（两种模式都一样）：设置页落盘前已经校验过可写。
  const std::wstring base = UtilNormalizeLocalDir(configuredBase);
  if (!base.empty()) return base;

  // 非 U 盘：沿用本机缓存。
  if (!UtilIsUsbMode()) return UtilNormalizeLocalDir(UtilDefaultWebView2BaseDir());

  ::AcquireSRWLockExclusive(&g_cacheBaseLock);
  ResolveCacheBaseLocked();
  const std::wstring out(g_cacheBase);
  ::ReleaseSRWLockExclusive(&g_cacheBaseLock);
  return out;
}

std::wstring UtilWebView2BaseSource(const std::wstring& configuredBase) {
  if (!UtilNormalizeLocalDir(configuredBase).empty()) return L"config";
  if (!UtilIsUsbMode()) return L"default";

  ::AcquireSRWLockExclusive(&g_cacheBaseLock);
  ResolveCacheBaseLocked();
  const std::wstring out(g_cacheBaseSource);
  ::ReleaseSRWLockExclusive(&g_cacheBaseLock);
  return out;
}

std::wstring UtilWebView2UserDataDir(const std::wstring& configuredBase) {
  const std::wstring base = UtilWebView2BaseDir(configuredBase);
  if (base.empty()) return std::wstring();
  return UtilJoin(base, L"WebView2");
}

std::wstring UtilWebView2SettingsDir(const std::wstring& configuredBase) {
  const std::wstring base = UtilWebView2BaseDir(configuredBase);
  if (base.empty()) return std::wstring();
  return UtilJoin(base, L"WebView2-Settings");
}

bool UtilDirExists(const std::wstring& dir) {
  if (dir.empty()) return false;
  const DWORD attr = ::GetFileAttributesW(dir.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool UtilIsWebView2UserDataDir(const std::wstring& path) {
  std::wstring s = UtilTrim(path);
  while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  if (!UtilIsLocalAbsolutePath(s)) return false;
  const size_t pos = s.find_last_of(L"\\/");
  if (pos == std::wstring::npos || pos + 1 >= s.size()) return false;
  return ::_wcsicmp(s.c_str() + pos + 1, L"WebView2") == 0;
}

uint64_t UtilDirSize(const std::wstring& dir, uint64_t stopAt) {
  if (dir.empty()) return 0;

  uint64_t total = 0;
  WIN32_FIND_DATAW fd = {};
  HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;

  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    const std::wstring full = UtilJoin(dir, fd.cFileName);
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        total += UtilDirSize(full, stopAt);
      }
    } else {
      total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    }
    if (stopAt != 0 && total >= stopAt) break;
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  return total;
}

bool UtilRemoveTree(const std::wstring& dir) {
  if (dir.empty()) return false;

  const DWORD attr = ::GetFileAttributesW(dir.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return true;

  if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    if (::DeleteFileW(dir.c_str())) return true;
    ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    return ::DeleteFileW(dir.c_str()) != FALSE;
  }

  if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return ::RemoveDirectoryW(dir.c_str()) != FALSE;
  }

  bool ok = true;
  WIN32_FIND_DATAW fd = {};
  HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = ::GetLastError();
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_NO_MORE_FILES) ok = false;
  } else {
    do {
      if (fd.cFileName[0] == L'.' &&
          (fd.cFileName[1] == L'\0' ||
           (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
        continue;
      }
      if (!UtilRemoveTree(UtilJoin(dir, fd.cFileName))) ok = false;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
  }

  if (!::RemoveDirectoryW(dir.c_str())) {
    ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!::RemoveDirectoryW(dir.c_str())) ok = false;
  }
  return ok;
}

std::wstring UtilFormatSize(uint64_t bytes) {
  wchar_t buf[64];
  if (bytes >= (1ull << 30)) {
    ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%.1f GB",
                       static_cast<double>(bytes) / 1073741824.0);
  } else if (bytes >= (1ull << 20)) {
    ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%.1f MB",
                       static_cast<double>(bytes) / 1048576.0);
  } else if (bytes >= (1ull << 10)) {
    ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%llu B",
                       static_cast<unsigned long long>(bytes));
  }
  return buf;
}

std::wstring UtilWin32ErrorText(DWORD code) {
  if (code == 0) return std::wstring();

  wchar_t* raw = nullptr;
  const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
  if (n == 0 || !raw) return std::wstring();

  std::wstring text(raw, n);
  ::LocalFree(raw);

  // 系统消息自带 "\r\n"，拼进弹窗会多出空行。
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
    text.pop_back();
  }
  return text;
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
  // 版本号是构建时刻的年.月.日.时分（见 tools\gen-version.ps1）：月日与时分补零，
  // 和资源管理器属性页显示的一致。UtilFileVersion 不带补零，那是给 WebView2 运行时
  // 那种点分版本号用的，两者不能合并。
  ::StringCchPrintfW(text, ARRAYSIZE(text), L"%u.%02u.%02u.%04u",
                     HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                     HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
  return text;
}

// ---- Opus 独立查看器 ----
// 插件在 Opus 进程里跑，宿主 exe 就是 dopus.exe；dopusrt.exe 与它同目录。
// Viewer Plugin SDK 没有「让 Opus 打开文件」的接口，官方对外只有 dopusrt 命令行：
//   dopusrt.exe /show "<file>"   用独立查看器显示文件
//   dopusrt.exe /vcmd ...        给当前查看器发命令
// 这里只做前者：起一个 dopusrt 就返回，不等它（它转手给 Opus 就退出了）。
namespace {

const wchar_t kViewerHostExe[] = L"dopusrt.exe";

// 扩展名白名单：Opus 查看器插件实际覆盖的类型。名单外的（exe、压缩包、无扩展名）
// 交回系统默认程序——Opus 对它们只会弹「无法显示」。顺序与 Viewers\ 下的插件对应：
// opuswic/WebP/gifanim（常见图片）、dcrawrap（相机 RAW）、j2k（JPEG 2000）、
// pcx/targa（旧图像格式）、text（文本与代码）、docs（Office）、movie（视频）、
// wav/wma（音频）、pdf。
const wchar_t* const kViewerExts[] = {
    // 常见图片
    L".png", L".jpg", L".jpeg", L".jpe", L".gif", L".bmp", L".webp", L".ico", L".tif",
    L".tiff", L".svg", L".avif", L".heic", L".psd", L".emf", L".wmf",
    // 相机 RAW（dcrawrap）
    L".cr2", L".cr3", L".nef", L".arw", L".dng", L".orf", L".rw2", L".raf", L".srw",
    L".pef", L".x3f",
    // JPEG 2000 与旧图像格式
    L".jp2", L".j2k", L".jpf", L".jpx", L".pcx", L".tga",
    // 文本与代码（text 插件）
    L".md", L".markdown", L".txt", L".log", L".json", L".xml", L".csv", L".tsv", L".ini",
    L".yaml", L".yml", L".html", L".htm", L".css", L".js", L".mjs", L".ts", L".c", L".h",
    L".cpp", L".hpp", L".cs", L".py", L".rs", L".go", L".java", L".sh", L".bat", L".ps1",
    L".sql", L".toml", L".cfg", L".conf",
    // Office（docs 插件）
    L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp",
    L".rtf",
    // 视频与音频（movie / wav / wma）
    L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".m4v", L".mpg", L".mpeg",
    L".flv", L".mp3", L".wav", L".flac", L".ogg", L".m4a", L".wma", L".aac",
    L".ape", L".opus",
    // 文档
    L".pdf",
};

std::wstring DirOfModule(HMODULE hmod) {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = ::GetModuleFileNameW(hmod, buf, ARRAYSIZE(buf));
  if (n == 0 || n >= ARRAYSIZE(buf)) return std::wstring();
  std::wstring path(buf, n);
  const size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return std::wstring();
  path.resize(pos);
  return path;
}

// 候选路径里有 dopusrt.exe 就返回它，否则空串。
std::wstring ProbeViewerIn(const std::wstring& dir) {
  if (dir.empty()) return std::wstring();
  const std::wstring exe = UtilJoin(dir, kViewerHostExe);
  return UtilFileExists(exe) ? exe : std::wstring();
}

}  // namespace

std::wstring UtilOpusViewerExe(void) {
  // 1) 宿主 exe 同目录：插件由 dopus.exe 加载，这条几乎总是命中（含便携版）。
  std::wstring exe = ProbeViewerIn(DirOfModule(nullptr));
  if (!exe.empty()) return exe;

  // 2) DLL 的上级目录：DLL 在 <Opus>\Viewers\ 下，父目录就是 Opus 根。
  const std::wstring moduleDir = UtilModuleDir();
  if (!moduleDir.empty()) {
    exe = ProbeViewerIn(UtilParentDir(moduleDir));
    if (!exe.empty()) return exe;
  }

  // 3) 安装版默认位置。便携版没有注册表项，只能靠前两条。
  wchar_t programFiles[MAX_PATH];
  const DWORD n = ::GetEnvironmentVariableW(L"ProgramFiles", programFiles,
                                            ARRAYSIZE(programFiles));
  if (n > 0 && n < ARRAYSIZE(programFiles)) {
    exe = ProbeViewerIn(UtilJoin(programFiles, L"GPSoftware\\Directory Opus"));
    if (!exe.empty()) return exe;
  }
  return std::wstring();
}

bool UtilOpusViewerShow(const std::wstring& path) {
  if (path.empty() || !UtilFileExists(path)) return false;
  const std::wstring exe = UtilOpusViewerExe();
  if (exe.empty()) return false;

  // 两个路径都可能带空格（便携版在 Downloads 下，文档名更常见）：一律加引号。
  std::wstring cmd = L"\"" + exe + L"\" /show \"" + path + L"\"";
  std::vector<wchar_t> line(cmd.begin(), cmd.end());
  line.push_back(L'\0');

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &si, &pi)) {
    return false;
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

std::wstring UtilOpusViewerDefaultExts(void) {
  static const std::wstring kDefault = [] {
    std::wstring out;
    for (const wchar_t* ext : kViewerExts) {
      if (!out.empty()) out += L';';
      out += ext;
    }
    return out;
  }();
  return kDefault;
}

namespace {

// 名单里是否含这个扩展名。分隔符宽松（分号/逗号/空白），项可带点可不带。
bool ExtListContains(const std::wstring& list, const wchar_t* ext) {
  std::wstring token;
  for (size_t i = 0; i <= list.size(); ++i) {
    const wchar_t ch = i < list.size() ? list[i] : L';';
    const bool sep = ch == L';' || ch == L',' || ch == L' ' || ch == L'\t' || ch == L'\r' ||
                     ch == L'\n';
    if (!sep) {
      token += ch;
      continue;
    }
    if (!token.empty()) {
      if (token[0] != L'.') token.insert(token.begin(), L'.');
      if (::_wcsicmp(token.c_str(), ext) == 0) return true;
      token.clear();
    }
  }
  return false;
}

}  // namespace

bool UtilOpusViewerHandlesExt(const wchar_t* ext, const std::wstring& list) {
  if (!ext || ext[0] != L'.') return false;
  return ExtListContains(list.empty() ? UtilOpusViewerDefaultExts() : list, ext);
}
