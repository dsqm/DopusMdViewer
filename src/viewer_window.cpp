// viewer_window.cpp - 查看器窗口与消息分发。
//
// M4 起正文不再整篇读入推送：打开时由 doc_source 建索引（同步前缀 + 后台全文），
// 这里只推 index 控制消息，块正文由页面按区间向 mdblocks.localhost 取。

#include "viewer_window.h"

#include <cstdarg>
#include <cstring>
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
const wchar_t kDocBase[] = L"https://mddoc.localhost/";

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
  uint32_t encOverride;  // 手动指定的代码页，0 = 自动
  std::vector<DVPCONTEXTMENUITEMW> ctxMenu;  // 右键菜单数组，必须活到下次请求/窗口销毁
  bool docComplete;  // 打开时索引已覆盖全文（没有后台扫描）
  LONGLONG qpcLoad;  // LOADW 起点，用于端到端耗时
  bool docOpened;    // doc_source 归本窗口
  bool indexFullPending;  // 全文索引已就绪但页面还没收到 index-full
  bool chromePushed;
  bool webFailed;
  bool hasHost;  // 宿主此刻是否在本窗口（单宿主，被别的窗口借走时没有画面）
  bool hasSelection;  // 页面里此刻有没有非折叠选区，DVPLUGINMSG_TESTSELECTION 靠它回答
  // 加载占位的自绘字体：按 DPI 缩放 + ClearType，避免默认位图字体的锯齿。
  HFONT fontName;
  HFONT fontHint;
  UINT fontDpi;
  int fontNamePx;
  int fontHintPx;
  wchar_t fontFace[LF_FACESIZE];
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
    {DVPLUGINMSG_PRINT, L"PRINT"},
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

void SetStatus(ViewerState* st, const wchar_t* pszFormat, ...) {  va_list args;
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

// 占位文字不能用默认的 System 位图字体：175% 缩放下又小又锯齿。这里按窗口 DPI
// 和当前字号设置生成 ClearType 字体，系统 UI 字体负责中文字形。
void EnsurePlaceholderFonts(ViewerState* st, HWND hwnd) {
  UINT dpi = ::GetDpiForWindow(hwnd);
  if (dpi == 0) dpi = 96;

  wchar_t face[LF_FACESIZE] = L"Microsoft YaHei UI";
  NONCLIENTMETRICSW ncm = {};
  ncm.cbSize = sizeof(ncm);
  if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) &&
      ncm.lfMessageFont.lfFaceName[0]) {
    ::StringCchCopyW(face, ARRAYSIZE(face), ncm.lfMessageFont.lfFaceName);
  }

  const int sizePx = ConfigSnapshot().font.sizePx;
  const int namePx = ::MulDiv(sizePx + 3, static_cast<int>(dpi), 96);
  const int hintPx = ::MulDiv(sizePx, static_cast<int>(dpi), 96);

  if (st->fontDpi == dpi && st->fontNamePx == namePx && st->fontHintPx == hintPx &&
      _wcsicmp(st->fontFace, face) == 0) {
    return;
  }

  if (st->fontName) ::DeleteObject(st->fontName);
  if (st->fontHint) ::DeleteObject(st->fontHint);

  LOGFONTW lf = {};
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
  ::StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), face);

  lf.lfHeight = -namePx;
  st->fontName = ::CreateFontIndirectW(&lf);
  lf.lfHeight = -hintPx;
  st->fontHint = ::CreateFontIndirectW(&lf);

  st->fontDpi = dpi;
  st->fontNamePx = namePx;
  st->fontHintPx = hintPx;
  ::StringCchCopyW(st->fontFace, ARRAYSIZE(st->fontFace), face);
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
  if (!::OpenClipboard(owner)) return false;
  ::EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  bool ok = false;
  if (mem) {
    void* p = ::GlobalLock(mem);
    if (p) {
      std::memcpy(p, text.c_str(), bytes);
      ::GlobalUnlock(mem);
      ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
      if (!ok) ::GlobalFree(mem);
    } else {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
  return ok;
}

// 页面已经把全文渲染成纯文本（copyset），这里只负责写剪贴板与状态提示。
void CopyRenderedText(ViewerState* st, int reason, const std::wstring& text) {
  if (!SetClipboardText(st->hwndSelf, text)) {
    SetStatus(st, L"复制失败：剪贴板被占用");
    return;
  }
  LogLine(L"VIEWER: 复制渲染文本 %zu 字符，原因 %d", text.size(), reason);
  if (reason == 1) {
    SetStatus(st, L"文档过大，已复制前 %zu 字符的渲染内容（未能全部复制）", text.size());
  } else if (reason == 2) {
    SetStatus(st, L"全文索引未完成，已复制前 %zu 字符，请稍后重试", text.size());
  } else {
    SetStatus(st, L"已复制全文（%zu 字符）", text.size());
  }
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

// 打印：全文不在 DOM 里，先让页面把全文渲染进打印容器，就绪消息回来再弹对话框。
void StartPrint(ViewerState* st) {
  if (!st->docOpened) {
    SetStatus(st, L"没有可打印的文档");
    return;
  }
  if (!st->hasHost || st->webFailed || !WebHostIsReady()) {
    SetStatus(st, L"预览未就绪，无法打印");
    return;
  }
  LogLine(L"VIEWER: 打印请求，等页面准备全文");
  SetStatus(st, L"正在准备打印…");
  if (!WebHostPost(st->hwndSelf, L"print|")) SetStatus(st, L"打印准备失败：页面不可达");
}

void PushIndex(ViewerState* st) {
  if (!st->docOpened || !st->hasHost || st->webFailed || !WebHostIsReady()) return;

  if (!WebHostSetDocFolder(UtilParentDir(st->szFile))) {
    LogLine(L"VIEWER: mddoc.localhost 映射失败，相对图片不会加载");
  }

  const bool ok = WebHostPost(
      st->hwndSelf,
      msg::MakeIndex(kDocBase, st->prefixBlocks, st->estBlocks, st->ullFileSize, st->encoding,
                     st->lastVisible, st->docComplete));
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
  if (!docsrc::Open(st->hwndSelf, st->szFile, &info, &err, st->encOverride)) {
    st->docOpened = false;
    const std::wstring name = UtilLeafName(st->szFile);
    const std::wstring why = err.empty() ? std::wstring(L"读取失败") : err;
    LogLine(L"VIEWER: 打开文档失败 \"%s\"：%s", st->szFile, err.c_str());
    SetStatus(st, L"打开失败：%s", name.c_str());
    // 不清空页面就会留着上一个文件的内容，很容易误以为失败的是那个文件。
    WebHostPost(st->hwndSelf, msg::MakeClear());
    WebHostPost(st->hwndSelf, msg::MakeDocError(L"无法预览此文件\n\n" + name + L"\n" + why));
    ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
    return;
  }

  st->docOpened = true;
  st->indexFullPending =
      !info.complete && docsrc::PublishedBlockCount() > info.prefixBlocks;
  st->prefixBlocks = info.prefixBlocks;
  st->estBlocks = info.estimatedBlocks;
  st->lastVisible = info.lastVisible;
  st->encoding = info.encoding;
  st->docComplete = info.complete;
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

// ---- 右键菜单：编码子菜单 ----
// Opus 在显示查看器右键菜单前发 DVPLUGINMSG_ADDCONTEXTMENU；返回的数组与字符串
// 必须活到查看器销毁（这里存在 ViewerState 里，字符串是字面量）。
constexpr UINT kEncMenuBase = 0x6D00;

struct EncMenuEntry {
  const wchar_t* label;
  uint32_t codepage;
};

const EncMenuEntry kEncMenu[] = {
    {L"自动", 0},           {L"UTF-8", 65001},       {L"UTF-16LE", 1200},
    {L"UTF-16BE", 1201},    {L"GB18030", 54936},     {L"Shift_JIS", 932},
    {L"EUC-JP", 20932},     {L"ISO-2022-JP", 50220}, {L"Big5", 950},
    {L"EUC-KR", 949},       {L"Windows-1251", 1251}, {L"KOI8-R", 20866},
    {L"Windows-1252", 1252}, {L"ISO-8859-1", 28591},
};
constexpr size_t kEncMenuCount = sizeof(kEncMenu) / sizeof(kEncMenu[0]);

void BuildContextMenu(ViewerState* st) {
  st->ctxMenu.clear();
  DVPCONTEXTMENUITEMW parent = {};
  parent.lpszLabel = const_cast<LPWSTR>(L"编码");
  parent.dwFlags = DVPCMF_BEGINSUBMENU | (st->szFile[0] ? 0 : DVPCMF_DISABLED);
  st->ctxMenu.push_back(parent);
  for (size_t i = 0; i < kEncMenuCount; ++i) {
    DVPCONTEXTMENUITEMW item = {};
    item.lpszLabel = const_cast<LPWSTR>(kEncMenu[i].label);
    item.dwFlags = DVPCMF_RADIOCHECK;
    if (kEncMenu[i].codepage == st->encOverride) item.dwFlags |= DVPCMF_CHECKED;
    if (i + 1 == kEncMenuCount) item.dwFlags |= DVPCMF_ENDSUBMENU;
    item.uID = kEncMenuBase + static_cast<UINT>(i);
    st->ctxMenu.push_back(item);
  }
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

  if (type == L"print-ready") {
    const bool ok = WebHostShowPrintUI(st->hwndSelf);
    LogLine(L"VIEWER: 打印内容就绪，ShowPrintUI %s", ok ? L"成功" : L"失败");
    if (!ok) SetStatus(st, L"打印不可用：WebView2 运行时过旧");
    return;
  }

  if (type == L"print-done") {
    LogLine(L"VIEWER: 打印对话框已关闭");
    return;
  }

  if (type == L"copyset") {
    const std::wstring flag = msg::SecondField(rest);
    const std::wstring text = rest.size() > flag.size() + 1 ? rest.substr(flag.size() + 1)
                                                             : std::wstring();
    CopyRenderedText(st, _wtoi(flag.c_str()), text);
    return;
  }

  if (type == L"status") {
    SetStatus(st, L"%s", rest.c_str());
    return;
  }

  if (type == L"ctxmenu") {
    // 右键落在 WebView2 子窗口上，父窗口收不到 WM_CONTEXTMENU；转发给 Opus，
    // 它才会显示查看器菜单并向我们索取「编码」子菜单。
    if (st->hwndParent) {
      POINT pt = {};
      ::GetCursorPos(&pt);
      ::SendMessageW(st->hwndParent, WM_CONTEXTMENU,
                     reinterpret_cast<WPARAM>(st->hwndSelf), MAKELPARAM(pt.x, pt.y));
    }
    return;
  }

  if (type == L"sel") {
    // 宿主决定「复制」是否可用时要问 TESTSELECTION，这里记下页面报的实况。
    const bool on = (rest == L"1");
    if (on != st->hasSelection) {
      st->hasSelection = on;
      LogLine(L"VIEWER: 选区 %s", on ? L"有" : L"无");
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

void OnHostRecovering(ViewerState* st) {
  LogLine(L"VIEWER: 预览进程失效，等宿主重载页面");
  // 重载期间页面是空的：占位文字由 OnPaint 按「未就绪」自己顶上来。
  SetStatus(st, L"预览进程异常，正在恢复…");
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
    case MDV_EVT_RECOVERING:
      OnHostRecovering(st);
      return;
    default:
      return;
  }
}

// ---- 消息处理 ----

void HandleLoad(ViewerState* st, const wchar_t* pszPath) {
  if (!pszPath) pszPath = L"";
  // 相对路径会让 mddoc.localhost 映射失败（映射要求绝对路径），这里统一转绝对。
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
  st->encOverride = 0;  // 换文件回到自动判码
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
  st->encOverride = 0;
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
      st->docComplete = true;
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

      const bool loading = !st->webFailed && st->hasHost && !WebHostIsReady() && st->szFile[0];
      if (st->webFailed || !st->hasHost || loading) {
        EnsurePlaceholderFonts(st, hwnd);
        ::FillRect(hdc, &rc, ::GetSysColorBrush(COLOR_WINDOW));
        // ClearType 需要知道背景色；文字画在刚刷白的底色上。
        ::SetBkMode(hdc, OPAQUE);
        ::SetBkColor(hdc, ::GetSysColor(COLOR_WINDOW));
      }

      if (st->webFailed) {
        HGDIOBJ oldFont = ::SelectObject(
            hdc, st->fontHint ? st->fontHint : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));
        ::DrawTextW(hdc, L"WebView2 不可用，无法预览 Markdown。详见日志。", -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SelectObject(hdc, oldFont);
      } else if (!st->hasHost) {
        // 宿主在另一个查看器手里：给一行说明，别留一片白。
        HGDIOBJ oldFont = ::SelectObject(
            hdc, st->fontHint ? st->fontHint : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(hdc, ::GetSysColor(COLOR_GRAYTEXT));
        ::DrawTextW(hdc, L"预览正显示在另一个查看器窗口中", -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SelectObject(hdc, oldFont);
      } else if (loading) {
        // 冷启动/重建时 WebView2 要一会儿才出画面，先把文件名和状态画出来。
        const UINT dpi = st->fontDpi ? st->fontDpi : 96;
        HGDIOBJ oldFont = ::SelectObject(
            hdc, st->fontName ? st->fontName : ::GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW tmName = {};
        ::GetTextMetricsW(hdc, &tmName);
        ::SelectObject(hdc, st->fontHint ? st->fontHint : ::GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW tmHint = {};
        ::GetTextMetricsW(hdc, &tmHint);

        const int gap = ::MulDiv(6, static_cast<int>(dpi), 96);
        const int pad = ::MulDiv(16, static_cast<int>(dpi), 96);
        const int total = tmName.tmHeight + gap + tmHint.tmHeight;
        int top = rc.top + ((rc.bottom - rc.top) - total) / 2;
        if (top < rc.top + pad) top = rc.top + pad;

        RECT rcName = {rc.left + pad, top, rc.right - pad, top + tmName.tmHeight};
        RECT rcHint = {rc.left + pad, top + tmName.tmHeight + gap, rc.right - pad,
                       top + tmName.tmHeight + gap + tmHint.tmHeight};

        ::SelectObject(hdc, st->fontName ? st->fontName : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(hdc, ::GetSysColor(COLOR_WINDOWTEXT));
        ::DrawTextW(hdc, UtilLeafName(st->szFile), -1, &rcName,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(hdc, st->fontHint ? st->fontHint : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(hdc, ::GetSysColor(COLOR_GRAYTEXT));
        ::DrawTextW(hdc, L"正在加载预览…", -1, &rcHint,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SelectObject(hdc, oldFont);
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

    case DVPLUGINMSG_PRINT:
      StartPrint(st);
      return 0;

    case DVPLUGINMSG_TESTSELECTION:
      // Opus 用它决定「复制」是否置灰：不实现就一直返回 0，菜单项永远是灰的。
      return st->hasSelection ? TRUE : FALSE;

    case DVPLUGINMSG_SELECTALL:
      // Opus 菜单里的「全选」只做选中；随后的「复制」走下一分支
      if (st->hasHost && WebHostIsReady()) WebHostPost(st->hwndSelf, L"selectall|");
      return 0;

    case DVPLUGINMSG_COPYSELECTION:
      // Opus 菜单/命令里的「复制」：有选区就复制选区文本，没有就把全文渲染文本放进剪贴板。
      if (st->hasHost && WebHostIsReady()) WebHostPost(st->hwndSelf, L"copy|");
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
      // COPYSELECTION 必须报：不报 Opus 会认为本查看器不能复制，「复制」菜单项恒灰。
      return VPCAPABILITY_WANTFOCUS | VPCAPABILITY_SELECTALL | VPCAPABILITY_COPYSELECTION |
             VPCAPABILITY_PRINT | VPCAPABILITY_ADDCONTEXTMENU;

    case DVPLUGINMSG_ADDCONTEXTMENU: {
      BuildContextMenu(st);
      if (wParam) *reinterpret_cast<DWORD*>(wParam) = static_cast<DWORD>(st->ctxMenu.size());
      return reinterpret_cast<LRESULT>(st->ctxMenu.data());
    }

    case WM_COMMAND: {
      const UINT id = LOWORD(wParam);
      if (id >= kEncMenuBase && id < kEncMenuBase + kEncMenuCount) {
        const uint32_t cp = kEncMenu[id - kEncMenuBase].codepage;
        if (cp != st->encOverride) {
          st->encOverride = cp;
          LogLine(L"VIEWER: 右键菜单指定编码 cp=%u，重开文档", cp);
          if (st->szFile[0]) OpenDoc(st);
        }
        return 0;
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

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
      if (st->fontName) ::DeleteObject(st->fontName);
      if (st->fontHint) ::DeleteObject(st->fontHint);
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
  st->encOverride = 0;
  st->docComplete = false;
  st->qpcLoad = 0;
  st->docOpened = false;
  st->indexFullPending = false;
  st->chromePushed = false;
  st->webFailed = false;
  st->hasHost = false;
  st->hasSelection = false;
  st->fontName = nullptr;
  st->fontHint = nullptr;
  st->fontDpi = 0;
  st->fontNamePx = 0;
  st->fontHintPx = 0;
  st->fontFace[0] = L'\0';
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
