// viewer_window.cpp - 查看器窗口与消息分发。
//
// M4 起正文不再整篇读入推送：打开时由 doc_source 建索引（同步前缀 + 后台全文），
// 这里只推 index 控制消息，块正文由页面按区间向 mdblocks.local 取。

#include "viewer_window.h"

#include <cstdarg>
#include <mutex>
#include <new>
#include <vector>
#include <objidl.h>  // LPSTREAM，"viewer plugins.h" 需要（WIN32_LEAN_AND_MEAN 会藏掉）
#include <psapi.h>
#include <shellapi.h>
#include <strsafe.h>

#include "config.h"
#include "doc_source.h"
#include "log.h"
#include "msg.h"
#include "skin.h"
#include "util.h"
#include "viewer plugins.h"
#include "webhost.h"

namespace {

const wchar_t kClassName[] = L"DopusMdViewerWindow";
const wchar_t kTitle[] = L"DopusMdViewer";
const wchar_t kDocBase[] = L"https://mddoc.local/";

struct ViewerState {
  HWND hwndSelf;
  HWND hwndParent;
  HANDLE hAbortEvent;
  DWORD dwCreateFlags;
  UINT uMsgSeq;
  wchar_t szFile[MAX_PATH * 2];
  ULONGLONG ullFileSize;
  uint32_t prefixBlocks;
  uint32_t estBlocks;
  uint32_t lastVisible;  // 最后一个非隐藏块（脚注区挂它后面）
  int encoding;
  LONGLONG qpcLoad;  // LOADW 起点，用于端到端耗时
  bool docOpened;    // doc_source 归本窗口
  bool indexFullPending;  // 全文索引已就绪但页面还没收到 index-full
  bool chromePushed;
  bool webFailed;
  bool hasHost;  // 宿主此刻是否在本窗口（单宿主，被别的窗口借走时没有画面）
  // DVPNMSTATUSTEXT 的指针在查看器销毁前一直有效，文本要活得比调用长。
  wchar_t szStatus[512];
};

LONG g_liveViewers = 0;
HINSTANCE g_hInstance = nullptr;

// 设置保存后要通知所有查看器（它们可能跑在不同的 UI 线程上）。
std::mutex g_viewerMutex;
std::vector<HWND> g_viewers;

void RegisterViewer(HWND hwnd) {
  std::lock_guard<std::mutex> lock(g_viewerMutex);
  g_viewers.push_back(hwnd);
}

void UnregisterViewer(HWND hwnd) {
  std::lock_guard<std::mutex> lock(g_viewerMutex);
  for (size_t i = 0; i < g_viewers.size(); ++i) {
    if (g_viewers[i] == hwnd) {
      g_viewers.erase(g_viewers.begin() + i);
      return;
    }
  }
}

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
    {WM_MDV_INDEX_FULL, L"WM_MDV_INDEX_FULL"},
    {WM_MDV_CONFIG_CHANGED, L"WM_MDV_CONFIG_CHANGED"},
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
  if (!st->hasHost || st->webFailed || !WebHostIsReady()) return;
  const Config cfg = ConfigSnapshot();

  std::wstring css;
  std::wstring skinId = cfg.skin;
  if (!SkinLoad(skinId, &css)) {
    skinId = L"light";
    if (!SkinLoad(skinId, &css)) {
      LogLine(L"VIEWER: 连内置 light 皮肤都读不到，退回 base.css 的默认变量");
    }
  }

  const bool ok = WebHostPost(st->hwndSelf, msg::MakeSkin(skinId, css)) &&
                  WebHostPost(st->hwndSelf, msg::MakeFx(cfg.font)) &&
                  WebHostPost(st->hwndSelf, msg::MakeNetMode(cfg.netImageMode)) &&
                  WebHostPost(st->hwndSelf, msg::MakeCfg(cfg));
  st->chromePushed = ok;

  LogLine(L"VIEWER: 已推送皮肤 \"%s\"（%zu 字符）、字体（%dpx / %.2f / %dem）、网络图档位 %d、"
          L"渲染开关 %d%d%d%d%d",
          skinId.c_str(), css.size(), cfg.font.sizePx, cfg.font.lineHeight, cfg.font.measureEm,
          cfg.netImageMode, cfg.renderMermaid ? 1 : 0, cfg.renderKatex ? 1 : 0,
          cfg.renderHighlight ? 1 : 0, cfg.renderInlineHtml ? 1 : 0,
          cfg.renderLocalImages ? 1 : 0);
}

void PushBigTable(ViewerState* st) {
  if (!st->docOpened || !st->hasHost || st->webFailed || !WebHostIsReady()) return;
  std::string table;
  docsrc::BigTable(&table);
  WebHostPost(st->hwndSelf, msg::MakeBig(table));
  size_t items = table.empty() ? 0 : 1;
  for (char c : table) {
    if (c == ',') ++items;
  }
  const std::string head = table.size() > 200 ? table.substr(0, 200) + "..." : table;
  LogLine(L"VIEWER: 推送巨型块表 %zu 项 \"%hs\"", items, head.c_str());
}

void PushIndex(ViewerState* st) {
  if (!st->docOpened || !st->hasHost || st->webFailed || !WebHostIsReady()) return;

  if (!WebHostSetDocFolder(UtilParentDir(st->szFile))) {
    LogLine(L"VIEWER: mddoc.local 映射失败，相对图片不会加载");
  }

  const bool ok = WebHostPost(
      st->hwndSelf,
      msg::MakeIndex(kDocBase, st->prefixBlocks, st->estBlocks, st->ullFileSize, st->encoding,
                     st->lastVisible));
  LogLine(L"VIEWER: 推送索引 %u/%u 块（末可见块 %u）-> %s", st->prefixBlocks, st->estBlocks,
          st->lastVisible, ok ? L"成功" : L"失败");
  // 索引先到（页面会重置状态），巨型块表随后到，按块号生效
  PushBigTable(st);
}

void MaybePushIndexFull(ViewerState* st) {
  if (!st->indexFullPending || !st->docOpened || !st->hasHost || !WebHostIsReady()) return;
  const uint32_t count = docsrc::PublishedBlockCount();
  st->lastVisible = docsrc::LastVisible();
  if (!WebHostPost(st->hwndSelf, msg::MakeIndexFull(count, st->lastVisible))) return;
  PushBigTable(st);
  st->indexFullPending = false;
  LogLine(L"VIEWER: 已通知前端全文索引 %u 块（末可见块 %u）", count, st->lastVisible);
}

void OpenDoc(ViewerState* st) {
  if (!st->hasHost || st->webFailed) return;

  LARGE_INTEGER t0;
  LARGE_INTEGER t1;
  ::QueryPerformanceCounter(&t0);

  docsrc::DocumentInfo info;
  std::wstring err;
  if (!docsrc::Open(st->hwndSelf, st->szFile, &info, &err)) {
    st->docOpened = false;
    LogLine(L"VIEWER: 打开文档失败 \"%s\"：%s", st->szFile, err.c_str());
    SetStatus(st, L"打开失败：%s", UtilLeafName(st->szFile));
    return;
  }

  st->docOpened = true;
  st->indexFullPending =
      !info.complete && docsrc::PublishedBlockCount() > info.prefixBlocks;
  st->prefixBlocks = info.prefixBlocks;
  st->estBlocks = info.estimatedBlocks;
  st->lastVisible = info.lastVisible;
  st->encoding = info.encoding;
  st->ullFileSize = info.fileSize;

  ::QueryPerformanceCounter(&t1);
  LogLine(L"VIEWER: 索引就绪 prefix=%u est=%u，同步扫描 %.1fms", info.prefixBlocks,
          info.estimatedBlocks, QpcMs(t0.QuadPart, t1.QuadPart));

  PushChrome(st);
  PushIndex(st);
  MaybePushIndexFull(st);
  SetStatus(st, L"%s — %llu B", UtilLeafName(st->szFile), st->ullFileSize);
  ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
}

// ---- 宿主事件（由宿主线程 PostMessage 过来，运行在查看器自己的线程上） ----

void OnHostReady(ViewerState* st) {
  PushChrome(st);
  if (st->docOpened) PushIndex(st);
  MaybePushIndexFull(st);
  ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
}

void OnHostMessage(ViewerState* st, const std::wstring& frame) {
  std::wstring type;
  std::wstring rest;
  if (!msg::SplitType(frame, &type, &rest)) return;

  if (type == L"event") {
    const std::wstring kind = msg::SecondField(rest);
    const std::wstring detail = rest.size() > kind.size() + 1 ? rest.substr(kind.size() + 1) : L"";

    if (kind == L"rendered") {
      LARGE_INTEGER t1;
      ::QueryPerformanceCounter(&t1);
      const double total = st->qpcLoad ? QpcMs(st->qpcLoad, t1.QuadPart) : 0.0;
      LogLine(L"VIEWER: 首屏渲染完成 %s（LOADW 起 %.1fms）", detail.c_str(), total);
      LogMemory(L"首屏后");
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

void OnHostFailed(ViewerState* st, const wchar_t* reason) {
  st->webFailed = true;
  SetStatus(st, L"预览不可用：%s", reason ? reason : L"WebView2 初始化失败");
  ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
}

void HandleHostEvent(ViewerState* st, int kind, const wchar_t* text) {
  switch (kind) {
    case MDV_EVT_READY:
      OnHostReady(st);
      return;
    case MDV_EVT_FAILED:
      OnHostFailed(st, text);
      return;
    case MDV_EVT_MESSAGE:
      if (text) OnHostMessage(st, std::wstring(text));
      return;
    default:
      return;
  }
}

// ---- 消息处理 ----

void HandleLoad(ViewerState* st, const wchar_t* pszPath) {
  if (!pszPath) pszPath = L"";
  // 相对路径会让 mddoc.local 映射失败（映射要求绝对路径），这里统一转绝对。
  wchar_t szFull[MAX_PATH * 2];
  const DWORD cchFull = ::GetFullPathNameW(pszPath, ARRAYSIZE(szFull), szFull, nullptr);
  if (cchFull > 0 && cchFull < ARRAYSIZE(szFull)) {
    ::StringCchCopyW(st->szFile, ARRAYSIZE(st->szFile), szFull);
  } else {
    ::StringCchCopyW(st->szFile, ARRAYSIZE(st->szFile), pszPath);
  }
  pszPath = st->szFile;

  WIN32_FILE_ATTRIBUTE_DATA fad;
  st->ullFileSize = 0;
  if (::GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad)) {
    st->ullFileSize = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  }

  LARGE_INTEGER tLoad;
  ::QueryPerformanceCounter(&tLoad);
  st->qpcLoad = tLoad.QuadPart;

  st->docOpened = false;
  docsrc::CloseIfOwner(st->hwndSelf);

  if (!st->hasHost) {
    // 宿主在另一个查看器手里：先记住路径，收回宿主时再开索引。
    LogLine(L"VIEWER: 宿主不在本窗口，先不建索引（%s）", pszPath);
    SetStatus(st, L"%s — %llu B（预览正显示在另一个窗口）", UtilLeafName(pszPath),
              st->ullFileSize);
    return;
  }

  OpenDoc(st);
}

void HandleClear(ViewerState* st) {
  LogLine(L"VIEWER: CLEAR");
  st->szFile[0] = L'\0';
  st->ullFileSize = 0;
  st->docOpened = false;
  st->indexFullPending = false;
  docsrc::CloseIfOwner(st->hwndSelf);

  if (st->hasHost && WebHostIsReady()) {
    WebHostPost(st->hwndSelf, msg::MakeClear());
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
      WebHostEnsure();
      st->hasHost = WebHostBorrow(hwnd, ClientRectOf(hwnd));
      RegisterViewer(hwnd);
      return 0;
    }

    case WM_SIZE:
      if (st->hasHost) WebHostSetBounds(hwnd, ClientRectOf(hwnd));
      return 0;

    // 宿主被别的查看器借走了。画面跟着走了，这里只剩一块空白，等内容回来再补。
    case WM_MDV_HOST_LOST:
      LogLine(L"VIEWER: 宿主被借走，转为空白");
      st->hasHost = false;
      st->docOpened = false;
      st->indexFullPending = false;
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    // 宿主还回来了（原先借用它的窗口关了）。重新挂上并重开本文档。
    case WM_MDV_RECLAIM:
      if (!WebHostBorrow(hwnd, ClientRectOf(hwnd))) {
        LogLine(L"VIEWER: 收回宿主失败");
        return 0;
      }
      st->hasHost = true;
      LogLine(L"VIEWER: 收回宿主，重开文档");
      LARGE_INTEGER tReclaim;
      ::QueryPerformanceCounter(&tReclaim);
      st->qpcLoad = tReclaim.QuadPart;
      if (st->szFile[0]) {
        OpenDoc(st);
      } else {
        PushChrome(st);
        if (WebHostIsReady()) WebHostPost(hwnd, msg::MakeClear());
      }
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_MDV_INDEX_FULL:
      st->indexFullPending = true;
      MaybePushIndexFull(st);
      return 0;

    case WM_MDV_CONFIG_CHANGED:
      LogLine(L"VIEWER: 设置已更新，重推皮肤/字体/网络图档位");
      PushChrome(st);
      return 0;

    case WM_MDV_EVENT: {
      MDV_EVENT* evt = reinterpret_cast<MDV_EVENT*>(lParam);
      if (!evt) return 0;
      HandleHostEvent(st, evt->kind, evt->text);
      delete[] evt->text;
      delete evt;
      return 0;
    }

    case WM_DESTROY:
      // 必须在这里归还宿主：DestroyWindow 会连子窗口一起销毁，而宿主此刻正是
      // 这个窗口的子窗口。等到 WM_NCDESTROY 就晚了——宿主已经没了，controller
      // 跟着失效（之后每次 postMessage 都是 0x8007139F）。
      WebHostUnborrow(hwnd);
      docsrc::CloseIfOwner(hwnd);
      st->docOpened = false;
      st->hasHost = false;
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
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      if (st->webFailed) {
        ::FillRect(hdc, &rc, ::GetSysColorBrush(COLOR_WINDOW));
        ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));
        ::SetBkMode(hdc, TRANSPARENT);
        ::DrawTextW(hdc, L"WebView2 不可用，无法预览 Markdown。详见日志。", -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      } else if (!st->hasHost) {
        // 宿主在另一个查看器手里：给一行说明，别留一片白。
        ::FillRect(hdc, &rc, ::GetSysColorBrush(COLOR_WINDOW));
        ::SetTextColor(hdc, ::GetSysColor(COLOR_GRAYTEXT));
        ::SetBkMode(hdc, TRANSPARENT);
        ::DrawTextW(hdc, L"预览正显示在另一个查看器窗口中", -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      } else if (!WebHostIsReady() && st->szFile[0]) {
        // 冷启动/重建时 WebView2 要两三秒才出画面，先把文件名和状态画出来。
        ::FillRect(hdc, &rc, ::GetSysColorBrush(COLOR_WINDOW));
        ::SetBkMode(hdc, TRANSPARENT);

        RECT rcName = rc;
        rcName.bottom = (rc.top + rc.bottom) / 2;
        ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));
        ::DrawTextW(hdc, UtilLeafName(st->szFile), -1, &rcName,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT rcHint = rc;
        rcHint.top = rcName.bottom;
        ::SetTextColor(hdc, ::GetSysColor(COLOR_GRAYTEXT));
        ::DrawTextW(hdc, L"正在加载预览…", -1, &rcHint,
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
        WebHostSetBounds(hwnd, rc);
      } else {
        RECT rc = {0, 0, iWidth, iHeight};
        WebHostSetBounds(hwnd, rc);
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
      // 只是通知，不销毁窗口（viewer plugins.h:319）。配置变了就重推皮肤与字体；
      // 字号变化后的位置恢复由前端按锚点处理。
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
    UnregisterViewer(hwnd);
    if (st) {
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
  st->prefixBlocks = 0;
  st->estBlocks = 0;
  st->lastVisible = 0;
  st->encoding = 0;
  st->qpcLoad = 0;
  st->docOpened = false;
  st->indexFullPending = false;
  st->chromePushed = false;
  st->webFailed = false;
  st->hasHost = false;
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

void ViewerNotifyConfigChanged(void) {
  std::vector<HWND> targets;
  {
    std::lock_guard<std::mutex> lock(g_viewerMutex);
    targets = g_viewers;
  }
  for (HWND hwnd : targets) {
    if (::IsWindow(hwnd)) ::PostMessageW(hwnd, WM_MDV_CONFIG_CHANGED, 0, 0);
  }
}
