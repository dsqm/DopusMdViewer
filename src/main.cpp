// main.cpp - Directory Opus 13 viewer plugin, POC skeleton.
//
// Responsibilities of this translation unit:
//   * the six exported entry points Opus looks up by name
//   * DVP_IdentifyW / DVP_IdentifyFileW (what the plugin claims to handle)
//   * DVP_CreateViewer (hands off to the window layer in viewer.cpp)
// Nothing here parses Markdown or renders anything; the goal is to prove the
// ABI and to leave a complete trace in %TEMP%\DopusMdViewer.log.

#include <windows.h>

#include <objidl.h>  // LPSTREAM, needed by "viewer plugins.h" (WIN32_LEAN_AND_MEAN hides it)
#include <strsafe.h>
#include <wchar.h>

#include "log.h"
#include "viewer.h"
#include "viewer plugins.h"

namespace {

HINSTANCE g_hInstance = nullptr;

// Opus may call Init/Uninit several times per process (plugin list refresh).
// The counters make that visible in the log instead of silently hiding it.
volatile LONG g_initCalls = 0;
volatile LONG g_initExCalls = 0;
volatile LONG g_uninitCalls = 0;
volatile LONG g_identifyCalls = 0;
volatile LONG g_identifyFileCalls = 0;
volatile LONG g_createViewerCalls = 0;

DWORD g_dwOpusVerMajor = 13;
DWORD g_dwOpusVerMinor = 0;
HWND g_hwndOpusMsgWindow = nullptr;
wchar_t g_szLanguage[128] = L"(not supplied)";

// Static buffers, so the cchXxxMax values below are the real buffer sizes.
wchar_t g_szHandleExts[] = L".md;.markdown";
wchar_t g_szName[] = L"Markdown";
wchar_t g_szDescription[] = L"DopusMdViewer (POC)";
wchar_t g_szCopyright[] = L"(c) 2026 DopusMdViewer contributors";
wchar_t g_szUrl[] = L"https://example.com/dopusmdviewer";

// {A7C1E4F2-3B6D-4E59-9C2A-1F8B7D0E5A31}
const GUID kPluginId = {0xA7C1E4F2,
                        0x3B6D,
                        0x4E59,
                        {0x9C, 0x2A, 0x1F, 0x8B, 0x7D, 0x0E, 0x5A, 0x31}};

const wchar_t* LeafName(const wchar_t* pszPath) {
  if (!pszPath) return L"";
  const wchar_t* pszLeaf = pszPath;
  for (const wchar_t* p = pszPath; *p; ++p) {
    if (*p == L'\\' || *p == L'/') pszLeaf = p + 1;
  }
  return pszLeaf;
}

// Extension after the last dot in the leaf name, or L"" if there is none.
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

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved) {
  (void)lpReserved;

  if (dwReason == DLL_PROCESS_ATTACH) {
    g_hInstance = hInstance;
    // Deliberately no file I/O here: the loader lock is held during DllMain.
    ::OutputDebugStringW(L"DopusMdViewer: DLL_PROCESS_ATTACH\r\n");
  } else if (dwReason == DLL_PROCESS_DETACH) {
    ::OutputDebugStringW(L"DopusMdViewer: DLL_PROCESS_DETACH\r\n");
  }

  return TRUE;
}

extern "C" BOOL DVP_Init(void) {
  const LONG n = ::InterlockedIncrement(&g_initCalls);
  LogLine(L"=== DVP_Init call #%ld (pid=%lu) log=%s ===", n,
          static_cast<unsigned long>(::GetCurrentProcessId()), LogLastPath());
  return TRUE;
}

extern "C" BOOL DVP_InitEx(LPDVPINITEXDATA pInitExData) {
  const LONG n = ::InterlockedIncrement(&g_initExCalls);

  if (!pInitExData) {
    LogLine(L"=== DVP_InitEx call #%ld with NULL data ===", n);
    return TRUE;
  }

  g_hwndOpusMsgWindow = pInitExData->hwndDOpusMsgWindow;
  g_dwOpusVerMajor = pInitExData->dwOpusVerMajor;
  g_dwOpusVerMinor = pInitExData->dwOpusVerMinor;

  if (pInitExData->pszLanguageName && *pInitExData->pszLanguageName) {
    ::StringCchCopyW(g_szLanguage, ARRAYSIZE(g_szLanguage), pInitExData->pszLanguageName);
  }

  LogLine(L"=== DVP_InitEx call #%ld cbSize=%u hwndOpusMsgWindow=0x%p opusVer=%lu.%lu "
          L"language=\"%s\" pid=%lu ===",
          n, pInitExData->cbSize, g_hwndOpusMsgWindow,
          static_cast<unsigned long>(g_dwOpusVerMajor),
          static_cast<unsigned long>(g_dwOpusVerMinor), g_szLanguage,
          static_cast<unsigned long>(::GetCurrentProcessId()));
  return TRUE;
}

extern "C" void DVP_Uninit(void) {
  const LONG n = ::InterlockedIncrement(&g_uninitCalls);
  const LONG live = ViewerLiveCount();

  LogLine(L"=== DVP_Uninit call #%ld (init=#%ld initEx=#%ld) live viewers=%ld ===", n,
          ::InterlockedCompareExchange(&g_initCalls, 0, 0),
          ::InterlockedCompareExchange(&g_initExCalls, 0, 0), live);

  if (live != 0) {
    // Opus owns the viewer windows; it destroys them. Flag it so the log shows
    // whether Uninit ran before or after the last WM_NCDESTROY.
    LogLine(L"DVP_Uninit: %ld viewer window(s) still alive at uninit time", live);
  }

  g_hwndOpusMsgWindow = nullptr;
}

extern "C" BOOL DVP_IdentifyW(LPVIEWERPLUGININFOW lpVPInfo) {
  const LONG n = ::InterlockedIncrement(&g_identifyCalls);

  if (!lpVPInfo) {
    LogLine(L"DVP_IdentifyW call #%ld with NULL struct -> FALSE", n);
    return FALSE;
  }

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
  lpVPInfo->dwlMaxFileSize = 0;  // 0 == no upper limit
  lpVPInfo->dwlMinPreviewFileSize = 0;
  lpVPInfo->dwlMaxPreviewFileSize = 0;

  lpVPInfo->uiMajorFileType = DVPMajorType_Text;
  lpVPInfo->idPlugin = kPluginId;
  lpVPInfo->dwOpusVerMajor = g_dwOpusVerMajor;
  lpVPInfo->dwOpusVerMinor = g_dwOpusVerMinor;
  lpVPInfo->dwInitFlags = 0;
  lpVPInfo->hIconSmall = nullptr;
  lpVPInfo->hIconLarge = nullptr;

  LogLine(L"DVP_IdentifyW call #%ld -> TRUE name=\"%s\" exts=\"%s\" cbSize=%u opusVer=%lu.%lu "
          L"idPlugin={A7C1E4F2-3B6D-4E59-9C2A-1F8B7D0E5A31}",
          n, g_szName, g_szHandleExts, lpVPInfo->cbSize,
          static_cast<unsigned long>(g_dwOpusVerMajor),
          static_cast<unsigned long>(g_dwOpusVerMinor));
  return TRUE;
}

extern "C" BOOL DVP_IdentifyFileW(HWND hWnd, LPWSTR lpszName, LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                  HANDLE hAbortEvent) {
  const LONG n = ::InterlockedIncrement(&g_identifyFileCalls);
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

  LogLine(L"DVP_IdentifyFileW call #%ld file=\"%s\" -> %s size=%llu", n,
          lpszName ? lpszName : L"(null)", bMatch ? L"TRUE" : L"FALSE", ullSize);

  return bMatch ? TRUE : FALSE;
}

extern "C" HWND DVP_CreateViewer(HWND hWndParent, LPRECT lpRc, DWORD dwFlags) {
  const LONG n = ::InterlockedIncrement(&g_createViewerCalls);

  LogLine(L"DVP_CreateViewer call #%ld hWndParent=0x%p dwFlags=0x%08X (Border=%d Preview=%d "
          L"ReturnTabs=%d)",
          n, hWndParent, static_cast<unsigned>(dwFlags),
          (dwFlags & DVPCVF_Border) ? 1 : 0, (dwFlags & DVPCVF_Preview) ? 1 : 0,
          (dwFlags & DVPCVF_ReturnTabs) ? 1 : 0);

  if (lpRc) {
    LogLine(L"DVP_CreateViewer lpRc=(%ld,%ld)-(%ld,%ld) width=%ld height=%ld", lpRc->left,
            lpRc->top, lpRc->right, lpRc->bottom, lpRc->right - lpRc->left,
            lpRc->bottom - lpRc->top);
  } else {
    LogLine(L"DVP_CreateViewer lpRc=NULL");
  }

  if (!ViewerRegisterClass(g_hInstance)) {
    LogLine(L"DVP_CreateViewer -> NULL (window class registration failed)");
    return nullptr;
  }

  HWND hwnd = ViewerCreate(hWndParent, lpRc, dwFlags);

  LogLine(L"DVP_CreateViewer -> hwnd=0x%p live viewers=%ld", hwnd, ViewerLiveCount());
  return hwnd;
}
