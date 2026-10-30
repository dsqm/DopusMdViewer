// viewer_window.cpp - 查看器窗口与消息分发。
//
// M1 的正文路径是「整篇读入 → 一次 md.render」，这是临时形态：M4 会换成二进制帧
// 按块取。所以这里没有任何滚动/视口状态，那些都归 JS（票 11 第 4 节）。

#include "viewer_window.h"

#include <cstdarg>
#include <new>
#include <objidl.h>  // LPSTREAM，"viewer plugins.h" 需要（WIN32_LEAN_AND_MEAN 会藏掉）
#include <psapi.h>
#include <shellapi.h>
#include <strsafe.h>

#include "config.h"
#include "log.h"
#include "msg.h"
#include "skin.h"
#include "util.h"
#include "viewer plugins.h"
#include "webhost.h"

namespace {

const wchar_t kClassName[] = L"DopusMdViewerWindow";
const wchar_t kTitle[] = L"DopusMdViewer";

// M1 的临时整篇路径：超过这个大小就不整篇塞进 postMessage（M4 才是正式解法）。
const size_t kMaxWholeDocBytes = 64u * 1024u * 1024u;

const wchar_t kDocBase[] = L"https://mddoc.local/";

struct ViewerState {
  HWND hwndSelf;
  HWND hwndParent;
  HANDLE hAbortEvent;
  DWORD dwCreateFlags;
  UINT uMsgSeq;
  wchar_t szFile[MAX_PATH * 2];
  ULONGLONG ullFileSize;
  std::wstring docText;  // 已读入的正文；controller 未就绪时先缓存
  bool docPushed;
  bool chromePushed;
  bool webFailed;
  // DVPNMSTATUSTEXT 的指针在查看器销毁前一直有效，文本要活得比调用长。
  wchar_t szStatus[512];
};

LONG g_liveViewers = 0;
HINSTANCE g_hInstance = nullptr;

struct MsgEntry {
  UINT uMsg;
  const wchar_t* pszName;
};

// 票 11 第 8 节：探针删掉，只留名字表，方便把日志里的消息号翻成人能读的名字。
const MsgEntry kPluginMessages[] = {
    {DVPLUGINMSG_LOADA, L"LOADA"},
    {DVPLUGINMSG_LOADW, L"LOADW"},
    {DVPLUGINMSG_LOADSTREAMA, L"LOADSTREAMA"},
    {DVPLUGINMSG_LOADSTREAMW, L"LOADSTREAMW"},
    {DVPLUGINMSG_GETCAPABILITIES, L"GETCAPABILITIES"},
    {DVPLUGINMSG_RESIZE, L"RESIZE"},
    {DVPLUGINMSG_SELECTALL, L"SELECTALL"},
    {DVPLUGINMSG_COPYSELECTION, L"COPYSELECTION"},
    {DVPLUGINMSG_REDRAW, L"REDRAW"},
    {DVPLUGINMSG_SETABORTEVENT, L"SETABORTEVENT"},
    {DVPLUGINMSG_CLEAR, L"CLEAR"},
    {DVPLUGINMSG_REINITIALIZE, L"REINITIALIZE"},
    {DVPLUGINMSG_SETZOOM, L"SETZOOM"},
    {DVPLUGINMSG_GETZOOMFACTOR, L"GETZOOMFACTOR"},
    {DVPLUGINMSG_TRANSLATEACCEL, L"TRANSLATEACCEL"},
    {DVPLUGINMSG_ISDLGMESSAGE, L"ISDLGMESSAGE"},
    {DVPLUGINMSG_PREVENTAUTOSIZE, L"PREVENTAUTOSIZE"},
    {DVPLUGINMSG_MOUSEWHEEL, L"MOUSEWHEEL"},
};

const MsgEntry kWindowMessages[] = {
    {WM_CREATE, L"WM_CREATE"},
    {WM_DESTROY, L"WM_DESTROY"},
    {WM_SIZE, L"WM_SIZE"},
    {WM_NCDESTROY, L"WM_NCDESTROY"},
    {WM_SETFOCUS, L"WM_SETFOCUS"},
    {WM_KILLFOCUS, L"WM_KILLFOCUS"},
    {WM_ERASEBKGND, L"WM_ERASEBKGND"},
    {WM_PAINT, L"WM_PAINT"},
    {WM_TIMER, L"WM_TIMER"},
    {WM_MOUSEWHEEL, L"WM_MOUSEWHEEL"},
    {WM_KEYDOWN, L"WM_KEYDOWN"},
    {WM_CHAR, L"WM_CHAR"},
    {WM_SETCURSOR, L"WM_SETCURSOR"},
    {WM_DPICHANGED, L"WM_DPICHANGED"},
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

// 鼠标移动与命中测试每秒能来上百条，而每条日志都要开一次文件。它们对排查没有
// 价值，直接不记——鼠标掠过预览窗格时曾经一次会话刷出三千多行。
bool IsNoisyMessage(UINT uMsg) {
  switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_SETCURSOR:
    case WM_NCHITTEST:
    case WM_MOUSEACTIVATE:
    case WM_GETDLGCODE:
    case WM_PRINTCLIENT:
      return true;
    default:
      return false;
  }
}

double QpcMs(LONGLONG from, LONGLONG to) {
  static LONGLONG freq = 0;
  if (freq == 0) {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    freq = f.QuadPart ? f.QuadPart : 1;
  }
  return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq);
}

void LogMemory(const wchar_t* pszTag) {
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    LogLine(L"MEM %s workingSet=%.1fMB private=%.1fMB", pszTag,
            static_cast<double>(pmc.WorkingSetSize) / 1048576.0,
            static_cast<double>(pmc.PrivateUsage) / 1048576.0);
  }
}

void SendStatusText(ViewerState* st, const wchar_t* pszText) {
  if (!st->hwndParent) return;

  DVPNMSTATUSTEXT nmt = {};
  nmt.hdr.hwndFrom = st->hwndSelf;
  nmt.hdr.idFrom = static_cast<UINT_PTR>(::GetDlgCtrlID(st->hwndSelf));
  nmt.hdr.code = static_cast<UINT>(DVPN_STATUSTEXT);
  nmt.lpszStatusText = const_cast<LPTSTR>(pszText);
  nmt.fUnicode = TRUE;

  ::SendMessageW(st->hwndParent, WM_NOTIFY, nmt.hdr.idFrom, reinterpret_cast<LPARAM>(&nmt));
}

void SetStatus(ViewerState* st, const wchar_t* pszFormat, ...) {
  va_list args;
  va_start(args, pszFormat);
  ::StringCchVPrintfW(st->szStatus, ARRAYSIZE(st->szStatus), pszFormat, args);
  va_end(args);
  SendStatusText(st, st->szStatus);
}

RECT ClientRectOf(HWND hwnd) {
  RECT rc = {0, 0, 0, 0};
  ::GetClientRect(hwnd, &rc);
  return rc;
}

// 皮肤与字体永远成对推送：先皮肤，再 fx（fx 最后注入且写全量槽位，压住皮肤）。
void PushChrome(ViewerState* st) {
  const Config& cfg = ConfigLoad();

  std::wstring css;
  std::wstring skinId = cfg.skin;
  if (!SkinLoad(skinId, &css)) {
    skinId = L"light";
    if (!SkinLoad(skinId, &css)) {
      LogLine(L"VIEWER: 连内置 light 皮肤都读不到，退回 base.css 的默认变量");
    }
  }

  WebHostPost(msg::MakeSkin(skinId, css));
  WebHostPost(msg::MakeFx(cfg.font));
  st->chromePushed = true;

  LogLine(L"VIEWER: 已推送皮肤 \"%s\"（%zu 字符）与字体（%dpx / %.2f / %dem）", skinId.c_str(),
          css.size(), cfg.font.sizePx, cfg.font.lineHeight, cfg.font.measureEm);
}

void PushDoc(ViewerState* st) {
  if (st->webFailed) return;
  if (!WebHostIsReady()) {
    LogLine(L"VIEWER: 页面未就绪，正文先留着（%zu 字符）", st->docText.size());
    return;
  }

  if (!WebHostSetDocFolder(UtilParentDir(st->szFile))) {
    LogLine(L"VIEWER: mddoc.local 映射失败，相对图片不会加载");
  }

  LARGE_INTEGER t0;
  LARGE_INTEGER t1;
  ::QueryPerformanceCounter(&t0);
  LogMemory(L"推送前");

  const bool ok = WebHostPost(msg::MakeDoc(kDocBase, st->docText));

  ::QueryPerformanceCounter(&t1);
  st->docPushed = ok;
  LogLine(L"VIEWER: 推送正文 %zu 字符 -> %s，C++ 侧耗时 %.1fms", st->docText.size(),
          ok ? L"成功" : L"失败",
          QpcMs(t0.QuadPart, t1.QuadPart));
  if (ok) LogMemory(L"推送后");
}

// ---- 页面回调（都在 Opus 的 UI 线程上） ----

void OnWebReady(void* ctx) {
  ViewerState* st = static_cast<ViewerState*>(ctx);
  if (!st) return;

  if (!st->chromePushed) PushChrome(st);
  if (!st->docText.empty() && !st->docPushed) PushDoc(st);
}

void OnWebEvent(void* ctx, const std::wstring& type, const std::wstring& rest) {
  ViewerState* st = static_cast<ViewerState*>(ctx);
  if (!st) return;

  if (type == L"event") {
    const std::wstring kind = msg::SecondField(rest);
    const std::wstring detail = rest.size() > kind.size() + 1 ? rest.substr(kind.size() + 1) : L"";

    if (kind == L"rendered") {
      LogLine(L"VIEWER: JS 渲染完成 %s", detail.c_str());
      LogMemory(L"渲染后");
      SetStatus(st, L"%s — %llu B", UtilLeafName(st->szFile), st->ullFileSize);
    } else {
      LogLine(L"VIEWER: 页面事件 \"%s\" %s", kind.c_str(), detail.c_str());
    }
    return;
  }

  if (type == L"open") {
    // 预览器不做导航：网页里的链接交回系统浏览器
    LogLine(L"VIEWER: 打开外部链接 %s", rest.c_str());
    ::ShellExecuteW(nullptr, L"open", rest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

void OnWebFailed(void* ctx, const wchar_t* reason) {
  ViewerState* st = static_cast<ViewerState*>(ctx);
  if (st) {
    st->webFailed = true;
    SetStatus(st, L"预览不可用：%s", reason ? reason : L"WebView2 初始化失败");
    ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
  }
}

WebHostSink MakeSink(ViewerState* st) {
  WebHostSink sink = {};
  sink.ctx = st;
  sink.onReady = &OnWebReady;
  sink.onEvent = &OnWebEvent;
  sink.onFailed = &OnWebFailed;
  return sink;
}

// ---- 消息处理 ----

void HandleLoad(ViewerState* st, const wchar_t* pszPath) {
  if (!pszPath) pszPath = L"";
  ::StringCchCopyW(st->szFile, ARRAYSIZE(st->szFile), pszPath);

  WIN32_FILE_ATTRIBUTE_DATA fad;
  st->ullFileSize = 0;
  if (::GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad)) {
    st->ullFileSize = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  }

  st->docText.clear();
  st->docPushed = false;

  LARGE_INTEGER t0;
  LARGE_INTEGER t1;
  ::QueryPerformanceCounter(&t0);

  std::string bytes;
  if (!UtilReadFileBytes(pszPath, kMaxWholeDocBytes, &bytes)) {
    LogLine(L"VIEWER: 读取失败或超过 %zu MB：%s", kMaxWholeDocBytes / 1048576, pszPath);
    SetStatus(st, L"读取失败：%s", UtilLeafName(pszPath));
    return;
  }

  if (!UtilDecodeText(bytes, &st->docText)) {
    LogLine(L"VIEWER: 编码无法识别（既非 UTF-8 也非 GB18030）：%s", pszPath);
    SetStatus(st, L"编码无法识别：%s", UtilLeafName(pszPath));
    return;
  }

  ::QueryPerformanceCounter(&t1);
  LogLine(L"VIEWER: 读入 \"%s\" %llu 字节 → %zu 字符，耗时 %.1fms", pszPath, st->ullFileSize,
          st->docText.size(), QpcMs(t0.QuadPart, t1.QuadPart));

  PushDoc(st);
  SetStatus(st, L"%s — %llu B", UtilLeafName(pszPath), st->ullFileSize);
}

void HandleClear(ViewerState* st) {
  LogLine(L"VIEWER: CLEAR（丢弃 %zu 字符）", st->docText.size());
  st->szFile[0] = L'\0';
  st->ullFileSize = 0;
  st->docText.clear();
  st->docPushed = false;

  if (WebHostIsReady()) {
    WebHostPost(msg::MakeDoc(kDocBase, std::wstring()));
  }
  ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
}

LRESULT HandleMessage(ViewerState* st, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CREATE: {
      st->hwndParent = ::GetParent(hwnd);

      if (!ViewerRegisterClass(g_hInstance)) {
        LogLine(L"VIEWER: 窗口类未注册");
      }

      // controller 挂在宿主窗口下；这里只是把宿主借进客户区。
      WebHostEnsure(MakeSink(st));
      WebHostBorrow(hwnd, ClientRectOf(hwnd));
      return 0;
    }

    case WM_SIZE:
      WebHostSetBounds(ClientRectOf(hwnd));
      return 0;

    case WM_DESTROY:
      // 必须在这里归还宿主：DestroyWindow 会连子窗口一起销毁，而宿主此刻正是
      // 这个窗口的子窗口。等到 WM_NCDESTROY 就晚了——宿主已经没了，controller
      // 跟着失效（之后每次 postMessage 都是 0x8007139F）。
      WebHostUnborrow();
      return 0;

    case WM_ERASEBKGND: {
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      ::FillRect(reinterpret_cast<HDC>(wParam), &rc,
                 ::GetSysColorBrush(COLOR_WINDOW));
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (st->webFailed) {
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        ::FillRect(hdc, &rc, ::GetSysColorBrush(COLOR_WINDOW));
        ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));
        ::SetBkMode(hdc, TRANSPARENT);
        ::DrawTextW(hdc, L"WebView2 不可用，无法预览 Markdown。详见日志。", -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case DVPLUGINMSG_LOADW:
      // 这个返回值是「加载成功/失败」。返回 FALSE 会让 Opus 立刻 CLEAR 并销毁窗口。
      HandleLoad(st, reinterpret_cast<const wchar_t*>(lParam));
      return TRUE;

    case DVPLUGINMSG_CLEAR:
      HandleClear(st);
      return 0;

    case DVPLUGINMSG_RESIZE: {
      const int iWidth = static_cast<short>(LOWORD(lParam));
      const int iHeight = static_cast<short>(HIWORD(lParam));
      if (iWidth <= 0 || iHeight <= 0) {
        RECT rc = ClientRectOf(hwnd);
        WebHostSetBounds(rc);
      } else {
        RECT rc = {0, 0, iWidth, iHeight};
        WebHostSetBounds(rc);
      }
      return 0;
    }

    case DVPLUGINMSG_GETCAPABILITIES:
      return VPCAPABILITY_WANTFOCUS;

    case DVPLUGINMSG_PREVENTAUTOSIZE:
      return TRUE;

    case DVPLUGINMSG_SETABORTEVENT:
      st->hAbortEvent = reinterpret_cast<HANDLE>(lParam);
      return 0;

    case DVPLUGINMSG_REINITIALIZE:
      // 只是通知，不销毁窗口（viewer plugins.h:319）。配置变了就重推一遍皮肤与字体，
      // 正文不动 —— 字号变化后的位置恢复由 JS 按 (anchorId, anchorDy) 处理。
      LogLine(L"VIEWER: REINITIALIZE，重推皮肤与字体");
      if (WebHostIsReady()) PushChrome(st);
      return 0;

    case DVPLUGINMSG_REDRAW:
      ::InvalidateRect(hwnd, nullptr, TRUE);
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
    if (!IsNoisyMessage(uMsg)) {
      LogLine(L"WND %p MSG #%u %s (0x%04X)", hwnd, st->uMsgSeq, MsgName(uMsg), uMsg);
    }
  }

  if (uMsg == WM_NCDESTROY) {
    if (st) {
      // 回调里带着 st，窗口没了必须先摘掉，否则事件会落到已释放的状态上
      WebHostSink empty = {};
      WebHostSetSink(empty);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      LogLine(L"WND %p WM_NCDESTROY 释放状态（共 %u 条消息）", hwnd, st->uMsgSeq);
      delete st;
    }
    ::InterlockedDecrement(&g_liveViewers);
    return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }

  if (!st) return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

  return HandleMessage(st, hwnd, uMsg, wParam, lParam);
}

}  // namespace

bool ViewerRegisterClass(HINSTANCE hInstance) {
  if (!g_hInstance) g_hInstance = hInstance;

  static bool registered = false;
  if (registered) return true;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = ViewerWndProc;
  wc.hInstance = hInstance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // WM_ERASEBKGND 自己刷
  wc.lpszClassName = kClassName;

  if (::RegisterClassExW(&wc)) {
    registered = true;
    return true;
  }

  if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    registered = true;
    return true;
  }

  LogLine(L"VIEWER: RegisterClassExW 失败 err=%lu", ::GetLastError());
  return false;
}

HWND ViewerCreate(HWND hWndParent, const RECT* lpRc, DWORD dwFlags) {
  RECT rc = {};
  if (lpRc) rc = *lpRc;

  if (!g_hInstance) {
    LogLine(L"VIEWER: 窗口类从未注册");
    return nullptr;
  }

  ViewerState* st = new (std::nothrow) ViewerState();
  if (!st) {
    LogLine(L"VIEWER: 状态分配失败");
    return nullptr;
  }

  st->hwndSelf = nullptr;
  st->hwndParent = hWndParent;
  st->hAbortEvent = nullptr;
  st->dwCreateFlags = dwFlags;
  st->uMsgSeq = 0;
  st->szFile[0] = L'\0';
  st->ullFileSize = 0;
  st->docPushed = false;
  st->chromePushed = false;
  st->webFailed = false;
  st->szStatus[0] = L'\0';

  const int cx = rc.right - rc.left;
  const int cy = rc.bottom - rc.top;

  HWND hwnd = ::CreateWindowExW(0, kClassName, kTitle, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                rc.left, rc.top, cx, cy, hWndParent, nullptr, g_hInstance, st);
  if (!hwnd) {
    LogLine(L"VIEWER: CreateWindowExW 失败 err=%lu", ::GetLastError());
    delete st;
    return nullptr;
  }

  ::InterlockedIncrement(&g_liveViewers);
  LogLine(L"VIEWER: 建窗口 %p parent=%p %dx%d flags=0x%08X", hwnd, hWndParent, cx, cy,
          static_cast<unsigned>(dwFlags));
  return hwnd;
}

LONG ViewerLiveCount(void) { return ::InterlockedCompareExchange(&g_liveViewers, 0, 0); }
