// host_shim.cpp - 冒充 dopus.exe 的 ResolveAliasPath。
//
// UtilDataDir 现在只走 DOpus 别名（/DopusMdViewerData → /dopusdata），
// unittest.exe 独立进程里没有宿主导出，配置读写测试会拿不到数据目录。
// 这里按 main 重定向过的 APPDATA 解析 /dopusdata，让隔离区重新生效；
// /DopusMdViewerData 一律不认，避免真实用户的自定义别名混进测试。

#include <windows.h>
#include <strsafe.h>

#include <string>

namespace {

bool EnsureDirRecursive(const std::wstring& dir) {
  if (::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  const size_t pos = dir.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return false;
  if (!EnsureDirRecursive(dir.substr(0, pos))) return false;
  return ::CreateDirectoryW(dir.c_str(), nullptr) != FALSE ||
         ::GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

extern "C" __declspec(dllexport) BOOL WINAPI ResolveAliasPath(LPCWSTR alias, LPWSTR out,
                                                             UINT cch) {
  if (!alias || !out || cch == 0) return FALSE;
  if (::lstrcmpiW(alias, L"/dopusdata") != 0) return FALSE;

  wchar_t appData[MAX_PATH];
  const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", appData, ARRAYSIZE(appData));
  if (n == 0 || n >= ARRAYSIZE(appData)) return FALSE;

  // 与 DOpus 的 /dopusdata 同形；UtilDataDir 会检查解析结果真实存在。
  // main 只重定向环境变量不建目录，这里逐级建（APPDATA 的父目录是 %TEMP%，已在）。
  const std::wstring base(appData);
  const std::wstring vendor = base + L"\\GPSoftware";
  const std::wstring root = vendor + L"\\Directory Opus";
  if (!EnsureDirRecursive(root)) return FALSE;
  return ::StringCchCopyW(out, cch, root.c_str()) == S_OK;
}
