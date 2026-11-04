// plugin.cpp - 导出层。
//
// 只做转调：Opus 按名字找的入口函数。这里不解析 Markdown、不碰 WebView2。
// 建窗口全部发生在别处，这里只转交：DVP_CreateViewer → ViewerCreate、
// DVP_Configure → SettingsShow。

#include <windows.h>

#include <objidl.h>  // LPSTREAM，"viewer plugins.h" 需要（WIN32_LEAN_AND_MEAN 会藏掉）
#include <strsafe.h>

#include <string>
#include <vector>

#include "config.h"
#include "settings_dlg.h"
#include "log.h"
#include "util.h"
#include "viewer_window.h"
#include "webhost.h"
#include "viewer plugins.h"

namespace {

HINSTANCE g_hInstance = nullptr;

// Opus 每次刷新插件列表都会成对调用 Init/Uninit，计数让这件事在日志里可见。
volatile LONG g_initCalls = 0;
volatile LONG g_initExCalls = 0;
volatile LONG g_uninitCalls = 0;
volatile LONG g_createViewerCalls = 0;

DWORD g_dwOpusVerMajor = 13;
DWORD g_dwOpusVerMinor = 0;
HWND g_hwndOpusMsgWindow = nullptr;
wchar_t g_szLanguage[128] = L"(未提供)";

wchar_t g_szHandleExts[] = L".md;.markdown";
wchar_t g_szName[] = L"Markdown";
wchar_t g_szDescription[] = L"Markdown 预览（WebView2）";
wchar_t g_szCopyright[] = L"(c) 2026 DopusMdViewer contributors";
wchar_t g_szUrl[] = L"https://example.com/dopusmdviewer";

// {A7C1E4F2-3B6D-4E59-9C2A-1F8B7D0E5A31}
const GUID kPluginId = {0xA7C1E4F2, 0x3B6D, 0x4E59, {0x9C, 0x2A, 0x1F, 0x8B, 0x7D, 0x0E, 0x5A, 0x31}};

const wchar_t* LeafName(const wchar_t* pszPath) {
  if (!pszPath) return L"";
  const wchar_t* pszLeaf = pszPath;
  for (const wchar_t* p = pszPath; *p; ++p) {
    if (*p == L'\\' || *p == L'/') pszLeaf = p + 1;
  }
  return pszLeaf;
}

const wchar_t* ExtensionOf(const wchar_t* pszPath) {
  const wchar_t* pszLeaf = LeafName(pszPath);
  const wchar_t* pszExt = nullptr;
  for (const wchar_t* p = pszLeaf; *p; ++p) {
    if (*p == L'.') pszExt = p;
  }
  return pszExt ? pszExt : L"";
}

bool IsMarkdownExtension(const wchar_t* pszPath) {
  const wchar_t* pszExt = ExtensionOf(pszPath);
  return _wcsicmp(pszExt, L".md") == 0 || _wcsicmp(pszExt, L".markdown") == 0;
}

ULONGLONG FileSizeOf(const wchar_t* pszPath) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!pszPath || !*pszPath) return 0;
  if (!::GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad)) return 0;
  return (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

// 日志里写清"现在跑的是哪个 DLL、什么时候链接的"。换 DLL 之后先看这一行，
// 就不用猜 Opus 到底加载了哪一份。
void LogModuleStamp() {
  wchar_t szPath[MAX_PATH * 2];
  const DWORD n = ::GetModuleFileNameW(g_hInstance, szPath, ARRAYSIZE(szPath));
  if (n == 0 || n >= ARRAYSIZE(szPath)) return;

  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!::GetFileAttributesExW(szPath, GetFileExInfoStandard, &fad)) {
    LogLine(L"DLL: %s（读不到文件信息）", szPath);
    return;
  }

  SYSTEMTIME st = {};
  if (!::FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) return;

  const ULONGLONG size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  LogLine(L"DLL: %s 构建于 %04u-%02u-%02u %02u:%02u:%02u UTC，%llu 字节", szPath, st.wYear,
          st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, size);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved) {
  (void)lpReserved;

  if (dwReason == DLL_PROCESS_ATTACH) {
    g_hInstance = hInstance;
    // DllMain 期间持有加载锁，这里刻意不做任何文件 I/O。
    ::OutputDebugStringW(L"DopusMdViewer: DLL_PROCESS_ATTACH\r\n");
  } else if (dwReason == DLL_PROCESS_DETACH) {
    ::OutputDebugStringW(L"DopusMdViewer: DLL_PROCESS_DETACH\r\n");
  }

  return TRUE;
}

extern "C" BOOL DVP_Init(void) {
  const LONG n = ::InterlockedIncrement(&g_initCalls);
  ConfigGet();  // 先应用 enableLog，再写第一行日志
  LogLine(L"=== DVP_Init #%ld (pid=%lu) dll=%s ===", n,
          static_cast<unsigned long>(::GetCurrentProcessId()), UtilModuleVersion().c_str());
  return TRUE;
}

extern "C" BOOL DVP_InitEx(LPDVPINITEXDATA pInitExData) {
  const LONG n = ::InterlockedIncrement(&g_initExCalls);
  ConfigGet();  // DVP_Init 未调用时也要让日志开关先生效

  if (!pInitExData) {
    LogLine(L"=== DVP_InitEx #%ld 参数为 NULL ===", n);
    return TRUE;
  }

  g_hwndOpusMsgWindow = pInitExData->hwndDOpusMsgWindow;
  g_dwOpusVerMajor = pInitExData->dwOpusVerMajor;
  g_dwOpusVerMinor = pInitExData->dwOpusVerMinor;

  if (pInitExData->pszLanguageName && *pInitExData->pszLanguageName) {
    ::StringCchCopyW(g_szLanguage, ARRAYSIZE(g_szLanguage), pInitExData->pszLanguageName);
  }

  LogLine(L"=== DVP_InitEx #%ld opusVer=%lu.%lu language=\"%s\" pid=%lu ===", n,
          static_cast<unsigned long>(g_dwOpusVerMajor),
          static_cast<unsigned long>(g_dwOpusVerMinor), g_szLanguage,
          static_cast<unsigned long>(::GetCurrentProcessId()));
  LogModuleStamp();

  // 两路判据各记一次：Identify 的 VPINITF_USB 不一定被置位，SDK 的 IsUSBInstall()
  // 才是随时可问的那条。U 盘版出问题时先看这一行。
  LogLine(L"OPUS: isUsbInstall=%d usbMode=%d", UtilOpusIsUsbInstall(), UtilIsUsbMode() ? 1 : 0);

  // 启动后台预热（可在设置里关掉）。失败只在日志里出现，不影响预览。
  WebHostPrewarm(ConfigPrewarm(), ConfigIdleReleaseMinutes());
  return TRUE;
}

extern "C" void DVP_Uninit(void) {
  const LONG n = ::InterlockedIncrement(&g_uninitCalls);
  // 只取一次：两次调用之间可能有查看器在别的线程上创建/销毁，日志值与判断值
  // 不一致时最难查。
  const LONG live = ViewerLiveCount();

  LogLine(L"=== DVP_Uninit #%ld (init=#%ld initEx=#%ld) 存活查看器=%ld ===", n,
          ::InterlockedCompareExchange(&g_initCalls, 0, 0),
          ::InterlockedCompareExchange(&g_initExCalls, 0, 0), live);

  // Opus 卸载插件后可能 FreeLibrary：宿主/搜索线程还在跑就会执行已卸载的代码。
  // 但实测 Opus 在插件仍加载时也会频繁成对调 Init/Uninit（跨进程、甚至 init=0），
  // 还有查看器活着时绝不能收——收掉等于把正在显示的预览连锅端。
  if (live > 0) {
    LogLine(L"DVP_Uninit: 仍有 %ld 个查看器存活，保留预览引擎不收尾", live);
  } else {
    WebHostShutdown();
  }

  g_hwndOpusMsgWindow = nullptr;
}

extern "C" BOOL DVP_IdentifyW(LPVIEWERPLUGININFOW lpVPInfo) {
  if (!lpVPInfo) return FALSE;

  // cbSize / dwOpusVer* / dwInitFlags 都是 Opus 填给插件的 [in] 字段，只能读不能写：
  // 改写 cbSize 会让 Opus 按错的结构长度读回，改写 dwInitFlags 等于把 VPINITF_USB 抹掉。
  const UINT cbIn = lpVPInfo->cbSize;
  const bool hasV4 = cbIn >= VIEWERPLUGININFOW_V4_SIZE;
  if (hasV4) UtilSetUsbMode((lpVPInfo->dwInitFlags & VPINITF_USB) != 0);

  lpVPInfo->dwFlags = DVPFIF_ExtensionsOnly | DVPFIF_CanConfigure;
  lpVPInfo->dwVersionHigh = 0;
  lpVPInfo->dwVersionLow = 1;

  lpVPInfo->lpszHandleExts = g_szHandleExts;
  lpVPInfo->lpszName = g_szName;
  lpVPInfo->lpszDescription = g_szDescription;
  lpVPInfo->lpszCopyright = g_szCopyright;
  lpVPInfo->lpszURL = g_szUrl;

  lpVPInfo->cchHandleExtsMax = static_cast<UINT>(ARRAYSIZE(g_szHandleExts));
  lpVPInfo->cchNameMax = static_cast<UINT>(ARRAYSIZE(g_szName));
  lpVPInfo->cchDescriptionMax = static_cast<UINT>(ARRAYSIZE(g_szDescription));
  lpVPInfo->cchCopyrightMax = static_cast<UINT>(ARRAYSIZE(g_szCopyright));
  lpVPInfo->cchURLMax = static_cast<UINT>(ARRAYSIZE(g_szUrl));

  lpVPInfo->dwlMinFileSize = 0;
  lpVPInfo->dwlMaxFileSize = 0;  // 0 = 不设上限
  lpVPInfo->dwlMinPreviewFileSize = 0;
  lpVPInfo->dwlMaxPreviewFileSize = 0;

  lpVPInfo->uiMajorFileType = DVPMajorType_Text;
  lpVPInfo->idPlugin = kPluginId;
  if (hasV4) {
    lpVPInfo->hIconSmall = nullptr;
    lpVPInfo->hIconLarge = nullptr;
  }

  LogLine(L"DVP_IdentifyW -> TRUE exts=\"%s\" cbSize=%u usb=%d", g_szHandleExts, cbIn,
          UtilIsUsbMode() ? 1 : 0);
  return TRUE;
}

extern "C" BOOL DVP_IdentifyFileW(HWND hWnd, LPWSTR lpszName, LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                  HANDLE hAbortEvent) {
  (void)hWnd;
  (void)hAbortEvent;

  const bool bMatch = IsMarkdownExtension(lpszName);
  const ULONGLONG ullSize = FileSizeOf(lpszName);

  if (bMatch && lpVPFileInfo) {
    lpVPFileInfo->cbSize = VIEWERPLUGINFILEINFO_V4_SIZE;
    lpVPFileInfo->dwFlags = DVPFIF_CanReturnViewer;
    lpVPFileInfo->wMajorType = static_cast<WORD>(DVPMajorType_Text);
    lpVPFileInfo->wMinorType = 0;
    lpVPFileInfo->iTypeHint = DVPFITypeHint_PlainText;
    lpVPFileInfo->dwlFileSize = ullSize;
  }

  return bMatch ? TRUE : FALSE;
}

extern "C" HWND DVP_Configure(HWND hWndParent, HWND hWndNotify, DWORD dwNotifyData) {
  (void)hWndNotify;
  (void)dwNotifyData;
  LogLine(L"DVP_Configure parent=%p", hWndParent);
  HWND hwnd = SettingsShow(hWndParent);
  LogLine(L"DVP_Configure -> %p", hwnd);
  return hwnd;
}

extern "C" HWND DVP_CreateViewer(HWND hWndParent, LPRECT lpRc, DWORD dwFlags) {
  const LONG n = ::InterlockedIncrement(&g_createViewerCalls);
  LogLine(L"DVP_CreateViewer #%ld parent=%p flags=0x%08X", n, hWndParent,
          static_cast<unsigned>(dwFlags));

  if (!ViewerRegisterClass(g_hInstance)) {
    LogLine(L"DVP_CreateViewer -> NULL（窗口类注册失败）");
    return nullptr;
  }

  HWND hwnd = ViewerCreate(hWndParent, lpRc, dwFlags);
  LogLine(L"DVP_CreateViewer -> %p 存活=%ld", hwnd, ViewerLiveCount());
  return hwnd;
}

// U 盘导出时 Opus 会调这里：返回 TRUE 即声明「不在系统里留任何东西」，同时把
// 需要跟着插件走的文件（DLL 同级的资源目录）报给它。不导出这个函数就等于没申报，
// 导出到 U 盘时插件可能根本不会被带上。
extern "C" BOOL DVP_USBSafe(LPOPUSUSBSAFEDATA pUSBSafeData) {
  if (!pUSBSafeData) return FALSE;

  const UINT cch = pUSBSafeData->cchOtherExports;
  LogLine(L"DVP_USBSafe: 申报 USB 安全，导出缓冲 %u 字符", cch);
  if (!pUSBSafeData->pszOtherExports || cch == 0) return TRUE;

  std::vector<std::wstring> files;
  UtilUsbExportFiles(&files);

  // 双 null 结尾的名字列表；装不下就按整条截断，绝不写半个名字。
  size_t used = 0;
  size_t written = 0;
  for (const std::wstring& name : files) {
    const size_t need = name.size() + 1;  // 名字 + 结尾 NUL
    if (used + need + 1 > cch) break;     // 末位留给列表结尾的空串
    if (FAILED(::StringCchCopyW(pUSBSafeData->pszOtherExports + used, cch - used, name.c_str()))) {
      break;
    }
    used += need;
    ++written;
  }
  pUSBSafeData->pszOtherExports[used] = L'\0';

  LogLine(L"DVP_USBSafe: 声明 %zu/%zu 个文件%s", written, files.size(),
          written < files.size() ? L"（缓冲装不下，已截断）" : L"");
  return TRUE;
}
