// plugin.cpp - 导出层。
//
// 只做转调：Opus 按名字找的六个入口 + 识别函数。这里不解析 Markdown、
// 不碰 WebView2、不建窗口（唯一例外是 DVP_CreateViewer 的转交）。

#include <windows.h>

#include <objidl.h>  // LPSTREAM，"viewer plugins.h" 需要（WIN32_LEAN_AND_MEAN 会藏掉）
#include <strsafe.h>

#include "config.h"
#include "settings_dlg.h"
#include "log.h"
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
  LogLine(L"=== DVP_Init #%ld (pid=%lu) log=%s ===", n,
          static_cast<unsigned long>(::GetCurrentProcessId()), LogLastPath());
  return TRUE;
}

extern "C" BOOL DVP_InitEx(LPDVPINITEXDATA pInitExData) {
  const LONG n = ::InterlockedIncrement(&g_initExCalls);

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

  // 启动后台预热（可在设置里关掉）。失败只在日志里出现，不影响预览。
  WebHostPrewarm(ConfigPrewarm());
  return TRUE;
}

extern "C" void DVP_Uninit(void) {
  const LONG n = ::InterlockedIncrement(&g_uninitCalls);
  const LONG live = ViewerLiveCount();

  LogLine(L"=== DVP_Uninit #%ld (init=#%ld initEx=#%ld) 存活查看器=%ld ===", n,
          ::InterlockedCompareExchange(&g_initCalls, 0, 0),
          ::InterlockedCompareExchange(&g_initExCalls, 0, 0), live);

  g_hwndOpusMsgWindow = nullptr;
}

extern "C" BOOL DVP_IdentifyW(LPVIEWERPLUGININFOW lpVPInfo) {
  if (!lpVPInfo) return FALSE;

  lpVPInfo->cbSize = VIEWERPLUGININFOW_V4_SIZE;
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
  lpVPInfo->dwOpusVerMajor = g_dwOpusVerMajor;
  lpVPInfo->dwOpusVerMinor = g_dwOpusVerMinor;
  lpVPInfo->dwInitFlags = 0;
  lpVPInfo->hIconSmall = nullptr;
  lpVPInfo->hIconLarge = nullptr;

  LogLine(L"DVP_IdentifyW -> TRUE exts=\"%s\"", g_szHandleExts);
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
  return SettingsShow(hWndParent);
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
