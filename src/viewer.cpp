// viewer.cpp - the plugin's child window.
//
// Scope of this file is deliberately narrow: receive messages, log them, and
// show something that proves the window is alive. No rendering, no Markdown,
// no WebView2.

#include "viewer.h"

#include <cstring>
#include <new>
#include <objidl.h>  // LPSTREAM, needed by "viewer plugins.h" (WIN32_LEAN_AND_MEAN hides it)
#include <strsafe.h>

#include "log.h"
#include "viewer plugins.h"

namespace {

// Opus 13 sends viewer messages that this SDK (v4) does not declare. Several of
// them carry pointer arguments, so before guessing at their meaning we dump the
// pointed-to words. Reads are bounded by the committed region, so a bogus
// pointer degrades to a log line instead of an access violation.
void SafeDumpHex(const wchar_t* pszTag, const void* p, int cQwords) {
  if (!p) {
    LogLine(L"PROBE %s ptr=null", pszTag);
    return;
  }

  MEMORY_BASIC_INFORMATION mbi = {};
  if (!::VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    LogLine(L"PROBE %s ptr=%p unreadable", pszTag, p);
    return;
  }

  const BYTE* pEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
  const BYTE* q = static_cast<const BYTE*>(p);

  wchar_t szBuf[512] = L"";
  for (int i = 0; i < cQwords && q + sizeof(unsigned long long) <= pEnd; ++i, q += sizeof(unsigned long long)) {
    unsigned long long v = 0;
    ::memcpy(&v, q, sizeof(v));
    wchar_t szOne[32];
    ::StringCchPrintfW(szOne, ARRAYSIZE(szOne), L" %016llX", v);
    ::StringCchCatW(szBuf, ARRAYSIZE(szBuf), szOne);
  }

  LogLine(L"PROBE %s ptr=%p %s", pszTag, p, szBuf);
}

const wchar_t kClassName[] = L"DopusMdViewerPocWindow";
const wchar_t kTitle[] = L"DOPUSMDVIEWER POC";
const int kStaticId = 1001;

struct ViewerState {
  HWND hwndSelf;
  HWND hwndStatic;
  HWND hwndParent;
  HBRUSH hbrBackground;
  COLORREF crBackground;
  bool backgroundKnown;
  HANDLE hAbortEvent;
  DWORD dwCreateFlags;
  UINT uMsgSeq;
  wchar_t szFile[MAX_PATH * 2];
  ULONGLONG ullFileSize;
  // Opus keeps the pointer handed over in DVPNMSTATUSTEXT until the viewer is
  // destroyed or a new status is set, so the text has to outlive the call.
  wchar_t szStatus[512];
};

LONG g_liveViewers = 0;

// The class is registered under the DLL's HINSTANCE, so CreateWindowExW has to
// use that same HINSTANCE - passing the host's would fail with ERROR_CANNOT_FIND_WND_CLASS.
HINSTANCE g_hViewerInstance = nullptr;

struct MsgEntry {
  UINT uMsg;
  const wchar_t* pszName;
};

const MsgEntry kWindowMessages[] = {
    {WM_NULL, L"WM_NULL"},
    {WM_CREATE, L"WM_CREATE"},
    {WM_DESTROY, L"WM_DESTROY"},
    {WM_MOVE, L"WM_MOVE"},
    {WM_SIZE, L"WM_SIZE"},
    {WM_ACTIVATE, L"WM_ACTIVATE"},
    {WM_SETFOCUS, L"WM_SETFOCUS"},
    {WM_KILLFOCUS, L"WM_KILLFOCUS"},
    {WM_ENABLE, L"WM_ENABLE"},
    {WM_PAINT, L"WM_PAINT"},
    {WM_CLOSE, L"WM_CLOSE"},
    {WM_ERASEBKGND, L"WM_ERASEBKGND"},
    {WM_SHOWWINDOW, L"WM_SHOWWINDOW"},
    {WM_SETTINGCHANGE, L"WM_SETTINGCHANGE"},
    {WM_CTLCOLORSTATIC, L"WM_CTLCOLORSTATIC"},
    {WM_PARENTNOTIFY, L"WM_PARENTNOTIFY"},
    {WM_GETDLGCODE, L"WM_GETDLGCODE"},
    {WM_KEYDOWN, L"WM_KEYDOWN"},
    {WM_KEYUP, L"WM_KEYUP"},
    {WM_CHAR, L"WM_CHAR"},
    {WM_SYSKEYDOWN, L"WM_SYSKEYDOWN"},
    {WM_SYSKEYUP, L"WM_SYSKEYUP"},
    {WM_SYSCHAR, L"WM_SYSCHAR"},
    {WM_COMMAND, L"WM_COMMAND"},
    {WM_TIMER, L"WM_TIMER"},
    {WM_MOUSEMOVE, L"WM_MOUSEMOVE"},
    {WM_LBUTTONDOWN, L"WM_LBUTTONDOWN"},
    {WM_LBUTTONUP, L"WM_LBUTTONUP"},
    {WM_LBUTTONDBLCLK, L"WM_LBUTTONDBLCLK"},
    {WM_RBUTTONDOWN, L"WM_RBUTTONDOWN"},
    {WM_RBUTTONUP, L"WM_RBUTTONUP"},
    {WM_MOUSEWHEEL, L"WM_MOUSEWHEEL"},
    {WM_SETCURSOR, L"WM_SETCURSOR"},
    {WM_NOTIFY, L"WM_NOTIFY"},
    {WM_CONTEXTMENU, L"WM_CONTEXTMENU"},
    {WM_MOUSEACTIVATE, L"WM_MOUSEACTIVATE"},
    {WM_CAPTURECHANGED, L"WM_CAPTURECHANGED"},
    {WM_CANCELMODE, L"WM_CANCELMODE"},
    {WM_SETTEXT, L"WM_SETTEXT"},
    {WM_GETTEXT, L"WM_GETTEXT"},
    {WM_GETTEXTLENGTH, L"WM_GETTEXTLENGTH"},
    {WM_NCCREATE, L"WM_NCCREATE"},
    {WM_NCDESTROY, L"WM_NCDESTROY"},
    {WM_NCCALCSIZE, L"WM_NCCALCSIZE"},
    {WM_NCPAINT, L"WM_NCPAINT"},
    {WM_NCHITTEST, L"WM_NCHITTEST"},
    {WM_NCACTIVATE, L"WM_NCACTIVATE"},
    {WM_WINDOWPOSCHANGING, L"WM_WINDOWPOSCHANGING"},
    {WM_WINDOWPOSCHANGED, L"WM_WINDOWPOSCHANGED"},
    {WM_STYLECHANGING, L"WM_STYLECHANGING"},
    {WM_STYLECHANGED, L"WM_STYLECHANGED"},
    {WM_SYSCOMMAND, L"WM_SYSCOMMAND"},
    {WM_DPICHANGED, L"WM_DPICHANGED"},
    {WM_DISPLAYCHANGE, L"WM_DISPLAYCHANGE"},
    {WM_FONTCHANGE, L"WM_FONTCHANGE"},
    {WM_PRINTCLIENT, L"WM_PRINTCLIENT"},
    {WM_THEMECHANGED, L"WM_THEMECHANGED"},
    {WM_IME_SETCONTEXT, L"WM_IME_SETCONTEXT"},
    {WM_IME_NOTIFY, L"WM_IME_NOTIFY"},
};

const MsgEntry kPluginMessages[] = {
    {DVPLUGINMSG_LOADA, L"LOADA"},
    {DVPLUGINMSG_LOADW, L"LOADW"},
    {DVPLUGINMSG_LOADSTREAMA, L"LOADSTREAMA"},
    {DVPLUGINMSG_LOADSTREAMW, L"LOADSTREAMW"},
    {DVPLUGINMSG_GETIMAGEINFOA, L"GETIMAGEINFOA"},
    {DVPLUGINMSG_GETIMAGEINFOW, L"GETIMAGEINFOW"},
    {DVPLUGINMSG_GETCAPABILITIES, L"GETCAPABILITIES"},
    {DVPLUGINMSG_RESIZE, L"RESIZE"},
    {DVPLUGINMSG_SETROTATION, L"SETROTATION"},
    {DVPLUGINMSG_ROTATE, L"ROTATE"},
    {DVPLUGINMSG_SETZOOM, L"SETZOOM"},
    {DVPLUGINMSG_ZOOM, L"ZOOM"},
    {DVPLUGINMSG_GETZOOMFACTOR, L"GETZOOMFACTOR"},
    {DVPLUGINMSG_SELECTALL, L"SELECTALL"},
    {DVPLUGINMSG_TESTSELECTION, L"TESTSELECTION"},
    {DVPLUGINMSG_COPYSELECTION, L"COPYSELECTION"},
    {DVPLUGINMSG_PRINT, L"PRINT"},
    {DVPLUGINMSG_PROPERTIES, L"PROPERTIES"},
    {DVPLUGINMSG_REDRAW, L"REDRAW"},
    {DVPLUGINMSG_GETPICSIZE, L"GETPICSIZE"},
    {DVPLUGINMSG_GETAUTOBGCOL, L"GETAUTOBGCOL"},
    {DVPLUGINMSG_MOUSEWHEEL, L"MOUSEWHEEL"},
    {DVPLUGINMSG_ADDCONTEXTMENUA, L"ADDCONTEXTMENUA"},
    {DVPLUGINMSG_ADDCONTEXTMENUW, L"ADDCONTEXTMENUW"},
    {DVPLUGINMSG_SETABORTEVENT, L"SETABORTEVENT"},
    {DVPLUGINMSG_GETORIGINALPICSIZE, L"GETORIGINALPICSIZE"},
    {DVPLUGINMSG_CLEAR, L"CLEAR"},
    {DVPLUGINMSG_NOTIFY_LOADPROGRESS, L"NOTIFY_LOADPROGRESS"},
    {DVPLUGINMSG_ISDLGMESSAGE, L"ISDLGMESSAGE"},
    {DVPLUGINMSG_TRANSLATEACCEL, L"TRANSLATEACCEL"},
    {DVPLUGINMSG_REINITIALIZE, L"REINITIALIZE"},
    {DVPLUGINMSG_SHOWHIDESCROLLBARS, L"SHOWHIDESCROLLBARS"},
    {DVPLUGINMSG_INLOADLOOP, L"INLOADLOOP"},
    {DVPLUGINMSG_SETIMAGEFRAME, L"SETIMAGEFRAME"},
    {DVPLUGINMSG_SETDESKWALLPAPERA, L"SETDESKWALLPAPERA"},
    {DVPLUGINMSG_GETZOOMLIMITS, L"GETZOOMLIMITS"},
    {DVPLUGINMSG_THUMBSCHANGED, L"THUMBSCHANGED"},
    {DVPLUGINMSG_GETBITMAP, L"GETBITMAP"},
    {DVPLUGINMSG_GAMMACHANGE, L"GAMMACHANGE"},
    {DVPLUGINMSG_APPCOMMAND, L"APPCOMMAND"},
    {DVPLUGINMSG_PREVENTFRAME, L"PREVENTFRAME"},
    {DVPLUGINMSG_FULLSCREEN, L"FULLSCREEN"},
    {DVPLUGINMSG_SHOWFILEINFO, L"SHOWFILEINFO"},
    {DVPLUGINMSG_ISFILEINFOSHOWN, L"ISFILEINFOSHOWN"},
    {DVPLUGINMSG_SETDESKWALLPAPERW, L"SETDESKWALLPAPERW"},
    {DVPLUGINMSG_PREVENTAUTOSIZE, L"PREVENTAUTOSIZE"},
    {DVPLUGINMSG_SHOWHEX, L"SHOWHEX"},
    {DVPLUGINMSG_ISALPHAHIDDEN, L"ISALPHAHIDDEN"},
    {DVPLUGINMSG_HIDEALPHA, L"HIDEALPHA"},
    {DVPLUGINMSG_CROPSELECTION, L"CROPSELECTION"},
    {DVPLUGINMSG_RESTORE, L"RESTORE"},
};

const wchar_t* LookupName(const MsgEntry* pTable, size_t cEntries, UINT uMsg) {
  for (size_t i = 0; i < cEntries; ++i) {
    if (pTable[i].uMsg == uMsg) return pTable[i].pszName;
  }
  return nullptr;
}

const wchar_t* MsgName(UINT uMsg) {
  const wchar_t* psz = LookupName(kPluginMessages, ARRAYSIZE(kPluginMessages), uMsg);
  if (psz) return psz;
  psz = LookupName(kWindowMessages, ARRAYSIZE(kWindowMessages), uMsg);
  return psz ? psz : L"UNKNOWN";
}

// Asks the Lister for the current viewer background colour. Returns false if the
// parent did not answer with a usable colour.
bool QueryParentBackground(HWND hwndParent, COLORREF* pcrOut) {
  if (!hwndParent) return false;

  LRESULT lr = ::SendMessageW(hwndParent, DVPN_GETBGCOL, 0, 0);
  LogLine(L"  DVPN_GETBGCOL query to parent %p msg=0x%08X -> lr=0x%08X", hwndParent,
          static_cast<unsigned>(DVPN_GETBGCOL), static_cast<unsigned>(lr));

  if (lr == static_cast<LRESULT>(CLR_INVALID)) return false;

  *pcrOut = static_cast<COLORREF>(lr);
  return true;
}

COLORREF ContrastTextColour(COLORREF cr) {
  const int luma = (299 * GetRValue(cr) + 587 * GetGValue(cr) + 114 * GetBValue(cr)) / 1000;
  return (luma > 128) ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

void RebuildBackgroundBrush(ViewerState* st) {
  if (st->hbrBackground) {
    ::DeleteObject(st->hbrBackground);
  }
  st->hbrBackground = ::CreateSolidBrush(st->crBackground);
}

void UpdateLabel(ViewerState* st) {
  if (!st->hwndStatic) return;

  wchar_t szText[2048];
  if (st->szFile[0] != L'\0') {
    ::StringCchPrintfW(szText, ARRAYSIZE(szText),
                       L"%s\r\n\r\nFile: %s\r\nSize: %llu bytes\r\nCreateViewer dwFlags: 0x%08X\r\n"
                       L"Background: RGB(%u,%u,%u)\r\n\r\nWaiting for the next message...",
                       kTitle, st->szFile, st->ullFileSize,
                       static_cast<unsigned>(st->dwCreateFlags), GetRValue(st->crBackground),
                       GetGValue(st->crBackground), GetBValue(st->crBackground));
  } else {
    ::StringCchPrintfW(szText, ARRAYSIZE(szText),
                       L"%s\r\n\r\nFile: (none loaded yet)\r\nCreateViewer dwFlags: 0x%08X\r\n"
                       L"Background: RGB(%u,%u,%u)\r\n\r\nWaiting for the next message...",
                       kTitle, static_cast<unsigned>(st->dwCreateFlags),
                       GetRValue(st->crBackground), GetGValue(st->crBackground),
                       GetBValue(st->crBackground));
  }

  ::SetWindowTextW(st->hwndStatic, szText);
}

// Tells the Lister what to put in the status bar; proves the notification path.
void SendStatusText(ViewerState* st, const wchar_t* pszText) {
  if (!st->hwndParent) return;

  DVPNMSTATUSTEXT nmt = {};
  nmt.hdr.hwndFrom = st->hwndSelf;
  nmt.hdr.idFrom = static_cast<UINT_PTR>(::GetDlgCtrlID(st->hwndSelf));
  nmt.hdr.code = static_cast<UINT>(DVPN_STATUSTEXT);
  nmt.lpszStatusText = const_cast<LPTSTR>(pszText);
  nmt.fUnicode = TRUE;

  LRESULT lr = ::SendMessageW(st->hwndParent, WM_NOTIFY, nmt.hdr.idFrom,
                              reinterpret_cast<LPARAM>(&nmt));

  LogLine(L"WND %p -> parent %p WM_NOTIFY DVPN_STATUSTEXT \"%s\" -> %lld", st->hwndSelf,
          st->hwndParent, pszText, static_cast<long long>(lr));
}

void LayoutFillClient(ViewerState* st) {
  if (!st->hwndStatic) return;

  RECT rc;
  ::GetClientRect(st->hwndSelf, &rc);
  ::MoveWindow(st->hwndStatic, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void HandleLoad(ViewerState* st, const wchar_t* pszPath) {
  if (!pszPath) pszPath = L"";

  ::StringCchCopyW(st->szFile, ARRAYSIZE(st->szFile), pszPath);
  st->ullFileSize = 0;

  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (::GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad)) {
    st->ullFileSize = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  }

  LogLine(L"WND %p LOADW path=\"%s\" size=%llu", st->hwndSelf, st->szFile, st->ullFileSize);

  UpdateLabel(st);

  const wchar_t* pszLeaf = st->szFile;
  for (const wchar_t* p = st->szFile; *p; ++p) {
    if (*p == L'\\' || *p == L'/') pszLeaf = p + 1;
  }
  ::StringCchPrintfW(st->szStatus, ARRAYSIZE(st->szStatus), L"POC: %s (%llu B)", pszLeaf,
                     st->ullFileSize);
  SendStatusText(st, st->szStatus);
}

void HandleClear(ViewerState* st) {
  LogLine(L"WND %p CLEAR dropping file=\"%s\" size=%llu", st->hwndSelf, st->szFile,
          st->ullFileSize);

  st->szFile[0] = L'\0';
  st->ullFileSize = 0;

  UpdateLabel(st);
  ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
}

LRESULT HandleMessage(ViewerState* st, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CREATE: {
      st->hwndParent = ::GetParent(hwnd);

      RECT rc;
      ::GetClientRect(hwnd, &rc);

      st->hwndStatic = ::CreateWindowExW(
          0, L"STATIC", kTitle, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0,
          rc.right - rc.left, rc.bottom - rc.top, hwnd,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStaticId)), g_hViewerInstance, nullptr);
      if (st->hwndStatic) {
        ::SendMessageW(st->hwndStatic, WM_SETFONT,
                       reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);
      }

      COLORREF cr = 0;
      st->backgroundKnown = QueryParentBackground(st->hwndParent, &cr);
      st->crBackground = st->backgroundKnown ? cr : ::GetSysColor(COLOR_WINDOW);
      RebuildBackgroundBrush(st);
      LogLine(L"WND %p WM_CREATE parent=%p static=%p DVPN_GETBGCOL ok=%d colour=0x%08X "
              L"RGB(%u,%u,%u)",
              hwnd, st->hwndParent, st->hwndStatic, st->backgroundKnown ? 1 : 0,
              static_cast<unsigned>(st->crBackground), GetRValue(st->crBackground),
              GetGValue(st->crBackground), GetBValue(st->crBackground));

      UpdateLabel(st);
      return 0;
    }

    case WM_SIZE: {
      LogLine(L"WND %p WM_SIZE cx=%u cy=%u", hwnd, LOWORD(lParam), HIWORD(lParam));
      LayoutFillClient(st);
      return 0;
    }

    case WM_SETFOCUS:
      LogLine(L"WND %p WM_SETFOCUS (gained)", hwnd);
      return 0;

    case WM_KILLFOCUS:
      LogLine(L"WND %p WM_KILLFOCUS (lost, new=%p)", hwnd, reinterpret_cast<HWND>(wParam));
      return 0;

    case WM_ERASEBKGND: {
      if (!st->backgroundKnown) {
        COLORREF cr = 0;
        if (QueryParentBackground(st->hwndParent, &cr)) {
          st->crBackground = cr;
          st->backgroundKnown = true;
          RebuildBackgroundBrush(st);
        }
      }

      RECT rc;
      ::GetClientRect(hwnd, &rc);
      ::FillRect(reinterpret_cast<HDC>(wParam), &rc, st->hbrBackground);
      return 1;
    }

    case WM_CTLCOLORSTATIC: {
      HDC hdc = reinterpret_cast<HDC>(wParam);
      ::SetBkColor(hdc, st->crBackground);
      ::SetTextColor(hdc, ContrastTextColour(st->crBackground));
      return reinterpret_cast<LRESULT>(st->hbrBackground);
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      ::FillRect(hdc, &rc, st->hbrBackground);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case DVPLUGINMSG_LOADW:
      // Opus reads this as "load succeeded" / "load failed"; returning FALSE makes
      // it send DVPLUGINMSG_CLEAR and destroy the viewer immediately.
      HandleLoad(st, reinterpret_cast<const wchar_t*>(lParam));
      return TRUE;

    case DVPLUGINMSG_CLEAR:
      HandleClear(st);
      return 0;

    case DVPLUGINMSG_RESIZE: {
      const int iLeft = static_cast<short>(LOWORD(wParam));
      const int iTop = static_cast<short>(HIWORD(wParam));
      const int iWidth = static_cast<short>(LOWORD(lParam));
      const int iHeight = static_cast<short>(HIWORD(lParam));
      LogLine(L"WND %p RESIZE left=%d top=%d width=%d height=%d", hwnd, iLeft, iTop, iWidth,
              iHeight);
      if (st->hwndStatic) {
        ::MoveWindow(st->hwndStatic, 0, 0, iWidth, iHeight, TRUE);
      }
      return 0;
    }

    case DVPLUGINMSG_GETCAPABILITIES: {
      const LRESULT lr = VPCAPABILITY_WANTFOCUS;
      LogLine(L"WND %p GETCAPABILITIES -> 0x%08X (VPCAPABILITY_WANTFOCUS)", hwnd,
              static_cast<unsigned>(lr));
      return lr;
    }

    case DVPLUGINMSG_PREVENTAUTOSIZE:
      LogLine(L"WND %p PREVENTAUTOSIZE -> TRUE", hwnd);
      return TRUE;

    case DVPLUGINMSG_SETABORTEVENT:
      st->hAbortEvent = reinterpret_cast<HANDLE>(lParam);
      LogLine(L"WND %p SETABORTEVENT handle=%p", hwnd, st->hAbortEvent);
      return 0;

    case DVPLUGINMSG_REDRAW: {
      if (wParam) {
        const COLORREF crNew = static_cast<COLORREF>(lParam);
        LogLine(L"WND %p REDRAW background changed 0x%08X -> 0x%08X", hwnd,
                static_cast<unsigned>(st->crBackground), static_cast<unsigned>(crNew));
        st->crBackground = crNew;
        st->backgroundKnown = true;
        RebuildBackgroundBrush(st);
        UpdateLabel(st);
      } else {
        LogLine(L"WND %p REDRAW (background unchanged)", hwnd);
      }
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case DVPLUGINMSG_REINITIALIZE: {
      COLORREF cr = 0;
      const bool bOk = QueryParentBackground(st->hwndParent, &cr);
      LogLine(L"WND %p REINITIALIZE re-query DVPN_GETBGCOL ok=%d colour=0x%08X", hwnd, bOk ? 1 : 0,
              bOk ? static_cast<unsigned>(cr) : 0u);
      if (bOk) {
        st->crBackground = cr;
        st->backgroundKnown = true;
        RebuildBackgroundBrush(st);
        UpdateLabel(st);
      }
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    // --- Undeclared Opus 13 messages, probed for payload shape -------------
    case DVPLUGINMSG_BASE + 0x46:
      SafeDumpHex(L"M+46.wParam", reinterpret_cast<const void*>(wParam), 8);
      SafeDumpHex(L"M+46.lParam", reinterpret_cast<const void*>(lParam), 8);
      return 0;

    case DVPLUGINMSG_BASE + 0x4A:
      LogLine(L"WND %p M+4A wParam=%llu (dword) lParam=0x%llX", hwnd,
              static_cast<unsigned long long>(wParam), static_cast<unsigned long long>(lParam));
      return 0;

    case DVPLUGINMSG_BASE + 0x4B:
      LogLine(L"WND %p M+4B wParam=0x%llX lParam=0x%llX", hwnd,
              static_cast<unsigned long long>(wParam), static_cast<unsigned long long>(lParam));
      return 0;

    case DVPLUGINMSG_BASE + 0x4C: {
      // Repeatedly delivered with a stack pointer; the payload is what matters.
      LogLine(L"WND %p M+4C wParam=0x%llX", hwnd, static_cast<unsigned long long>(wParam));
      SafeDumpHex(L"M+4C.lParam", reinterpret_cast<const void*>(lParam), 16);
      return 0;
    }

    case DVPLUGINMSG_BASE + 0x50:
      LogLine(L"WND %p M+50 wParam=0x%llX lParam=0x%llX", hwnd,
              static_cast<unsigned long long>(wParam), static_cast<unsigned long long>(lParam));
      return 0;

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

LRESULT CALLBACK ViewerWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  ViewerState* st = reinterpret_cast<ViewerState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  if (uMsg == WM_NCCREATE) {
    const CREATESTRUCTW* pcs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    st = static_cast<ViewerState*>(pcs->lpCreateParams);
    if (st) {
      st->hwndSelf = hwnd;
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    }
  }

  if (st) {
    ++st->uMsgSeq;
    LogLine(L"WND %p MSG #%u %s (0x%04X) wParam=0x%llX lParam=0x%llX", hwnd, st->uMsgSeq,
            MsgName(uMsg), uMsg, static_cast<unsigned long long>(wParam),
            static_cast<unsigned long long>(lParam));
  } else {
    LogLine(L"WND %p MSG (no state) %s (0x%04X) wParam=0x%llX lParam=0x%llX", hwnd,
            MsgName(uMsg), uMsg, static_cast<unsigned long long>(wParam),
            static_cast<unsigned long long>(lParam));
  }

  if (uMsg == WM_NCDESTROY) {
    if (st) {
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      if (st->hbrBackground) ::DeleteObject(st->hbrBackground);
      LogLine(L"WND %p WM_NCDESTROY releasing state (%u messages seen)", hwnd, st->uMsgSeq);
      delete st;
    }
    ::InterlockedDecrement(&g_liveViewers);
    return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }

  if (!st) {
    return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }

  return HandleMessage(st, hwnd, uMsg, wParam, lParam);
}

}  // namespace

bool ViewerRegisterClass(HINSTANCE hInstance) {
  if (!g_hViewerInstance) g_hViewerInstance = hInstance;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = ViewerWndProc;
  wc.hInstance = hInstance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // WM_ERASEBKGND paints with the Lister colour
  wc.lpszClassName = kClassName;

  if (::RegisterClassExW(&wc)) {
    LogLine(L"RegisterClassExW(\"%s\") ok", kClassName);
    return true;
  }

  const DWORD dwErr = ::GetLastError();
  if (dwErr == ERROR_CLASS_ALREADY_EXISTS) {
    return true;
  }

  LogLine(L"RegisterClassExW(\"%s\") FAILED err=%lu", kClassName, dwErr);
  return false;
}

HWND ViewerCreate(HWND hWndParent, const RECT* lpRc, DWORD dwFlags) {
  RECT rc = {};
  if (lpRc) rc = *lpRc;

  if (!g_hViewerInstance) {
    LogLine(L"ViewerCreate: window class was never registered");
    return nullptr;
  }

  ViewerState* st = new (std::nothrow) ViewerState();
  if (!st) {
    LogLine(L"ViewerCreate: out of memory allocating state");
    return nullptr;
  }

  st->hwndSelf = nullptr;
  st->hwndStatic = nullptr;
  st->hwndParent = hWndParent;
  st->hbrBackground = nullptr;
  st->crBackground = ::GetSysColor(COLOR_WINDOW);
  st->backgroundKnown = false;
  st->hAbortEvent = nullptr;
  st->dwCreateFlags = dwFlags;
  st->uMsgSeq = 0;
  st->szFile[0] = L'\0';
  st->ullFileSize = 0;

  const int cx = rc.right - rc.left;
  const int cy = rc.bottom - rc.top;

  HWND hwnd = ::CreateWindowExW(0, kClassName, kTitle, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                rc.left, rc.top, cx, cy, hWndParent, nullptr, g_hViewerInstance,
                                st);

  if (!hwnd) {
    const DWORD dwErr = ::GetLastError();
    LogLine(L"ViewerCreate: CreateWindowExW FAILED err=%lu rc=(%ld,%ld,%ld,%ld)", dwErr,
            rc.left, rc.top, rc.right, rc.bottom);
    delete st;
    return nullptr;
  }

  ::InterlockedIncrement(&g_liveViewers);
  LogLine(L"ViewerCreate: hwnd=%p parent=%p rect=(%ld,%ld)-(%ld,%ld) size=%dx%d flags=0x%08X",
          hwnd, hWndParent, rc.left, rc.top, rc.right, rc.bottom, cx, cy,
          static_cast<unsigned>(dwFlags));
  return hwnd;
}

LONG ViewerLiveCount(void) {
  return ::InterlockedCompareExchange(&g_liveViewers, 0, 0);
}
