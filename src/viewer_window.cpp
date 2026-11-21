// viewer_window.cpp - 查看器窗口与消息分发。
//
// 正文不再整篇读入推送：打开时由 doc_source 建索引（同步前缀 + 后台全文），
// 这里只推 index 控制消息，块正文由页面按区间向 mdblocks.localhost 取。

#include "viewer_window.h"

#include <cstdarg>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>
#include <objbase.h>  // CoInitializeEx / CoTaskMemFree（图片「打开方式」要进 COM STA）
#include <objidl.h>  // LPSTREAM，"viewer plugins.h" 需要（WIN32_LEAN_AND_MEAN 会藏掉）
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>     // SHAssocEnumHandlers / SHOpenWithDialog：图片「打开方式」
#include <shobjidl.h>   // IFileSaveDialog：导出 PDF 的保存位置
#include <shlwapi.h>    // AssocQueryString：ProgID → 友好应用名
#include <strsafe.h>
#include <wincodec.h>   // WIC：UWP 图标引用解析出的 AppList PNG → 菜单位图

#include "config.h"
#include "doc_source.h"
#include "log.h"
#include "msg.h"
#include "pdfmerge.h"
#include "skin.h"
#include "util.h"
#include "viewer plugins.h"
#include "webhost.h"

namespace {

const wchar_t kClassName[] = L"DopusMdViewerWindow";
const wchar_t kTitle[] = L"DopusMdViewer";
const wchar_t kDocBase[] = L"https://mddoc.localhost/";
// 深浅色没有可订阅的宿主通知，按半秒轮询 IsOpusInDarkMode；打印亮色的兜底恢复
// 要覆盖页面 5 分钟的打印超时。
constexpr UINT_PTR kDarkPollTimerId = 1;
constexpr UINT kDarkPollMs = 500;
// 状态栏属于 DOpus：我们只临时借几秒说一句，到点发 0 交还原生多分区内容。
// 提示本身不能长期占着——原生那条栏里的序号、缩放、大小、加载耗时都被顶掉了。
constexpr UINT_PTR kStatusResetTimerId = 2;
constexpr UINT kStatusResetMs = 4000;
// 对话框关闭后补做被延后的加载（见 HandleLoad）
constexpr UINT kMsgPendingLoad = WM_APP + 0x312;
constexpr ULONGLONG kPrintLightMaxMs = 6ull * 60 * 1000;
// 导出等引擎回调的上限：渲染进程崩掉时回调不会来。与 webhost 的 pdfBusy 回收
// （5 分钟）同长，否则宿主先放弃、引擎还在写，会留下孤儿 .part 文件。
// 单份按排版量切分，普通版面几秒、密集版面几十秒。
constexpr ULONGLONG kExportTimeoutMs = 5ull * 60 * 1000;
// 分段：单份源文本不超过 512 KB，且按采样得到的 DOM 节点密度切到分页成本拐点
// （引擎分页耗时随单段 DOM 规模超线性增长，段做大反而更慢）。
// 上限只作保护阀，不再是「文档大小」的天花板。
constexpr int kExportMaxSegs = 512;
// 页面协议版本：与 app.js 的 PAGE_PROTO 一一对应。旧页面发的是旧帧格式
// （export-ready 不带页数估算），静默解析会让进度估成错的，所以两边对不上就拒绝导出。
// 版本在每次导出前现问现答，不按窗口缓存：单宿主复用时新窗口不会重新加载页面。
constexpr int kPageProto = 6;
// 握手等应答的上限。页面应答是同步 postMessage，正常几毫秒；旧页面不认识
// proto-query，不会有任何回应。
constexpr ULONGLONG kProtoQueryTimeoutMs = 3000;
// 引擎分页（PrintToPdfStream）没有进度回调，预告时长与百分比只能折算。排版对象个数
// （DOM 节点数）是唯一与耗时同向的量；分段按节点预算切到成本拐点后，各份规模接近，
// 用「实测毫秒 / 节点数」这个单价折算即可。旧版按「2.6 秒固定开销 + 单价」拟合，
// 那是大段时代的截距：现在一份约 2 万节点、2.3 秒，固定开销会把预告抬到 5.7 秒。
constexpr double kExportMinMs = 1500;       // 起分页与写文件的下限，小文档按它兜底
constexpr double kExportPerNodeMs = 0.11;   // 约 2.3 秒 / 2.1 万节点
constexpr double kExportPerPageMs = 16.0;   // 拿不到节点数时的兜底（按页）
constexpr double kExportPerBlockMs = 0.8;   // 连页数也没有时的兜底（按块）

double g_exportMsPerNode = kExportPerNodeMs;
double g_exportMsPerPage = kExportPerPageMs;
double g_exportMsPerBlock = kExportPerBlockMs;
// 最近一份的实测耗时：份与份的内容量相近，用它推算剩余时间
double g_exportSegMs = 0;

// 单份预计耗时（毫秒）。nodes = 本份 DOM 节点数，pages = 估算页数，blocks = 块数
// （依次降级：页面报什么就用什么）。
double PredictExportMs(int nodes, int pages, int blocks) {
  double body;
  if (nodes > 0) {
    body = g_exportMsPerNode * nodes;
  } else if (pages > 0) {
    body = g_exportMsPerPage * pages;
  } else {
    body = g_exportMsPerBlock * (blocks > 0 ? blocks : 0);
  }
  if (body < kExportMinMs) return kExportMinMs;
  if (body > 10.0 * 60 * 1000) return 10.0 * 60 * 1000;
  return body;
}

// 按实测校正折算系数：页面报了什么就校正什么（节点优先，其后页数、块数）。
// 量太小的时候耗时几乎全在下限里，反推出来的单价没有意义，跳过（否则系数会被带偏）。
// first = 本次导出的第一份：上一轮的系数可能来自完全不同的文档，直接换成这次量到的值；
// 之后几份内容量相近，取加权平均把单次抖动压掉（直接覆盖会让进度条一跳一跳）。
void CalibrateExport(double spentMs, int nodes, int pages, int blocks, bool first) {
  if (spentMs <= 0) return;
  if (nodes > 200) {
    double v = spentMs / nodes;
    if (v < 0.01) v = 0.01;
    if (v > 200.0) v = 200.0;
    g_exportMsPerNode = first ? v : g_exportMsPerNode * 0.3 + v * 0.7;
    return;
  }
  if (pages > 0) {
    double v = spentMs / pages;
    if (v < 1.0) v = 1.0;
    if (v > 500.0) v = 500.0;
    g_exportMsPerPage = first ? v : g_exportMsPerPage * 0.3 + v * 0.7;
    return;
  }
  if (blocks < 2000) return;
  double v = spentMs / blocks;
  if (v < 0.05) v = 0.05;
  if (v > 60.0) v = 60.0;
  g_exportMsPerBlock = first ? v : g_exportMsPerBlock * 0.3 + v * 0.7;
}

struct ViewerState {
  HWND hwndSelf;
  HWND hwndParent;
  HANDLE hAbortEvent;  // 宿主给的加载中止事件，只记录不轮询（见 SETABORTEVENT 分支）
  wchar_t szFile[MAX_PATH * 2];
  ULONGLONG ullFileSize;
  // 页面判定「还是不是同一篇文档」用：小写路径 + 字节数 + 最后写入时间。
  // 打开时算一次就固定，之后同一次加载里推几次 index 都是同一个值。
  std::wstring docKey;
  uint32_t prefixBlocks;
  uint32_t estBlocks;
  uint32_t lastVisible;  // 最后一个非隐藏块（脚注区挂它后面）
  int encoding;
  uint32_t encOverride;  // 手动指定的代码页，0 = 自动
  bool docComplete;  // 打开时索引已覆盖全文（没有后台扫描）
  LONGLONG qpcLoad;  // LOADW 起点，用于端到端耗时
  bool docOpened;    // doc_source 归本窗口
  bool indexFullPending;  // 全文索引已就绪但页面还没收到 index-full
  bool webFailed;
  bool hasHost;  // 宿主此刻是否在本窗口（单宿主，被别的窗口借走时没有画面）
  bool hasSelection;  // 页面里此刻有没有非折叠选区，DVPLUGINMSG_TESTSELECTION 靠它回答
  // DOpus 当前深浅色：true = 暗色。拿不到信号时保持 false（用亮色框皮肤兜底）。
  bool darkMode;
  // 打印/导出期间改用配置里的打印皮肤；页面中止打印时不一定有 print-done，
  // 靠计时兜底恢复跟随。
  bool pagedSkin;
  ULONGLONG pagedStartTick;
  // 导出 PDF：路径在保存对话框里定下后一直留到 done/error 落地。
  bool exporting;
  std::wstring exportPath;
  // 分段导出：一份一个 PDF。页数由页面按块数切成几份后告知，这里只按段号命名。
  int exportSeg;
  int exportSegs;
  int exportOk;
  // 当前这一份的块数、页面估算的页数与预计耗时：进度百分比按它们折算，
  // 跑完后也用它校正每页/每块的耗时
  int exportSegBlocks;
  int exportSegPages;
  int exportSegNodes;
  double exportPredMs;
  // 整篇口径：起点时刻、本份已铺块数、已完成各份的实测耗时合计。
  // 状态栏只报「整篇进度 + 整篇剩余」，这三个值让它在换段时也不断档。
  ULONGLONG exportRunStartTick;
  int exportSegBuiltBlocks;
  double exportDoneMs;
  // 状态栏去重：铺全文时进度帧每批都来，同样的数字重复推没有意义
  int exportLastPct;
  int exportLastSeg;
  wchar_t exportLastTail[48];
  // 分段时各份先落到 .partNN.pdf，全部写完再合并成用户要的那一份
  std::vector<std::wstring> exportParts;
  // 导出前的协议握手：版本属于页面资源，不属于某个窗口，所以不缓存、每次现问
  bool protoQueryPending;
  ULONGLONG protoQueryTick;
  // 导出计划：份数由页面按真实块数算，宿主拿到后才弹确认框、才开始
  bool exportPlanPending;
  ULONGLONG planTick;
  std::wstring testExportPath;  // 仅 hosttest：跳过两个对话框时用的目标路径
  // 已发起 PrintToPdfStream、等引擎回调：这一段没有百分比，用秒数报活
  bool waitingPdf;
  ULONGLONG lastStatusSec;
  // 当前皮肤 --md-bg：页面出来前宿主窗口先铺同色，暗色下不闪白。
  COLORREF bgColor;
  // 页面最后右键的图片（imgctx 帧）：「打开方式」要还原磁盘路径，「放大查看」
  // 原样生效。只在弹菜单时读一次，空 = 无图片上下文（全局菜单不带图片动作区）。
  std::wstring imgCtxUrl;
  std::wstring imgCtxKind;  // local（已还原出磁盘路径的候选）/ remote（只可放大）
  // CreateWindowExW 期间的一次性凭据：WM_NCCREATE 认领 st 后置 true，此后 st 的
  // 生命周期归窗口管（WM_NCDESTROY 释放）。创建失败时靠它决定谁 delete。
  bool* pClaimed;
  // 加载占位的自绘字体：按 DPI 缩放 + ClearType，避免默认位图字体的锯齿。
  HFONT fontName;
  HFONT fontHint;
  UINT fontDpi;
  int fontNamePx;
  int fontHintPx;
  wchar_t fontFace[LF_FACESIZE];
  // DVPNMSTATUSTEXT 的指针在查看器销毁前一直有效，文本要活得比调用长。
  wchar_t szStatus[512];
  // 确认框/保存框开着时为 true：这期间 DOpus 发来的 LOADW 一律延后处理。
  // 模态对话框有自己的消息循环，不挡住的话换文档会插进导出流程中间，
  // 对话框返回后就会拿旧文档的状态继续跑。
  bool dlgBusy;
  bool pendingLoad;
  wchar_t pendingFile[MAX_PATH * 2];
  // 存活令牌：窗口可能在对话框开着的时候被销毁（DOpus 换文件时重建窗格）。
  // 调用方在弹框前取一份 shared_ptr，返回后靠它判断自己还能不能碰 st。
  std::shared_ptr<bool> alive;
};

LONG g_liveViewers = 0;
HINSTANCE g_hInstance = nullptr;

// 状态栏的导出进度（整篇百分比 + 整篇剩余）：铺全文、引擎分页、换段三处共用一套算法
void UpdateExportStatus(ViewerState* st);

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

double QpcMs(LONGLONG from, LONGLONG to) {
  static LONGLONG freq = 0;
  if (freq == 0) {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    freq = f.QuadPart ? f.QuadPart : 1;
  }
  return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq);
}

// 把 "a|b|c" 逐段转成整数，缺的补 0。
void ParseIntFields(const std::wstring& rest, int* out, int count) {
  const wchar_t* p = rest.c_str();
  for (int i = 0; i < count; ++i) {
    if (!p) {
      out[i] = 0;
      continue;
    }
    out[i] = _wtoi(p);
    const wchar_t* bar = wcschr(p, L'|');
    p = bar ? bar + 1 : nullptr;
  }
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
  // pszText 为 0 才是「复位成 DOpus 原生内容」；传 L"" 只会上报一条空文本，
  // 状态栏仍然被我们占着（SDK 的复位方式只有这一种）。
  nmt.lpszStatusText = const_cast<LPTSTR>(pszText);
  nmt.fUnicode = TRUE;

  ::SendMessageW(st->hwndParent, WM_NOTIFY, nmt.hdr.idFrom, reinterpret_cast<LPARAM>(&nmt));
}

int ColorLuma(COLORREF c) {
  return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

// 与首帧皮肤共用 util 里的同一套判定，别在这里另算一套：两处不一致会先显示
// 一种主题、再切到另一种。每 10 秒记一行判定值便于取证。
bool HostThemeDark(ViewerState* st) {
  bool reliable = false;
  const bool dark = UtilHostThemeDark(st->hwndParent, &reliable);
  static ULONGLONG lastLog = 0;
  const ULONGLONG now = ::GetTickCount64();
  if (!lastLog || now - lastLog > 10000) {
    lastLog = now;
    LogLine(L"VIEWER: 主题判定 dark=%d 导出=%d 可靠=%d", dark ? 1 : 0,
            UtilIsOpusDarkMode() ? 1 : 0, reliable ? 1 : 0);
  }
  return dark;
}

void SetStatus(ViewerState* st, const wchar_t* pszFormat, ...) {  va_list args;
  va_start(args, pszFormat);
  ::StringCchVPrintfW(st->szStatus, ARRAYSIZE(st->szStatus), pszFormat, args);
  va_end(args);
  SendStatusText(st, st->szStatus);
  // 借来的状态栏几秒后必须还：进度类提示每帧都调这里，定时器随之续期，
  // 跑完最后一句再到期复位；没有后续更新的长操作则提前交还原生内容。
  if (st->hwndSelf) {
    ::SetTimer(st->hwndSelf, kStatusResetTimerId, kStatusResetMs, nullptr);
  }
}

// 把状态栏还给 DOpus。注意必须是 lpszStatusText=0，不能是空串——
// 空串只是把我们的文本改成空，原生分区仍然回不来。
void ResetStatus(ViewerState* st) {
  if (!st || !st->hwndSelf) return;
  ::KillTimer(st->hwndSelf, kStatusResetTimerId);
  st->szStatus[0] = L'\0';
  SendStatusText(st, nullptr);
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
  HFONT madeName = ::CreateFontIndirectW(&lf);
  lf.lfHeight = -hintPx;
  HFONT madeHint = ::CreateFontIndirectW(&lf);
  st->fontName = madeName;
  st->fontHint = madeHint;

  // 建字体失败就不写缓存：写进去之后这句 if 永远命中，再也不会重试，
  // 界面会一直退到 System 位图字体。
  if (!madeName || !madeHint) {
    LogLine(L"VIEWER: 占位字体创建失败，下次重画再试");
    return;
  }

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

// 代码块 / 表格右上角的复制按钮（clip）：页面只给「代码」「表格」这样的标签和正文，
// 提示文案统一在这里拼，免得页面里再散一份状态栏文案。
void CopyLabeledText(ViewerState* st, const std::wstring& what, const std::wstring& text) {
  if (text.empty()) {
    SetStatus(st, L"没有可复制的内容");
    return;
  }
  if (!SetClipboardText(st->hwndSelf, text)) {
    SetStatus(st, L"复制失败：剪贴板被占用");
    return;
  }
  LogLine(L"VIEWER: 复制%s %zu 字符", what.c_str(), text.size());
  SetStatus(st, L"已复制%s（%zu 字符）", what.c_str(), text.size());
}

// 皮肤与字体永远成对推送：先皮肤，再 fx（fx 最后注入且写全量槽位，压住皮肤）。
void PushChrome(ViewerState* st) {
  const Config cfg = ConfigSnapshot();
  // 打印与导出期间用设置里的打印皮肤；其余时刻跟随 DOpus 深浅色
  const bool paged = st->pagedSkin;
  const bool dark = st->darkMode && !paged;

  std::wstring css;
  std::wstring skinId = paged ? cfg.printSkin : (dark ? cfg.skinDark : cfg.skinLight);
  if (!SkinLoad(skinId, &css)) {
    skinId = L"light";
    if (!SkinLoad(skinId, &css)) {
      LogLine(L"VIEWER: 连内置 light 皮肤都读不到，退回 base.css 的默认变量");
    }
  }

  // 加载占位也用它铺底：页面没就绪时窗口是可见的，暗色下不能刷白
  const COLORREF bg =
      UtilParseSkinBg(css, dark ? RGB(0x28, 0x2c, 0x34) : ::GetSysColor(COLOR_WINDOW));
  if (bg != st->bgColor) {
    st->bgColor = bg;
    if (st->hwndSelf) ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
  }
  // WebView2 自己也铺同色：换尺寸/恢复挂起时露出的默认底色不能是白的
  WebHostSetBackground(bg);

  if (!st->hasHost || st->webFailed || !WebHostIsReady()) return;
  const bool ok = WebHostPost(st->hwndSelf, msg::MakeSkin(skinId, css)) &&
                  WebHostPost(st->hwndSelf, msg::MakeFx(cfg.font)) &&
                  WebHostPost(st->hwndSelf, msg::MakeNetMode(cfg.netImageMode)) &&
                  WebHostPost(st->hwndSelf, msg::MakeLinkWait(cfg.linkConfirmWait)) &&
                  WebHostPost(st->hwndSelf, msg::MakeCfg(cfg));
  if (!ok) LogLine(L"VIEWER: 皮肤/字体推送失败（宿主不在或页面未就绪）");

  LogLine(L"VIEWER: 已推送皮肤 \"%s\"（%zu 字符）、字体（%dpx / %.2f / %dem）、网络图档位 %d、"
          L"渲染开关 %d%d%d%d%d、行号 %d、链接确认等待 %d",
          skinId.c_str(), css.size(), cfg.font.sizePx, cfg.font.lineHeight, cfg.font.measureEm,
          cfg.netImageMode, cfg.renderMermaid ? 1 : 0, cfg.renderKatex ? 1 : 0,
          cfg.renderHighlight ? 1 : 0, cfg.renderInlineHtml ? 1 : 0,
          cfg.renderLocalImages ? 1 : 0, cfg.codeLineNumbers ? 1 : 0,
          cfg.linkConfirmWait ? 1 : 0);
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
  st->pagedSkin = true;
  st->pagedStartTick = ::GetTickCount64();
  PushChrome(st);  // 打印期间用设置里的打印皮肤，打完再恢复跟随
  if (!WebHostPost(st->hwndSelf, L"print|")) SetStatus(st, L"打印准备失败：页面不可达");
}

/* ---------------- 导出 PDF ----------------
   和打印共用「页面铺全文」那一段，区别只在于不弹打印对话框：由引擎直接把页面
   写成 PDF 文件，不经过任何打印机驱动，文字才保持可复制（走 Adobe PDF 一类
   虚拟打印机时，打印栈会把文本转成轮廓）。 */

// 默认文件名：源文件主干 + .pdf。
std::wstring PdfDefaultName(const wchar_t* path) {
  std::wstring leaf = UtilLeafName(path);
  const size_t dot = leaf.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0) leaf = leaf.substr(0, dot);
  if (leaf.empty()) return L"document.pdf";
  return leaf + L".pdf";
}

// 每份放多少块；0 = 不分段（整篇一份）。
// 标准保存对话框：默认源文件目录 + 同名 .pdf。取消或失败返回 false。
bool PickPdfTarget(ViewerState* st, std::wstring* out) {
  // 保存对话框要 COM STA。查看器窗口建在 DOpus 的 UI 线程上，那里通常已初始化，
  // 重复调用返回 S_FALSE——照样要 CoUninitialize，所以成败都要配对。
  struct ComGuard {
    const HRESULT hr;
    ComGuard() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComGuard() {
      if (SUCCEEDED(hr)) ::CoUninitialize();
    }
  } com;

  IFileSaveDialog* dlg = nullptr;
  const HRESULT hr =
      ::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&dlg));
  if (FAILED(hr) || !dlg) {
    LogLine(L"VIEWER: 保存对话框创建失败 hr=0x%08X", static_cast<unsigned>(hr));
    return false;
  }

  DWORD options = 0;
  dlg->GetOptions(&options);
  dlg->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
  const COMDLG_FILTERSPEC filter[] = {{L"PDF 文档", L"*.pdf"}};
  dlg->SetFileTypes(ARRAYSIZE(filter), filter);
  dlg->SetDefaultExtension(L"pdf");
  const std::wstring name = PdfDefaultName(st->szFile);
  dlg->SetFileName(name.c_str());

  const std::wstring dir = UtilParentDir(st->szFile);
  if (!dir.empty()) {
    IShellItem* folder = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
        folder) {
      dlg->SetFolder(folder);
      folder->Release();
    }
  }

  bool picked = false;
  HWND owner = ::GetAncestor(st->hwndSelf, GA_ROOT);
  if (!owner) owner = st->hwndSelf;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      LPWSTR psz = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
        *out = psz;
        ::CoTaskMemFree(psz);
        picked = true;
      }
      item->Release();
    }
  }
  dlg->Release();
  return picked;
}

// 分段的中间文件：<name>.part001.pdf …。全部写完再合并成用户要的那一份。
// 三位编号：上百段时文件名仍等宽，清理脚本按序号匹配不会漏。
std::wstring PartPath(const std::wstring& base, int seg, int segs) {
  if (segs <= 1) return base;
  wchar_t suffix[24];
  ::StringCchPrintfW(suffix, ARRAYSIZE(suffix), L".part%03d", seg + 1);
  const size_t dot = base.find_last_of(L'.');
  const size_t slash = base.find_last_of(L"\\/");
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
    return base + suffix + L".pdf";
  }
  return base.substr(0, dot) + suffix + base.substr(dot);
}

void EndExport(ViewerState* st, const wchar_t* status) {
  st->exporting = false;
  st->protoQueryPending = false;
  st->exportPath.clear();
  st->exportSeg = 0;
  st->exportSegs = 1;
  st->exportOk = 0;
  st->exportSegBlocks = 0;
  st->exportSegPages = 0;
  st->exportSegNodes = 0;
  st->exportPredMs = 0;
  st->exportRunStartTick = 0;
  st->exportSegBuiltBlocks = 0;
  st->exportDoneMs = 0;
  st->exportLastPct = -1;
  st->exportLastSeg = -1;
  st->exportLastTail[0] = L'\0';
  st->exportParts.clear();
  st->exportPlanPending = false;
  st->waitingPdf = false;
  st->lastStatusSec = 0;
  if (st->pagedSkin) {
    st->pagedSkin = false;
    PushChrome(st);
  }
  // 页面要自己清掉打印容器：宿主这边收尾与页面收尾是两条线
  if (st->hasHost && WebHostIsReady()) WebHostPost(st->hwndSelf, L"export-end|");
  if (status) SetStatus(st, L"%s", status);
}

void StartExport(ViewerState* st) {
  if (!st->docOpened) {
    SetStatus(st, L"没有可导出的文档");
    return;
  }
  if (!st->hasHost || st->webFailed || !WebHostIsReady()) {
    SetStatus(st, L"预览未就绪，无法导出");
    return;
  }
  if (st->exporting) {
    SetStatus(st, L"正在导出，请稍候…");
    return;
  }
  // 版本属于页面资源而不是窗口：每次导出前向当前页面现问一次。单宿主复用时
  // 窗口切换不会重新加载页面，按窗口缓存的版本会过期；缓存偶尔吐出旧 app.js
  // 也会让启动时那一次自报失真。
  if (st->protoQueryPending) {
    SetStatus(st, L"正在导出，请稍候…");
    return;
  }
  st->protoQueryPending = true;
  st->protoQueryTick = ::GetTickCount64();
  if (!WebHostPost(st->hwndSelf, L"proto-query|")) {
    st->protoQueryPending = false;
    SetStatus(st, L"导出准备失败：页面不可达");
  }
}

// 协议握手通过后才要计划：确认框、状态栏与文件名必须和页面算出的份数同源。
void RequestExportPlan(ViewerState* st) {
  st->exportPlanPending = true;
  st->planTick = ::GetTickCount64();
  if (!WebHostPost(st->hwndSelf, L"export-plan|")) {
    st->exportPlanPending = false;
    SetStatus(st, L"导出准备失败：页面不可达");
  }
}

// 计划到手：份数与粒度都按页面给的值走，不再自己算一遍。
void StartExportWithPlan(ViewerState* st, int segBlocks, int planSegs, bool interactive) {
  if (planSegs < 1) planSegs = 1;
  if (planSegs > kExportMaxSegs) {
    LogLine(L"VIEWER: 导出被拒绝，需要 %d 份，超过上限 %d", planSegs, kExportMaxSegs);
    SetStatus(st, L"文档太大：约需要 %d 份，超过上限 %d", planSegs, kExportMaxSegs);
    return;
  }
  // 两条对话框都是模态循环，期间 DOpus 的消息照常进来。先记一份存活令牌，
  // 窗口万一在这期间被销毁（换文件时重建窗格），返回后只能靠它判断还能不能碰 st。
  const std::shared_ptr<bool> alive = st->alive;
  bool proceed = true;
  st->dlgBusy = true;

  if (planSegs > 1 && interactive) {
    // 分段是分钟级操作，先把份数与「别切文件」讲清楚，免得中途才发现
    std::wstring base = PdfDefaultName(st->szFile);
    const size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    // 不给「几分钟」这种预告：同样的块数，密集小行的文档比普通正文慢一个数量级，
    // 提前算只能是猜；真正的数只能等页面铺完一份、量出页数才知道（见 export-ready）。
    wchar_t text[1024];
    ::StringCchPrintfW(
        text, ARRAYSIZE(text),
        L"文档约 %llu MB，会切成 %d 段生成，最后合并成一个 PDF：\n%s.pdf\n\n"
        L"生成期间请不要切换文件——切换会打断导出，只有已写出的内容保留。\n"
        L"实际耗时取决于版面，状态栏会按进度给出剩余时间。\n\n"
        L"继续导出？",
        static_cast<unsigned long long>(st->ullFileSize / (1024ull * 1024ull)), planSegs,
        base.c_str());
    if (::MessageBoxW(st->hwndSelf, text, kTitle, MB_ICONINFORMATION | MB_OKCANCEL) != IDOK) {
      LogLine(L"VIEWER: 分段导出取消（确认框）");
      proceed = false;
    }
  }

  std::wstring path;
  if (proceed) {
    if (interactive) {
      if (!PickPdfTarget(st, &path)) {
        LogLine(L"VIEWER: 导出取消（保存对话框）");
        proceed = false;
      }
    } else {
      path = st->testExportPath;
      st->testExportPath.clear();
      if (path.empty()) proceed = false;
    }
  }
  st->dlgBusy = false;

  // 对话框期间窗口没了：st 已释放，后面一个字都不能碰
  if (!alive || !*alive) {
    LogLine(L"VIEWER: 对话框返回时查看器已销毁，放弃导出");
    return;
  }
  // 对话框期间换了文档：用户的目标已经变了，不要拿旧文档继续导
  if (st->pendingLoad) {
    st->pendingLoad = false;
    LogLine(L"VIEWER: 对话框期间切换了文件，放弃导出");
    SetStatus(st, L"文档已切换，导出已取消");
    ::PostMessageW(st->hwndSelf, kMsgPendingLoad, 0, 0);
    return;
  }
  if (!proceed) return;

  st->exportPath = path;
  st->exporting = true;
  st->exportSeg = 0;
  st->exportSegs = planSegs;
  st->exportOk = 0;
  st->exportSegBlocks = segBlocks;
  st->exportSegPages = 0;
  st->exportSegNodes = 0;
  st->exportPredMs = PredictExportMs(0, 0, segBlocks);
  st->exportSegBuiltBlocks = 0;
  st->exportDoneMs = 0;
  st->exportLastPct = -1;
  st->exportLastSeg = -1;
  st->exportLastTail[0] = L'\0';
  st->pagedSkin = true;
  st->pagedStartTick = ::GetTickCount64();
  st->exportRunStartTick = st->pagedStartTick;
  LogLine(L"VIEWER: 导出 PDF 请求 -> %s（每份 %d 块，共 %d 份）", path.c_str(), segBlocks,
          planSegs);
  if (planSegs > 1) {
    SetStatus(st, L"正在准备导出（共 %d 段，期间不要切换文件）…", planSegs);
  } else {
    SetStatus(st, L"正在准备导出…");
  }
  PushChrome(st);

  wchar_t plan[64];
  ::StringCchPrintfW(plan, ARRAYSIZE(plan), L"export|%d|%d", segBlocks, planSegs);
  if (!WebHostPost(st->hwndSelf, plan)) EndExport(st, L"导出准备失败：页面不可达");
}

void PushIndex(ViewerState* st) {
  if (!st->docOpened || !st->hasHost || st->webFailed || !WebHostIsReady()) return;

  if (!WebHostSetDocFolder(st->hwndSelf, UtilParentDir(st->szFile))) {
    LogLine(L"VIEWER: mddoc.localhost 映射失败，相对图片不会加载");
  }

  const bool ok = WebHostPost(
      st->hwndSelf,
      msg::MakeIndex(kDocBase, st->prefixBlocks, st->estBlocks, st->ullFileSize, st->encoding,
                     st->lastVisible, st->docComplete, st->docKey, UtilProcessSessionId()));
  LogLine(L"VIEWER: 推送索引 %u/%u 块（末可见块 %u）-> %s", st->prefixBlocks, st->estBlocks,
          st->lastVisible, ok ? L"成功" : L"失败");
  // 网络图答复按文档键记，换一版就会重问：键写进日志，才看得出"为什么又问了"
  LogLine(L"VIEWER: 文档键 %s", st->docKey.c_str());
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

  // 换文档会重推索引，页面上的打印容器也就作废了：先让导出停手，
  // 否则写出去的是上一份内容的半截
  if (st->exporting) {
    LogLine(L"VIEWER: 导出途中切换文档，取消本次导出");
    EndExport(st, L"导出已取消：文档已切换");
  }

  // 换文档后旧图片 URL 的语义不再成立（mdblocks 按当前文档目录解析）
  st->imgCtxUrl.clear();
  st->imgCtxKind.clear();

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
  // 文件名与字节数 DOpus 原生状态栏本来就有，这里不再上报，免得为一条重复信息
  // 把整条原生分区顶掉。
  ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
}

// ---- 右键菜单：编码子菜单 ----
// 命令号从 0x6D00 起：菜单项的 uID 原样回传，不做偏移。
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

// ---- 右键菜单：代码块子菜单 ----
// 与设置窗口是同一份全局配置：菜单改完存盘并广播，所有窗口一起变。
// 枚举里存的是「序号」，命令号 = kCbMenuBase + 序号；switch 时两边才对得上。
constexpr UINT kCbMenuBase = 0x6E00;

enum CodeBlockMenuItem : UINT {
  kCbLineNumbers = 0,
  kCbHighlight,
  kCbCopyButton,
  kCbWsOff,
  kCbWsSelected,
  kCbWsBoundary,
  kCbWsAll,
  kCbMenuItemCount,
};

// ---- 右键菜单：图片动作区 ----
// 页面右键图片先发 imgctx 帧记下上下文；本地图还原出磁盘路径才有「打开方式」，
// 远程图只给「放大查看」（插件不把网络图悄悄落盘）。
constexpr UINT kImgMenuBase = 0x6F00;

enum ImageMenuItem : UINT {
  kImgZoom = 0,       // 放大查看：zoom| 发回页面开遮罩
  kImgOpenInDOpus,     // 在 DOpus 查看器中打开：dopusrt /show（只有本地图才有）
  kImgOpenWithOther,  // 选择其他应用…：系统的 OpenWith 对话框（可勾「始终」）
  kImgHandlerFirst,   // 子菜单里系统枚举出的处理器从这里起号
};
constexpr UINT kImgHandlerMax = 32;  // 枚举上限：推荐处理器一屏绰绰有余

// 右键菜单改全局设置：存盘 + 通知所有查看器重推配置。
void ApplyCodeBlockMenu(UINT cmd) {
  Config cfg = ConfigSnapshot();
  switch (cmd - kCbMenuBase) {
    case kCbLineNumbers:
      cfg.codeLineNumbers = !cfg.codeLineNumbers;
      break;
    case kCbHighlight:
      cfg.renderHighlight = !cfg.renderHighlight;
      break;
    case kCbCopyButton:
      cfg.copyCodeBlock = !cfg.copyCodeBlock;
      break;
    case kCbWsOff:
      cfg.codeWsOn = false;
      break;
    case kCbWsSelected:
      cfg.codeWsOn = true;
      cfg.codeWsRange = 1;
      break;
    case kCbWsBoundary:
      cfg.codeWsOn = true;
      cfg.codeWsRange = 2;
      break;
    case kCbWsAll:
      cfg.codeWsOn = true;
      cfg.codeWsRange = 3;
      break;
    default:
      return;
  }
  ConfigSaveFailure fail{};
  if (!ConfigSave(cfg, &fail)) {
    LogLine(L"VIEWER: 右键菜单改设置写盘失败 err=%lu", fail.code);
    return;
  }
  LogLine(L"VIEWER: 右键菜单改设置 id=%u", cmd);
  ViewerNotifyConfigChanged();
}

// 「打开方式」短列表的白名单：OpenWithProgids（HKCR 安装注册 + HKCU 用户历史）
// 里登记的 ProgID 解析出的友好应用名。资源管理器/DOpus 的打开方式子菜单就是这个
// 数据源；无过滤枚举比它多出一片「未知类型兜底」噪音（给任何文件都能塞的那批
// 记事本/VMware/媒体播放器），必须按这个名字集挡掉。解析不出友好名的 ProgID
// （如空壳 pngfile）跳过。
void CollectOpenWithProgids(const std::wstring& ext, std::vector<std::wstring>* names) {
  const struct {
    HKEY root;
    std::wstring path;
  } sources[2] = {
      {HKEY_CURRENT_USER,
       L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext +
           L"\\OpenWithProgids"},
      {HKEY_CLASSES_ROOT, ext + L"\\OpenWithProgids"},
  };
  for (const auto& src : sources) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(src.root, src.path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
      continue;
    }
    for (DWORD i = 0;; ++i) {
      wchar_t progid[256];
      DWORD cch = 256;
      if (::RegEnumValueW(key, i, progid, &cch, nullptr, nullptr, nullptr, nullptr) !=
          ERROR_SUCCESS) {
        break;
      }
      wchar_t friendly[256];
      DWORD cchFriendly = 256;
      if (FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, progid, L"open",
                                     friendly, &cchFriendly)) ||
          cchFriendly == 0) {
        continue;
      }
      const std::wstring name(friendly, cchFriendly - 1);
      bool dup = false;
      for (const std::wstring& have : *names) dup = dup || have == name;
      if (!dup) names->push_back(name);
    }
    ::RegCloseKey(key);
  }
}

// 类型默认程序的 exe 文件名。资源管理器的打开方式 flyout 永远包含默认程序，但它
// 可能既不在推荐档、也没进 OpenWithProgids。默认是 UWP 包时解析不出（探针证实
// ASSOCSTR_EXECUTABLE 对 AppX ProgID 失败），正好——那些由推荐档负责。
std::wstring DefaultProgramExeLeaf(const std::wstring& ext) {
  wchar_t exe[MAX_PATH * 2] = {};
  DWORD cch = ARRAYSIZE(exe);
  if (FAILED(::AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, ext.c_str(),
                                 nullptr, exe, &cch)) ||
      !exe[0]) {
    return std::wstring();
  }
  const std::wstring full(exe);
  const size_t slash = full.find_last_of(L"\\/");
  const std::wstring leaf = slash == std::wstring::npos ? full : full.substr(slash + 1);
  return _wcsicmp(leaf.c_str(), L"openwith.exe") == 0 ? std::wstring() : leaf;
}

// ---- 右键菜单：「打开方式」的程序图标 ----
// 图标来源与 Pulse 一致：IAssocHandler::GetIconLocation 给出图标引用，UWP 应用是
// @{包名?ms-resource://…}，用 SHLoadIndirectString 解析成磁盘上的 AppList PNG；
// 普通程序是「exe,index」，走 SHDefExtractIconW；都解析不出来再拿 exe 的第一个
// 图标兜底。位图做成 32bpp 预乘 alpha——原生菜单的 MIIM_BITMAP 会按 alpha 叠在
// 菜单背景上（探针证实），不能直接搬 shell「打开方式」菜单里的位图：那是强调色
// 实心底的贴图，在菜单里就是一块蓝方块。
// DOpus 自己的菜单图标取自它的图标集（context_menu_icon_set，默认 Windows 11
// Default Icons 的 small 档），该档按 DPI 分块换位图：100%=22、126–175%=32、
// 200%=48、300%=64（.dis 里 <dpi><scale> 的表）。自绘菜单按同一组分块取尺寸，
// 与 DOpus 菜单的观感一致；16 DIP 线性放大在 175% 只有 28px，比 DOpus 小一档。

struct MenuIcons {
  IWICImagingFactory* wic = nullptr;
  std::vector<HBITMAP> owned;  // 菜单销毁后统一 DeleteObject，位图不是菜单的

  bool Init() {
    return SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&wic)));
  }
  HBITMAP Add(HBITMAP bmp) {
    if (bmp) owned.push_back(bmp);
    return bmp;
  }
  ~MenuIcons() {
    for (HBITMAP bmp : owned) ::DeleteObject(bmp);
    if (wic) wic->Release();
  }
};

UINT MenuIconEdge(HWND hwnd) {
  UINT dpi = hwnd ? ::GetDpiForWindow(hwnd) : 96;
  if (!dpi) dpi = 96;
  if (dpi <= 120) return 22;   // 100–125%
  if (dpi <= 168) return 32;   // 126–175%
  if (dpi <= 240) return 48;   // 176–250%
  return 64;                   // 251%+
}

HBITMAP BitmapFromBgra(MenuIcons* icons, UINT w, UINT h, const std::vector<uint8_t>& bgra) {
  if (!w || !h || bgra.size() < static_cast<size_t>(w) * h * 4u) return nullptr;
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = static_cast<LONG>(w);
  bi.bmiHeader.biHeight = -static_cast<LONG>(h);
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HBITMAP bmp =
      screen ? ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (screen) ::ReleaseDC(nullptr, screen);
  if (!bmp || !bits) {
    if (bmp) ::DeleteObject(bmp);
    return nullptr;
  }
  memcpy(bits, bgra.data(), static_cast<size_t>(w) * h * 4u);
  return icons->Add(bmp);
}

// WIC 源统一转成菜单要的预乘 BGRA。
HBITMAP BitmapFromWicSource(MenuIcons* icons, IWICBitmapSource* src) {
  if (!icons->wic || !src) return nullptr;
  IWICFormatConverter* conv = nullptr;
  if (FAILED(icons->wic->CreateFormatConverter(&conv)) || !conv) return nullptr;
  HBITMAP bmp = nullptr;
  if (SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                 nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
    UINT w = 0, h = 0;
    if (SUCCEEDED(conv->GetSize(&w, &h)) && w && h) {
      std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4u);
      if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(px.size()), px.data())))
        bmp = BitmapFromBgra(icons, w, h, px);
    }
  }
  conv->Release();
  return bmp;
}

HBITMAP BitmapFromHicon(MenuIcons* icons, HICON icon) {
  if (!icons->wic || !icon) return nullptr;
  IWICBitmap* wicBmp = nullptr;
  HBITMAP bmp = nullptr;
  if (SUCCEEDED(icons->wic->CreateBitmapFromHICON(icon, &wicBmp)) && wicBmp)
    bmp = BitmapFromWicSource(icons, wicBmp);
  if (wicBmp) wicBmp->Release();
  return bmp;
}

// UWP 的引用解析出来是磁盘上的 PNG（AppList 图）。这张图自带一圈透明留白：
// 66px 画布里图形实际只有 42–48px，整张画布缩到 edge 后图形只剩六成多，菜单里
// 就比 DOpus 显示的同款图标小一圈。先按 alpha 量出图形范围、裁掉留白，再把图形
// 等比缩进 edge×edge 并居中放回透明画布——视觉尺寸才和 DOpus 菜单一致。
HBITMAP BitmapFromImageFile(MenuIcons* icons, const std::wstring& file, UINT edge) {
  if (!icons->wic) return nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  HBITMAP bmp = nullptr;
  if (SUCCEEDED(icons->wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &dec)) &&
      dec && SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
      SUCCEEDED(icons->wic->CreateFormatConverter(&conv)) && conv &&
      SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                 nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
    UINT sw = 0, sh = 0;
    conv->GetSize(&sw, &sh);
    std::vector<uint8_t> px(static_cast<size_t>(sw) * sh * 4u);
    if (sw && sh &&
        SUCCEEDED(conv->CopyPixels(nullptr, sw * 4u, static_cast<UINT>(px.size()), px.data()))) {
      // 图形范围（alpha 非零的外接框）
      UINT l = sw, t = sh, r = 0, b = 0;
      for (UINT y = 0; y < sh; ++y) {
        for (UINT x = 0; x < sw; ++x) {
          if (!px[(static_cast<size_t>(y) * sw + x) * 4u + 3u]) continue;
          l = (std::min)(l, x);
          t = (std::min)(t, y);
          r = (std::max)(r, x + 1);
          b = (std::max)(b, y + 1);
        }
      }
      if (r > l && b > t) {
        const UINT aw = r - l, ah = b - t;
        const float fit =
            (std::min)(static_cast<float>(edge) / aw, static_cast<float>(edge) / ah);
        const UINT tw = (std::max)(1u, static_cast<UINT>(aw * fit + 0.5f));
        const UINT th = (std::max)(1u, static_cast<UINT>(ah * fit + 0.5f));
        std::vector<uint8_t> crop(static_cast<size_t>(aw) * ah * 4u);
        for (UINT y = 0; y < ah; ++y) {
          memcpy(&crop[static_cast<size_t>(y) * aw * 4u],
                 &px[(static_cast<size_t>(t + y) * sw + l) * 4u], static_cast<size_t>(aw) * 4u);
        }
        IWICBitmap* cropped = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        if (SUCCEEDED(icons->wic->CreateBitmapFromMemory(
                aw, ah, GUID_WICPixelFormat32bppPBGRA, aw * 4u,
                static_cast<UINT>(crop.size()), crop.data(), &cropped)) &&
            cropped && SUCCEEDED(icons->wic->CreateBitmapScaler(&scaler)) && scaler &&
            SUCCEEDED(scaler->Initialize(cropped, tw, th,
                                         WICBitmapInterpolationModeHighQualityCubic))) {
          std::vector<uint8_t> scaled(static_cast<size_t>(tw) * th * 4u);
          if (SUCCEEDED(scaler->CopyPixels(nullptr, tw * 4u, static_cast<UINT>(scaled.size()),
                                           scaled.data()))) {
            std::vector<uint8_t> canvas(static_cast<size_t>(edge) * edge * 4u, 0);
            const UINT ox = (edge - tw) / 2, oy = (edge - th) / 2;
            for (UINT y = 0; y < th; ++y) {
              memcpy(&canvas[(static_cast<size_t>(oy + y) * edge + ox) * 4u],
                     &scaled[static_cast<size_t>(y) * tw * 4u], static_cast<size_t>(tw) * 4u);
            }
            bmp = BitmapFromBgra(icons, edge, edge, canvas);
          }
        }
        if (scaler) scaler->Release();
        if (cropped) cropped->Release();
      }
    }
  }
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (dec) dec->Release();
  return bmp;
}

HBITMAP BitmapFromModule(MenuIcons* icons, const std::wstring& file, int index, UINT edge) {
  HICON icon = nullptr;
  if (FAILED(::SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr, MAKELONG(edge, edge))) ||
      !icon)
    return nullptr;
  HBITMAP bmp = BitmapFromHicon(icons, icon);
  ::DestroyIcon(icon);
  return bmp;
}

// 图标引用三种形态：@{包?ms-resource://…}（解析成 PNG）、带环境变量的 exe 路径、
// 「路径,索引」。PathParseIconLocationW 的索引优先，没有就用 GetIconLocation 另给的。
HBITMAP BitmapFromIconLocation(MenuIcons* icons, const std::wstring& reference, int index,
                               UINT edge) {
  if (reference.empty()) return nullptr;
  if (reference[0] == L'@') {
    wchar_t file[MAX_PATH * 4] = {};
    if (SUCCEEDED(::SHLoadIndirectString(reference.c_str(), file, ARRAYSIZE(file), nullptr)) &&
        file[0]) {
      if (HBITMAP bmp = BitmapFromImageFile(icons, file, edge)) return bmp;
    }
    return nullptr;
  }
  wchar_t buffer[MAX_PATH * 4] = {};
  if (wcsncpy_s(buffer, reference.c_str(), _TRUNCATE) != 0) return nullptr;
  const int refIndex = ::PathParseIconLocationW(buffer);
  ::PathUnquoteSpacesW(buffer);
  wchar_t expanded[MAX_PATH * 4] = {};
  const DWORD n = ::ExpandEnvironmentStringsW(buffer, expanded, ARRAYSIZE(expanded));
  const std::wstring file = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : buffer;
  if (file.empty()) return nullptr;
  return BitmapFromModule(icons, file, refIndex != 0 ? refIndex : index, edge);
}

HBITMAP LoadHandlerIcon(MenuIcons* icons, IAssocHandler* handler, UINT edge) {
  PWSTR loc = nullptr;
  int index = 0;
  if (SUCCEEDED(handler->GetIconLocation(&loc, &index)) && loc) {
    const std::wstring ref = loc;
    ::CoTaskMemFree(loc);
    if (HBITMAP bmp = BitmapFromIconLocation(icons, ref, index, edge)) return bmp;
  }
  PWSTR name = nullptr;
  if (SUCCEEDED(handler->GetName(&name)) && name) {
    const std::wstring exe = name;
    ::CoTaskMemFree(name);
    if (HBITMAP bmp = BitmapFromModule(icons, exe, 0, edge)) return bmp;
  }
  return nullptr;
}

void SetMenuIcon(HMENU menu, UINT pos, HBITMAP icon) {
  if (!icon) return;
  MENUITEMINFOW set = {sizeof(set)};
  set.fMask = MIIM_BITMAP;
  set.hbmpItem = icon;
  ::SetMenuItemInfoW(menu, pos, TRUE, &set);
}

// 「打开方式」子菜单：推荐档（系统推荐，画图/照片这些 UWP 也在其中）全收；
// 无过滤档按 OpenWithProgids 白名单过滤后补齐其余注册程序（用户用过的编辑器），
// 类型默认程序的 exe 也放行（flyout 永远含默认程序），按显示名去重。
// 显示名用 GetUIName（友好名）；GetName 是 exe 路径，只配兜底。
// SHAssocEnumHandlers 只收扩展名（".png"）：传完整路径它会把整条路径当「扩展名」，
// 推荐档为空、无过滤档只剩未知类型的兜底列表（经白名单已挡）。
// 每项顺手按 GetIconLocation 取图标（UWP 走 SHLoadIndirectString 的 PNG），
// 图标位图由 icons 统一持有到菜单关闭之后。
// 同步枚举——条目量小，弹菜单前完成没有可感知延迟。命令号 = kImgMenuBase +
// kImgHandlerFirst + 序号，handler 留在 handlers 里活到菜单关闭之后（Invoke 用）。
void CollectOpenWithHandlers(const std::wstring& ext, HMENU ow,
                             std::vector<IAssocHandler*>* handlers, MenuIcons* icons,
                             UINT iconEdge) {
  std::vector<std::wstring> allow;
  CollectOpenWithProgids(ext, &allow);
  const std::wstring defaultLeaf = DefaultProgramExeLeaf(ext);

  for (const ASSOC_FILTER filter : {ASSOC_FILTER_RECOMMENDED, ASSOC_FILTER_NONE}) {
    IEnumAssocHandlers* en = nullptr;
    const HRESULT hr = ::SHAssocEnumHandlers(ext.c_str(), filter, &en);
    if (FAILED(hr) || !en) {
      LogLine(L"VIEWER: 打开方式枚举失败 ext=%s filter=%d hr=0x%08lX", ext.c_str(),
              static_cast<int>(filter), static_cast<unsigned long>(hr));
      continue;
    }
    ULONG fetched = 0;
    for (;;) {
      if (handlers->size() >= kImgHandlerMax) break;
      IAssocHandler* h = nullptr;
      fetched = 0;
      if (en->Next(1, &h, &fetched) != S_OK || fetched == 0 || !h) break;
      PWSTR raw = nullptr;
      if (FAILED(h->GetUIName(&raw)) || !raw) h->GetName(&raw);
      if (!raw) {
        h->Release();  // 两个名字都拿不到的处理器不进菜单
        continue;
      }
      const std::wstring label = raw;
      ::CoTaskMemFree(raw);
      // 无过滤档里不在白名单、也不是默认程序的，都是「任何文件都给」的兜底项，挡掉
      if (filter == ASSOC_FILTER_NONE && !allow.empty()) {
        bool ok = false;
        for (const std::wstring& name : allow) ok = ok || name == label;
        if (!ok && !defaultLeaf.empty()) {
          PWSTR exeRaw = nullptr;
          if (SUCCEEDED(h->GetName(&exeRaw)) && exeRaw) {
            const std::wstring exe = exeRaw;
            ::CoTaskMemFree(exeRaw);
            const size_t slash = exe.find_last_of(L"\\/");
            const std::wstring leaf = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
            ok = !leaf.empty() && _wcsicmp(leaf.c_str(), defaultLeaf.c_str()) == 0;
          }
        }
        if (!ok) {
          h->Release();
          continue;
        }
      }
      // 无过滤档会重复推荐档的程序、同一程序也可能有多个 ProgID，按显示名去重
      bool dup = false;
      for (IAssocHandler* prev : *handlers) {
        PWSTR prevName = nullptr;
        if (SUCCEEDED(prev->GetUIName(&prevName)) && prevName) {
          dup = label == prevName;
          ::CoTaskMemFree(prevName);
          if (dup) break;
        }
      }
      if (dup) {
        h->Release();
        continue;
      }
      ::AppendMenuW(ow, MF_STRING,
                    kImgMenuBase + kImgHandlerFirst + static_cast<UINT>(handlers->size()),
                    label.c_str());
      SetMenuIcon(ow, static_cast<UINT>(::GetMenuItemCount(ow)) - 1,
                  LoadHandlerIcon(icons, h, iconEdge));
      handlers->push_back(h);
    }
    en->Release();
  }
  std::wstring menuLabels;
  for (IAssocHandler* h : *handlers) {
    PWSTR ui = nullptr;
    if (SUCCEEDED(h->GetUIName(&ui)) && ui) {
      if (!menuLabels.empty()) menuLabels += L" | ";
      menuLabels += ui;
      ::CoTaskMemFree(ui);
    }
  }
  LogLine(L"VIEWER: 打开方式 ext=%s 白名单 %u 项，菜单 %u 项 [%s]", ext.c_str(),
          static_cast<unsigned>(allow.size()), static_cast<unsigned>(handlers->size()),
          menuLabels.c_str());
}

// ---- 「打开方式」的执行 ----
// 有序的三条路，前一条成功就停：
// 1. 资源管理器自己那条「打开方式」菜单项：向文件本身的 IContextMenu 要 verb 为
//    openas 的子菜单，按显示名对上同一行后 InvokeCommand。
// 2. 经典程序（GetName 是现成的 exe）：ShellExecuteEx 直接执行 exe + 文件参数，
//    与注册的 open 命令同形。
// 3. IAssocHandler::Invoke（文档里填充 Open With 菜单、单选项的调用）兜底。
//
// 为什么不能只用 3：个别机器上 handler->Invoke 返回 S_OK 却只弹出系统的「你要用
// 什么方式打开这个文件」选择框（实测照片与 XnView 都这样，同一时刻资源管理器
// 和 DOpus 自己的打开方式都能直接启动程序），1/2 是绕过这条判断的直连路径。
// 3 的数据对象必须取 shell 为这个文件建的那份（IShellItem 的 BHID_DataObject）：
// SHCreateDataObject 拿完整 pidl 当「桌面的子项」传进去只会得到空对象（CF_HDROP /
// ShellIDList / ShellItemArray 全不支持）——经典程序 Invoke 返回 0x80070483、
// 静默不开，UWP 激活（照片）会在 windows.storage 里踩空崩溃。

// shell 菜单项动词的编号区间（只在取 openas 子菜单时用一次）。
constexpr UINT kShellVerbFirst = 0x7100;
constexpr UINT kShellVerbLast = 0x71FF;

// 走资源管理器同款菜单项。openas 子菜单是 shell 惰性填的，不先把
// WM_INITMENUPOPUP 喂给 IContextMenu3 就是空的。
BOOL InvokeShellOpenAsItem(const wchar_t* path, const wchar_t* label, HWND owner) {
  PIDLIST_ABSOLUTE pidl = nullptr;
  if (FAILED(::SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) || !pidl) return FALSE;
  IShellFolder* folder = nullptr;
  PCUITEMID_CHILD child = nullptr;
  IContextMenu* cm = nullptr;
  if (SUCCEEDED(::SHBindToParent(pidl, IID_PPV_ARGS(&folder), &child)) && folder) {
    folder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr,
                          reinterpret_cast<void**>(&cm));
    folder->Release();
  }
  ::ILFree(pidl);
  if (!cm) return FALSE;

  IContextMenu3* cm3 = nullptr;
  cm->QueryInterface(IID_PPV_ARGS(&cm3));
  const HMENU root = ::CreatePopupMenu();
  BOOL done = FALSE;
  if (root &&
      SUCCEEDED(cm->QueryContextMenu(root, 0, kShellVerbFirst, kShellVerbLast, CMF_NORMAL))) {
    HMENU sub = nullptr;
    UINT subPos = 0;
    for (UINT i = 0; i < static_cast<UINT>(::GetMenuItemCount(root)); ++i) {
      MENUITEMINFOW mii = {sizeof(mii)};
      mii.fMask = MIIM_SUBMENU | MIIM_ID;
      if (!::GetMenuItemInfoW(root, i, TRUE, &mii) || !mii.hSubMenu) continue;
      wchar_t verb[64] = {};
      // GCS_VERBW 按 Unicode 往调用方给的缓冲里写，形参类型还是 LPSTR。
      if (SUCCEEDED(cm->GetCommandString(mii.wID - kShellVerbFirst, GCS_VERBW, nullptr,
                                        reinterpret_cast<LPSTR>(verb), 63)) &&
          _wcsicmp(verb, L"openas") == 0) {
        sub = mii.hSubMenu;
        subPos = i;
        break;
      }
    }
    if (sub) {
      if (cm3) {
        LRESULT ignored = 0;
        cm3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub),
                            MAKELPARAM(subPos, FALSE), &ignored);
      }
      const int count = ::GetMenuItemCount(sub);
      for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii = {sizeof(mii)};
        wchar_t text[256] = {};
        mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = 255;
        if (!::GetMenuItemInfoW(sub, static_cast<UINT>(i), TRUE, &mii)) continue;
        if ((mii.fType & (MFT_SEPARATOR | MFT_OWNERDRAW)) || mii.hSubMenu) continue;
        if (_wcsicmp(text, label) != 0) continue;
        CMINVOKECOMMANDINFOEX info = {};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_UNICODE;
        info.hwnd = owner;
        info.lpVerb = MAKEINTRESOURCEA(mii.wID - kShellVerbFirst);
        info.lpVerbW = MAKEINTRESOURCEW(mii.wID - kShellVerbFirst);
        info.nShow = SW_SHOWNORMAL;
        done = SUCCEEDED(cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info)));
        break;
      }
    }
  }
  if (root) ::DestroyMenu(root);
  if (cm3) cm3->Release();
  cm->Release();
  return done;
}

bool LaunchExeWithFile(const std::wstring& exe, const std::wstring& path, DWORD* errOut) {
  std::wstring args = L"\"" + path + L"\"";
  SHELLEXECUTEINFOW sei = {sizeof(sei)};
  sei.lpFile = exe.c_str();
  sei.lpParameters = args.c_str();
  sei.nShow = SW_SHOWNORMAL;
  const BOOL ok = ::ShellExecuteExW(&sei);
  *errOut = ok ? ERROR_SUCCESS : ::GetLastError();
  return ok != FALSE;
}

void InvokeOpenWithHandler(IAssocHandler* handler, const std::wstring& path, HWND owner) {
  PWSTR ui = nullptr;
  std::wstring label;
  if (SUCCEEDED(handler->GetUIName(&ui)) && ui) {
    label = ui;
    ::CoTaskMemFree(ui);
  }
  PWSTR rawName = nullptr;
  std::wstring name;
  if (SUCCEEDED(handler->GetName(&rawName)) && rawName) {
    name = rawName;
    ::CoTaskMemFree(rawName);
  }

  // 1. 资源管理器同款菜单项
  if (!label.empty()) {
    const BOOL ok = InvokeShellOpenAsItem(path.c_str(), label.c_str(), owner);
    LogLine(L"VIEWER: 打开方式执行 \"%s\" path=\"%s\" shell菜单=%d", label.c_str(), path.c_str(),
            static_cast<int>(ok));
    if (ok) return;
  }

  // 2. 经典程序：直接执行 exe
  if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0 &&
      ::GetFileAttributesW(name.c_str()) != INVALID_FILE_ATTRIBUTES) {
    DWORD err = ERROR_SUCCESS;
    const bool ok = LaunchExeWithFile(name, path, &err);
    LogLine(L"VIEWER: 打开方式执行 \"%s\" path=\"%s\" exe=\"%s\" shellExecute=%d err=%lu",
            label.c_str(), path.c_str(), name.c_str(), ok ? 1 : 0, err);
    if (ok) return;
  }

  // 3. 兜底：handler->Invoke
  IShellItem* item = nullptr;
  IDataObject* pdo = nullptr;
  HRESULT hrItem = E_FAIL;
  HRESULT hrBind = E_FAIL;
  if (SUCCEEDED(hrItem = ::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                        IID_PPV_ARGS(&item))) &&
      item) {
    hrBind = item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&pdo));
    item->Release();
  }
  if (!pdo) {
    LogLine(L"VIEWER: 打开方式兜底取不到数据对象 path=\"%s\" item=0x%08lX bind=0x%08lX",
            path.c_str(), static_cast<unsigned long>(hrItem),
            static_cast<unsigned long>(hrBind));
    return;
  }
  const HRESULT hr = handler->Invoke(pdo);
  LogLine(L"VIEWER: 打开方式执行 \"%s\" path=\"%s\" name=\"%s\" item=0x%08lX "
          L"bind=0x%08lX pdo=%d invoke=0x%08lX",
          label.c_str(), path.c_str(), name.c_str(), static_cast<unsigned long>(hrItem),
          static_cast<unsigned long>(hrBind), pdo ? 1 : 0, static_cast<unsigned long>(hr));
  if (pdo) pdo->Release();
}

// 独立查看器不会把插件条目并进 DOpus 自己的菜单（预览窗格会），这里自绘一个。
constexpr UINT kCtxCopy = 0x6D80;
constexpr UINT kCtxSelectAll = 0x6D81;
constexpr UINT kCtxPrint = 0x6D82;
constexpr UINT kCtxReload = 0x6D83;
constexpr UINT kCtxExportPdf = 0x6D84;

// 右键菜单：预览窗格与独立查看器用同一份自绘菜单。
// 自绘的命令号由 TrackPopupMenu 直接带回，不会像 DOpus 自己的菜单那样丢掉
// （那条路要靠 DOpus 把命令转回来，而焦点在 WebView2 子窗口时它认为查看器没焦点）。
void ShowOwnContextMenu(ViewerState* st) {
  // 枚举器与 OpenWith 对话框要求线程进 COM STA：查看器窗口建在 DOpus 的 UI 线程上，
  // 那里通常已初始化，重复调用返回 S_FALSE（照样要成对 CoUninitialize）。
  struct ComGuard {
    const HRESULT hr;
    ComGuard() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComGuard() {
      if (SUCCEEDED(hr)) ::CoUninitialize();
    }
  } com;

  HMENU menu = ::CreatePopupMenu();
  HMENU enc = ::CreatePopupMenu();
  HMENU cb = ::CreatePopupMenu();
  HMENU ws = ::CreatePopupMenu();
  if (!menu || !enc || !cb || !ws) {
    if (menu) ::DestroyMenu(menu);
    if (enc) ::DestroyMenu(enc);
    if (cb) ::DestroyMenu(cb);
    if (ws) ::DestroyMenu(ws);
    return;
  }

  // 图片动作区（只有 imgctx 帧才给）：插在最上面，分隔线隔开全局项。
  // 本地图与资产读盘同一套路径校验；路径非法或文件没了就只出原有全局菜单。
  const bool imgRemote = st->imgCtxKind == L"remote";
  std::wstring imgPath;
  const bool imgLocal =
      st->imgCtxKind == L"local" && docsrc::AssetUrlToPath(st->imgCtxUrl, &imgPath);
  std::vector<IAssocHandler*> imgHandlers;
  MenuIcons menuIcons;  // 菜单图标位图归它，菜单关闭并 Invoke 完再释放
  struct HandlerReleaser {  // 菜单选中后还要 Invoke，统一在函数出口释放
    std::vector<IAssocHandler*>* v;
    ~HandlerReleaser() {
      for (IAssocHandler* h : *v) h->Release();
    }
  } imgRelease{&imgHandlers};
  if (imgRemote || imgLocal) {
    ::AppendMenuW(menu, MF_STRING, kImgMenuBase + kImgZoom, L"放大查看");
    // DOpus 自己的查看器：只对本地图给（远程图没有磁盘路径）。dopusrt 找不到
    // 就不出这一项，免得点了没反应。
    if (imgLocal && !UtilDOpusViewerExe().empty()) {
      ::AppendMenuW(menu, MF_STRING, kImgMenuBase + kImgOpenInDOpus, L"在 DOpus 查看器中打开");
    }
    if (imgLocal) {
      // 枚举器只认扩展名：从磁盘路径取「.png」这种（目录名带点不受影响，取的是
      // 最后一段里的点）；没有扩展名的文件无从注册处理器，不建这个子菜单
      const size_t dot = imgPath.find_last_of(L".\\/");
      const std::wstring ext =
          (dot != std::wstring::npos && imgPath[dot] == L'.' && dot + 1 < imgPath.size())
              ? imgPath.substr(dot)
              : std::wstring();
      HMENU ow = ext.empty() ? nullptr : ::CreatePopupMenu();
      if (ow) {
        const UINT iconEdge = MenuIconEdge(st->hwndSelf);
        menuIcons.Init();  // WIC 建不成就只出文字，菜单照常
        CollectOpenWithHandlers(ext, ow, &imgHandlers, &menuIcons, iconEdge);
        if (imgHandlers.empty()) {
          // 扩展名没注册任何推荐处理器：留一条灰项，子菜单不至于只有兜底
          ::AppendMenuW(ow, MF_STRING | MF_GRAYED, 0, L"（没有已注册的程序）");
        }
        ::AppendMenuW(ow, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(ow, MF_STRING, kImgMenuBase + kImgOpenWithOther, L"选择其他应用…");
        // 「选择其他应用…」在 Explorer 的 flyout 里也没有图标，保持一致
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(ow), L"打开方式");
      }
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }

  for (size_t i = 0; i < kEncMenuCount; ++i) {
    UINT flags = MF_STRING;
    if (kEncMenu[i].codepage == st->encOverride) flags |= MF_CHECKED;
    ::AppendMenuW(enc, flags, kEncMenuBase + static_cast<UINT>(i), kEncMenu[i].label);
  }
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(enc), L"编码");

  const Config cfg = ConfigSnapshot();
  ::AppendMenuW(cb, MF_STRING | (cfg.codeLineNumbers ? MF_CHECKED : 0), kCbMenuBase + kCbLineNumbers,
               L"行号");
  ::AppendMenuW(cb, MF_STRING | (cfg.renderHighlight ? MF_CHECKED : 0), kCbMenuBase + kCbHighlight,
               L"语法高亮");
  ::AppendMenuW(cb, MF_STRING | (cfg.copyCodeBlock ? MF_CHECKED : 0), kCbMenuBase + kCbCopyButton,
               L"代码块复制按钮");
  // 空白符各档单独开一层子菜单：单选组在一层里看不出互斥，分一层就一目了然。
  // 顺序按常显的多少递增，和设置页的单选、配置值三处对齐。
  ::AppendMenuW(ws, MF_STRING, kCbMenuBase + kCbWsOff, L"关闭");
  ::AppendMenuW(ws, MF_STRING, kCbMenuBase + kCbWsSelected, L"仅选中");
  ::AppendMenuW(ws, MF_STRING, kCbMenuBase + kCbWsBoundary, L"边界");
  ::AppendMenuW(ws, MF_STRING, kCbMenuBase + kCbWsAll, L"全部");
  UINT wsId = kCbMenuBase + kCbWsBoundary;
  if (!cfg.codeWsOn) wsId = kCbMenuBase + kCbWsOff;
  else if (cfg.codeWsRange == 1) wsId = kCbMenuBase + kCbWsSelected;
  else if (cfg.codeWsRange == 3) wsId = kCbMenuBase + kCbWsAll;
  ::CheckMenuRadioItem(ws, kCbMenuBase + kCbWsOff, kCbMenuBase + kCbWsAll, wsId,
                       MF_BYCOMMAND);
  ::AppendMenuW(cb, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(cb, MF_POPUP, reinterpret_cast<UINT_PTR>(ws), L"空白符范围");
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(cb), L"代码块");

  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  const UINT docFlags = st->docOpened ? 0 : MF_GRAYED;
  ::AppendMenuW(menu, MF_STRING | docFlags, kCtxCopy, L"复制");
  ::AppendMenuW(menu, MF_STRING | docFlags, kCtxSelectAll, L"全选");
  ::AppendMenuW(menu, MF_STRING | docFlags, kCtxPrint, L"打印…");
  ::AppendMenuW(menu, MF_STRING | docFlags, kCtxExportPdf, L"导出 PDF…");
  ::AppendMenuW(menu, MF_STRING | docFlags, kCtxReload, L"刷新");

  POINT pt = {};
  ::GetCursorPos(&pt);
  const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, st->hwndSelf, nullptr);
  ::DestroyMenu(menu);
  if (!cmd) return;

  if (cmd == kImgMenuBase + kImgZoom) {
    if (st->hasHost && WebHostIsReady()) WebHostPost(st->hwndSelf, L"zoom|");
    return;
  }
  if (cmd == kImgMenuBase + kImgOpenInDOpus) {
    if (!UtilDOpusViewerShow(imgPath)) {
      LogLine(L"VIEWER: DOpus 查看器打不开图片 %s", imgPath.c_str());
      SetStatus(st, L"打不开：DOpus 查看器不可用");
    }
    return;
  }
  if (cmd == kImgMenuBase + kImgOpenWithOther) {
    // 系统的 OpenWith 对话框：可勾「始终」，勾了写回该扩展名的默认程序
    OPENASINFO oi = {};
    oi.pcszFile = imgPath.c_str();
    oi.pcszClass = nullptr;
    oi.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
    ::SHOpenWithDialog(st->hwndSelf, &oi);
    return;
  }
  if (cmd >= kImgMenuBase + kImgHandlerFirst &&
      cmd < kImgMenuBase + kImgHandlerFirst + static_cast<UINT>(imgHandlers.size())) {
    InvokeOpenWithHandler(imgHandlers[cmd - kImgMenuBase - kImgHandlerFirst], imgPath,
                          st->hwndSelf);
    return;
  }
  if (cmd >= kEncMenuBase && cmd < kEncMenuBase + kEncMenuCount) {
    const uint32_t cp = kEncMenu[cmd - kEncMenuBase].codepage;
    if (cp != st->encOverride) {
      st->encOverride = cp;
      LogLine(L"VIEWER: 右键菜单指定编码 cp=%u，重开文档", cp);
      if (st->szFile[0]) OpenDoc(st);
    }
    return;
  }
  if (cmd >= kCbMenuBase && cmd < kCbMenuBase + kCbMenuItemCount) {
    ApplyCodeBlockMenu(cmd);
    return;
  }
  if (cmd == kCtxCopy) {
    if (st->hasHost && WebHostIsReady()) WebHostPost(st->hwndSelf, L"copy|");
    return;
  }
  if (cmd == kCtxSelectAll) {
    if (st->hasHost && WebHostIsReady()) WebHostPost(st->hwndSelf, L"selectall|");
    return;
  }
  if (cmd == kCtxPrint) {
    StartPrint(st);
    return;
  }
  if (cmd == kCtxExportPdf) {
    StartExport(st);
    return;
  }
  if (cmd == kCtxReload) {
    LogLine(L"VIEWER: 右键菜单刷新文档");
    if (st->szFile[0]) OpenDoc(st);
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

  // 导出要跨页面、宿主线程、文件三层，出问题时只有这一条时间线能对齐
  if (type == L"export" ||
      (type.rfind(L"export-", 0) == 0 && type != L"export-progress")) {
    LogLine(L"VIEWER: 页面消息 %s|%s", type.c_str(), rest.c_str());
  }

  if (type == L"proto") {
    const int proto = _wtoi(rest.c_str());
    LogLine(L"VIEWER: 页面协议 %d，插件要求 %d", proto, kPageProto);
    // 启动时的自报只记日志；放行与否一律看导出前那次握手的当次应答
    if (!st->protoQueryPending) return;
    st->protoQueryPending = false;
    if (proto != kPageProto) {
      LogLine(L"VIEWER: 拒绝导出，页面协议 %d ≠ %d", proto, kPageProto);
      SetStatus(st, L"web 资源与插件版本不一致（资源 %d / 插件 %d），请一并更新",
                proto, kPageProto);
      return;
    }
    RequestExportPlan(st);
    return;
  }

  if (type == L"event") {
    const std::wstring kind = msg::SecondField(rest);
    const std::wstring detail = rest.size() > kind.size() + 1 ? rest.substr(kind.size() + 1) : L"";

    if (kind == L"rendered") {
      LARGE_INTEGER t1;
      ::QueryPerformanceCounter(&t1);
      const double total = st->qpcLoad ? QpcMs(st->qpcLoad, t1.QuadPart) : 0.0;
      LogLine(L"VIEWER: 首屏渲染完成 %s（LOADW 起 %.1fms）", detail.c_str(), total);
      LogMemory(L"首屏后");
    } else if (kind.find(L"error") != std::wstring::npos || kind == L"csp" ||
               kind == L"skin-invalid" || kind == L"mermaid-unavailable" ||
               kind == L"anchor-missing" || kind == L"perf") {
      LogLine(L"VIEWER: 页面事件 \"%s\" %s", kind.c_str(), detail.c_str());
    }
    return;
  }

  if (type == L"print-ready") {
    const bool ok = WebHostShowPrintUI(st->hwndSelf);
    LogLine(L"VIEWER: 打印内容就绪，系统打印对话框调用%s", ok ? L"成功" : L"失败");
    if (!ok) SetStatus(st, L"打印不可用：WebView2 运行时过旧");
    return;
  }

  if (type == L"print-done") {
    LogLine(L"VIEWER: 打印对话框已关闭");
    // 导出期间也可能收到迟到的 print-done：打印皮肤不能在这时候被提前撤销
    if (st->pagedSkin && !st->exporting) {
      st->pagedSkin = false;
      PushChrome(st);
    }
    return;
  }

  if (type == L"export-plan") {
    // export-plan|<每份块数>|<总份数>：页面按真实块数算的，确认框用它
    if (!st->exportPlanPending) return;
    st->exportPlanPending = false;
    int v[2] = {0, 1};
    ParseIntFields(rest, v, ARRAYSIZE(v));
    const bool interactive = st->testExportPath.empty();
    StartExportWithPlan(st, v[0], v[1], interactive);
    return;
  }

  if (type == L"export-request") {
    // 快捷键与菜单都走这里：保存对话框必须在查看器线程上弹
    StartExport(st);
    return;
  }

  if (type == L"export-ready") {
    if (!st->exporting) return;
    // export-ready|<段号 0 基>|<总段数>|<页数估算>|<DOM 节点数>
    int v[4] = {0, 1, 0, 0};
    ParseIntFields(rest, v, ARRAYSIZE(v));
    st->exportSeg = v[0];
    st->exportSegs = v[1] > 0 ? v[1] : 1;
    st->exportSegPages = v[2] > 0 ? v[2] : 0;
    st->exportSegNodes = v[3] > 0 ? v[3] : 0;
    // 每段单独计时：整轮由页面串行驱动，总耗时可以是几十分钟
    st->pagedStartTick = ::GetTickCount64();
    st->waitingPdf = true;
    st->lastStatusSec = 0;
    st->exportSegBuiltBlocks = 0;
    // 分页耗时按本份的排版对象个数折算（页面量的 DOM 节点数），这是三份样本里唯一
    // 跟耗时同向的量：页数相同、块数相同都可能差 8 倍，节点数不会
    st->exportPredMs =
        PredictExportMs(st->exportSegNodes, st->exportSegPages, st->exportSegBlocks);
    UpdateExportStatus(st);
    const std::wstring target = PartPath(st->exportPath, st->exportSeg, st->exportSegs);
    const bool ok = WebHostExportPdf(st->hwndSelf, target);
    LogLine(L"VIEWER: 第 %d/%d 份内容就绪（%d 块、约 %d 页、%d 个节点，预计 %llu 毫秒），PrintToPdfStream %s -> %s",
            st->exportSeg + 1, st->exportSegs, st->exportSegBlocks, st->exportSegPages,
            st->exportSegNodes, static_cast<unsigned long long>(st->exportPredMs),
            ok ? L"已发起" : L"失败", target.c_str());
    if (!ok) EndExport(st, L"导出失败：预览引擎不可用");
    return;
  }

  if (type == L"export-progress") {
    if (!st->exporting) return;
    // export-progress|<段号 0 基>|<总份数>|<本份已铺块>|<本份总块>
    int v[4] = {0, 1, 0, 0};
    ParseIntFields(rest, v, ARRAYSIZE(v));
    if (v[3] > 0) {
      // 本份真实块数：分页耗时按它折算，块数比用计划值准
      st->exportSegBlocks = v[3];
      st->exportSegBuiltBlocks = v[2] > v[3] ? v[3] : (v[2] > 0 ? v[2] : 0);
      st->exportSeg = v[0];
      st->exportSegs = v[1] > 0 ? v[1] : 1;
      UpdateExportStatus(st);
    }
    return;
  }

  if (type == L"export-done") {
    // 一份写完（宿主线程的回调）：让页面继续铺下一份，全部铺完由页面收尾
    if (!st->exporting) return;
    st->waitingPdf = false;
    ++st->exportOk;
    st->exportParts.push_back(rest);
    // 用实测校正折算系数，并记下这一份的真实耗时（份与份内容量相近，剩余时间靠它推）。
    // 块数太少时耗时几乎全是固定开销，反推出来的单价没有意义，CalibrateExport 会跳过。
    const ULONGLONG spent = ::GetTickCount64() - st->pagedStartTick;
    CalibrateExport(static_cast<double>(spent), st->exportSegNodes, st->exportSegPages,
                    st->exportSegBlocks, st->exportOk == 1);
    g_exportSegMs = static_cast<double>(spent);
    st->exportDoneMs += static_cast<double>(spent);
    LogLine(L"VIEWER: 第 %d/%d 份已写入 %s（实测 %llu 毫秒，预计 %llu；%d 块 %d 页 %d 节点；"
            L"折算 %.2f 毫秒/节点、%.2f 毫秒/页、%.2f 毫秒/块）",
            st->exportSeg + 1, st->exportSegs, rest.c_str(),
            static_cast<unsigned long long>(spent),
            static_cast<unsigned long long>(st->exportPredMs), st->exportSegBlocks,
            st->exportSegPages, st->exportSegNodes, g_exportMsPerNode, g_exportMsPerPage,
            g_exportMsPerBlock);
    wchar_t ack[32];
    ::StringCchPrintfW(ack, ARRAYSIZE(ack), L"export-ack|%d", st->exportSeg);
    WebHostPost(st->hwndSelf, ack);
    return;
  }

  if (type == L"export-all-done") {
    const std::wstring target = st->exportPath;
    const std::wstring leaf = UtilLeafName(target.c_str());
    const size_t parts = st->exportParts.size();
    if (parts > 1) {
      // 分份只是为了绕开引擎分页，用户要的是一份：这里拼起来再删中间文件
      SetStatus(st, L"正在合并 %d 份 PDF…", static_cast<int>(parts));
      std::wstring err;
      const bool ok = PdfMergeFiles(st->exportParts, target, &err);
      if (ok) {
        for (const std::wstring& p : st->exportParts) ::DeleteFileW(p.c_str());
        LogLine(L"VIEWER: 已合并 %d 份为 %s", static_cast<int>(parts), target.c_str());
      } else {
        LogLine(L"VIEWER: 合并失败：%s（分份文件保留）", err.c_str());
      }
      st->exportParts.clear();
      EndExport(st, nullptr);
      if (ok) {
        SetStatus(st, L"已导出 PDF：%s", leaf.c_str());
      } else {
        SetStatus(st, L"合并失败：%s（分份文件已保留）", err.c_str());
      }
      return;
    }

    LogLine(L"VIEWER: 导出完成 %s", target.c_str());
    st->exportParts.clear();
    EndExport(st, nullptr);
    SetStatus(st, L"已导出 PDF：%s", leaf.c_str());
    return;
  }

  if (type == L"export-error") {
    // 宿主写失败的这一份要明确通知页面：它正停在 waitExportAck 上
    const bool fromHost = st->exporting;
    const bool cancelled = rest.find(L"已取消") != std::wstring::npos;
    const int ok = st->exportOk;
    st->waitingPdf = false;
    LogLine(L"VIEWER: 导出失败：%s", rest.c_str());
    if (fromHost) WebHostPost(st->hwndSelf, L"export-fail|" + rest);
    // 取消是放弃这次导出，中间文件留着只会占地方；真正的失败保留它们，便于事后补救
    if (cancelled) {
      for (const std::wstring& p : st->exportParts) ::DeleteFileW(p.c_str());
    }
    EndExport(st, nullptr);
    if (cancelled) {
      if (ok > 0) {
        SetStatus(st, L"已取消导出（放弃已生成的 %d 份）", ok);
      } else {
        SetStatus(st, L"已取消导出");
      }
    } else {
      SetStatus(st, L"%s", rest.empty() ? L"导出失败" : rest.c_str());
    }
    return;
  }

  if (type == L"copyset") {
    const std::wstring flag = msg::SecondField(rest);
    const std::wstring text = rest.size() > flag.size() + 1 ? rest.substr(flag.size() + 1)
                                                             : std::wstring();
    CopyRenderedText(st, _wtoi(flag.c_str()), text);
    return;
  }

  if (type == L"clip") {
    const std::wstring what = msg::SecondField(rest);
    const std::wstring text = rest.size() > what.size() + 1 ? rest.substr(what.size() + 1)
                                                            : std::wstring();
    CopyLabeledText(st, what, text);
    return;
  }

  if (type == L"status") {
    SetStatus(st, L"%s", rest.c_str());
    return;
  }

  if (type == L"ctxmenu") {
    // 两边都自绘菜单：命令号直接回来，不会像 DOpus 自己的菜单那样丢掉。
    // 全局菜单不带图片动作区：只有 imgctx 帧才允许。
    st->imgCtxUrl.clear();
    st->imgCtxKind.clear();
    ShowOwnContextMenu(st);
    return;
  }

  if (type == L"imgctx") {
    // 页面右键了图片：rest = <图片URL>|<local|remote>。kind 取最后一个字段，
    // URL 里不会出现裸 |（路径部分页面已 encodeURIComponent）。
    const size_t cut = rest.rfind(L'|');
    const std::wstring kind = cut == std::wstring::npos ? std::wstring() : rest.substr(cut + 1);
    if (kind != L"local" && kind != L"remote") {
      LogLine(L"VIEWER: imgctx 帧不认识 kind \"%s\"", kind.c_str());
      return;
    }
    st->imgCtxUrl = cut == std::wstring::npos ? rest : rest.substr(0, cut);
    st->imgCtxKind = kind;
    ShowOwnContextMenu(st);
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
    // 预览器不做导航：页面里已做过二次确认，这里交回系统打开。
    // 文档目录内的相对链接在页面里被解析成 https://mddoc.localhost/...，那是
    // WebView2 的虚拟主机，直接交给 ShellExecuteW 必然打不开，先还原成磁盘路径。
    LogLine(L"VIEWER: open 帧 %s", rest.c_str());
    std::wstring target;
    bool resolved = docsrc::DocUrlToPath(rest, &target);
    if (!resolved) {
      // 页面只把过了 DOMPurify 的链接改写成虚拟主机地址；纯 Markdown 块不经过它，
      // 发上来的是原始相对路径。补一次前缀再还原，别把 "a.png" 交给 ShellExecute。
      const bool relative = rest.find(L"://") == std::wstring::npos &&
                            !UtilIsLocalAbsolutePath(rest) &&
                            rest.compare(0, 2, L"\\\\") != 0;
      if (relative) resolved = docsrc::DocUrlToPath(std::wstring(kDocBase) + rest, &target);
    }
    if (resolved) {
      if (!UtilFileExists(target) && !UtilDirExists(target)) {
        LogLine(L"VIEWER: 相对链接指向的文件不存在 %s", target.c_str());
        SetStatus(st, L"打不开链接：文件不存在（%s）", UtilLeafName(target.c_str()));
        return;
      }
      LogLine(L"VIEWER: 打开相对链接 %s", target.c_str());
      // 本地文件优先交给 DOpus 独立查看器（dopusrt /show）：图片、md、文本都能在
      // DOpus 里看，和双击资源管理器里同一类文件的行为一致。目录不算——查看器只显示
      // 文件，目录仍走系统（DOpus 替换资源管理器时就是 DOpus 文件窗口）。
      if (UtilFileExists(target)) {
        const Config cfg = ConfigSnapshot();
        const wchar_t* ext = ::PathFindExtensionW(target.c_str());
        const std::wstring viewerExe = UtilDOpusViewerExe();
        const bool wantDOpus = cfg.openLinksInOpusViewer;
        const bool handled = UtilDOpusViewerHandlesExt(ext, cfg.viewerExts);
        const bool shown = wantDOpus && handled && UtilDOpusViewerShow(target);
        LogLine(L"VIEWER: 链接 %s ext=\"%s\" cfg=%d handled=%d exe=\"%s\" show=%d", target.c_str(),
                ext, wantDOpus ? 1 : 0, handled ? 1 : 0, viewerExe.c_str(), shown ? 1 : 0);
        if (shown) {
          LogLine(L"VIEWER: 相对链接交给 DOpus 查看器 %s", target.c_str());
          return;
        }
      }
      ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return;
    }
    LogLine(L"VIEWER: 打开外部链接 %s", rest.c_str());
    // 虚拟主机地址交给系统必然打不开（DNS 里没有 mddoc.localhost）。走到这里说明
    // 页面把本地链接当成了外部链接，是内部不一致，别再静默丢给 ShellExecute。
    const size_t baseLen = ::wcslen(kDocBase);
    if (rest.size() >= baseLen && _wcsnicmp(rest.c_str(), kDocBase, baseLen) == 0) {
      SetStatus(st, L"打不开链接：本地路径还原失败");
      return;
    }
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

// 状态栏只有一条口径：整篇进度 + 整篇剩余。铺全文与引擎分页两段合成一份算，
// 换段时立即按新一份的预计刷新，所以剩余时间既不退回、也不消失。
// 剩余按「完成各份的实测 + 本份预计 + 后面几份按上一份实测」推，减去整篇已用时间。
void UpdateExportStatus(ViewerState* st) {
  if (!st->exporting) return;
  const int segs = st->exportSegs > 0 ? st->exportSegs : 1;
  const int seg = st->exportSeg;
  const double pred = st->exportPredMs > 0
                          ? st->exportPredMs
                          : PredictExportMs(st->exportSegNodes, st->exportSegPages,
                                            st->exportSegBlocks);
  const ULONGLONG now = ::GetTickCount64();
  if (!st->exportRunStartTick) st->exportRunStartTick = now;

  // 份内进度：铺全文占一成半（按块数），引擎分页占一成半到九成半（按预计耗时）
  double intra = 0.0;
  if (st->waitingPdf) {
    const double elapsed = static_cast<double>(now - st->pagedStartTick);
    intra = pred > 0 ? 0.15 + 0.80 * elapsed / pred : 0.95;
  } else if (st->exportSegBlocks > 0) {
    intra = 0.15 * static_cast<double>(st->exportSegBuiltBlocks) / st->exportSegBlocks;
  }
  if (intra > 0.95) intra = 0.95;
  if (intra < 0.0) intra = 0.0;
  double pct = (static_cast<double>(seg) + intra) * 100.0 / segs;
  if (pct > 95.0) pct = 95.0;

  const double perSegRef = g_exportSegMs > 0 ? g_exportSegMs : pred;
  double totalPred = st->exportDoneMs + pred;
  totalPred += static_cast<double>(segs - seg - 1) * perSegRef;
  double leftMs = totalPred - static_cast<double>(now - st->exportRunStartTick);
  if (leftMs < 0) leftMs = 0;

  // 第一份还没量出来之前（只有按块数的兜底值），剩余会偏得离谱，先只说进度；
  // 有了实测或本份的页数/节点数估算之后再报。措辞沿用系统惯用的「大约剩余」
  // （Windows 复制对话框的 About x minutes remaining），这里缩成「剩余约」。
  wchar_t tail[48] = L"";
  const bool known = st->waitingPdf || st->exportDoneMs > 0 || g_exportSegMs > 0;
  if (known && leftMs >= 55000) {
    ::StringCchPrintfW(tail, ARRAYSIZE(tail), L"，剩余约 %llu 分钟",
                       static_cast<unsigned long long>((leftMs + 30000) / 60000));
  } else if (known && leftMs >= 4000) {
    ::StringCchPrintfW(tail, ARRAYSIZE(tail), L"，剩余约 %llu 秒",
                       static_cast<unsigned long long>((leftMs + 999) / 1000));
  }
  const int shown = static_cast<int>(pct + 0.5);
  if (shown == st->exportLastPct && seg == st->exportLastSeg &&
      wcscmp(tail, st->exportLastTail) == 0) {
    return;
  }
  st->exportLastPct = shown;
  st->exportLastSeg = seg;
  ::StringCchCopyW(st->exportLastTail, ARRAYSIZE(st->exportLastTail), tail);
  if (segs > 1) {
    SetStatus(st, L"正在生成第 %d/%d 段 PDF… %d%%%s", seg + 1, segs, shown, tail);
  } else {
    SetStatus(st, L"正在生成 PDF… %d%%%s", shown, tail);
  }
}

// 没拿到宿主的原因有两种，状态栏与占位文字要分开说：宿主窗口存在但正在被别的
// 查看器借用，和宿主线程/窗口还没建起来（建线程有几秒窗口期）。
const wchar_t* NoHostReason() {
  return WebHostHostExists() ? L"预览正显示在另一个窗口" : L"预览引擎正在启动";
}

void HandleLoad(ViewerState* st, const wchar_t* pszPath) {
  if (!pszPath) pszPath = L"";
  // 模态对话框（确认框/保存框）开着时 DOpus 照样发 LOADW。此刻换文档会把导出
  // 流程踩断：对话框返回后还在用旧文档的路径与块数。先记下，等对话框关掉再打开。
  if (st->dlgBusy) {
    ::StringCchCopyW(st->pendingFile, ARRAYSIZE(st->pendingFile), pszPath);
    st->pendingLoad = true;
    LogLine(L"VIEWER: 对话框打开期间收到 LOADW \"%s\"，延后处理", pszPath);
    return;
  }
  // 相对路径会让 mddoc.localhost 映射失败（映射要求绝对路径），这里统一转绝对。
  wchar_t szFull[MAX_PATH * 2];
  const DWORD cchFull = ::GetFullPathNameW(pszPath, ARRAYSIZE(szFull), szFull, nullptr);
  if (cchFull > 0 && cchFull < ARRAYSIZE(szFull)) {
    ::StringCchCopyW(st->szFile, ARRAYSIZE(st->szFile), szFull);
  } else {
    ::StringCchCopyW(st->szFile, ARRAYSIZE(st->szFile), pszPath);
  }
  pszPath = st->szFile;
  // 入口就写一行：DOpus 什么时候让我们换文件，是排查「窗格不刷新」的唯一凭据
  // （外部程序用 Go 定位文件时，DOpus 可能一条消息都不发，日志里必须能看出「没收到」）。
  LogLine(L"VIEWER: LOADW \"%s\" host=%d", pszPath, st->hasHost ? 1 : 0);

  // 预览窗口关闭期间发生的主题切换走不到轮询；每次打开文件重查一次，
  // 别沿用创建窗口时那个已经过期的判定。OpenDoc 随后会按新判定重推皮肤。
  const bool dark = HostThemeDark(st);
  if (dark != st->darkMode) {
    st->darkMode = dark;
    LogLine(L"VIEWER: LOADW 时主题已变 dark=%d", dark ? 1 : 0);
  }

  WIN32_FILE_ATTRIBUTE_DATA fad;
  st->ullFileSize = 0;
  bool haveInfo = false;
  if (::GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad)) {
    st->ullFileSize = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    haveInfo = true;
  }
  // 文档键在这里定：再晚就会被索引就绪时的 fileSize 覆盖，键就不再稳定了。
  st->docKey = haveInfo
                   ? UtilMakeDocKey(pszPath, st->ullFileSize,
                                    (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                        fad.ftLastWriteTime.dwLowDateTime)
                   : UtilMakeDocKey(pszPath, 0, 0);

  LARGE_INTEGER tLoad;
  ::QueryPerformanceCounter(&tLoad);
  st->qpcLoad = tLoad.QuadPart;

  st->docOpened = false;
  st->encOverride = 0;  // 换文件回到自动判码
  docsrc::CloseIfOwner(st->hwndSelf);

  if (!st->hasHost) {
    if (WebHostHostExists()) {
      // 宿主在别的查看器手里：先记住路径，收回宿主时再开索引。
      LogLine(L"VIEWER: 宿主不在本窗口，先不建索引（%s）", pszPath);
      SetStatus(st, L"%s", NoHostReason());
      return;
    }
    // 宿主线程/窗口还没建好（建线程有几秒窗口期）：这里补借一次，失败再转空白。
    if (WebHostBorrow(st->hwndSelf, ClientRectOf(st->hwndSelf))) {
      st->hasHost = true;
      LogLine(L"VIEWER: 补借宿主成功");
    } else {
      LogLine(L"VIEWER: 宿主不可用，先不建索引（%s）", pszPath);
      SetStatus(st, L"%s", NoHostReason());
      return;
    }
  }

  OpenDoc(st);
}

void HandleClear(ViewerState* st) {
  LogLine(L"VIEWER: CLEAR");
  st->szFile[0] = L'\0';
  st->ullFileSize = 0;
  st->docKey.clear();
  st->docOpened = false;
  st->encOverride = 0;
  st->indexFullPending = false;
  docsrc::CloseIfOwner(st->hwndSelf);

  if (st->hasHost && WebHostIsReady()) {
    WebHostPost(st->hwndSelf, msg::MakeClear());
  }
  // 没有文档了，状态栏也不该再挂着上一个文件的提示
  ResetStatus(st);
  ::InvalidateRect(st->hwndSelf, nullptr, TRUE);
}

LRESULT HandleMessage(ViewerState* st, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case kMsgPendingLoad:
      // 对话框关掉之后补做被延后的加载：换到新消息里做，避免还压在对话框的栈上
      if (st->pendingFile[0]) HandleLoad(st, st->pendingFile);
      return 0;

    case WM_CREATE: {
      st->hwndParent = ::GetParent(hwnd);

      if (!ViewerRegisterClass(g_hInstance)) {
        LogLine(L"VIEWER: 窗口类未注册");
      }

      // controller 挂在宿主窗口下；这里只是把宿主借进客户区。
      WebHostEnsure();
      st->hasHost = WebHostBorrow(hwnd, ClientRectOf(hwnd));
      RegisterViewer(hwnd);
      ::SetTimer(hwnd, kDarkPollTimerId, kDarkPollMs, nullptr);
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
      // wParam 是发起扫描时的文档代号。换文件瞬间旧文档的"全文索引完成"会落到
      // 新文档上，把 indexFullPending/docComplete 错置并提前推 index-full。
      if (static_cast<uint32_t>(wParam) != docsrc::DocId()) {
        LogLine(L"VIEWER: 忽略过期的索引完成通知 doc=%u 当前=%u",
                static_cast<unsigned>(wParam), docsrc::DocId());
        return 0;
      }
      st->indexFullPending = true;
      st->docComplete = true;
      MaybePushIndexFull(st);
      return 0;

    case WM_MDV_CONFIG_CHANGED:
      LogLine(L"VIEWER: 设置已更新，重推皮肤/字体/网络图档位/链接确认等待");
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
      ::KillTimer(hwnd, kStatusResetTimerId);
      WebHostUnborrow(hwnd);
      docsrc::CloseIfOwner(hwnd);
      st->docOpened = false;
      st->hasHost = false;
      return 0;

    case WM_ERASEBKGND: {
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      HBRUSH brush = ::CreateSolidBrush(st->bgColor);
      ::FillRect(reinterpret_cast<HDC>(wParam), &rc, brush);
      ::DeleteObject(brush);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = ::BeginPaint(hwnd, &ps);
      RECT rc;
      ::GetClientRect(hwnd, &rc);

      // 每次自绘都先铺满皮肤底色。宿主已就绪但被藏起来时（复用实例）旧逻辑在这里
      // 什么都不画，只靠 WM_ERASEBKGND，系统合成的那一帧会从这里漏出默认底色。
      // WS_CLIPCHILDREN 会把可见的 WebView2 宿主区域自动剪掉，铺底不盖内容。
      HBRUSH bgBrush = ::CreateSolidBrush(st->bgColor);
      ::FillRect(hdc, &rc, bgBrush);
      ::DeleteObject(bgBrush);
      // ClearType 需要知道背景色；文字画在这块底色上。
      ::SetBkMode(hdc, OPAQUE);
      ::SetBkColor(hdc, st->bgColor);

      const bool bgDark = ColorLuma(st->bgColor) < 128;
      const COLORREF textColor = bgDark ? RGB(0xD8, 0xD8, 0xD8) : ::GetSysColor(COLOR_WINDOWTEXT);
      const COLORREF hintColor = bgDark ? RGB(0x96, 0x96, 0x96) : ::GetSysColor(COLOR_GRAYTEXT);

      const bool loading = !st->webFailed && st->hasHost && !WebHostIsReady() && st->szFile[0];
      if (st->webFailed || !st->hasHost || loading) EnsurePlaceholderFonts(st, hwnd);

      if (st->webFailed) {
        HGDIOBJ oldFont = ::SelectObject(
            hdc, st->fontHint ? st->fontHint : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(hdc, textColor);
        ::DrawTextW(hdc, L"WebView2 不可用，无法预览 Markdown。详见日志。", -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SelectObject(hdc, oldFont);
      } else if (!st->hasHost) {
        // 宿主在另一个查看器手里（或引擎还在启动）：给一行说明，别留一片白。
        HGDIOBJ oldFont = ::SelectObject(
            hdc, st->fontHint ? st->fontHint : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(hdc, hintColor);
        ::DrawTextW(hdc, NoHostReason(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        ::SetTextColor(hdc, textColor);
        ::DrawTextW(hdc, UtilLeafName(st->szFile), -1, &rcName,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(hdc, st->fontHint ? st->fontHint : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(hdc, hintColor);
        ::DrawTextW(hdc, L"正在加载预览…", -1, &rcHint,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SelectObject(hdc, oldFont);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case DVPLUGINMSG_LOADW:
      // 这个返回值是「加载成功/失败」。返回 FALSE 会让 DOpus 立刻 CLEAR 并销毁窗口。
      HandleLoad(st, reinterpret_cast<const wchar_t*>(lParam));
      return TRUE;

    // 只在我们申报了 DVPFIF_CanHandleStreams 时才会来。没申报却收到，说明 DOpus
    // 走了另一条加载路径；记下来，别让它静默变成「窗格空白」。
    case DVPLUGINMSG_LOADSTREAMW:
      LogLine(L"VIEWER: 收到 LOADSTREAMW（未实现），按失败返回");
      return FALSE;

    case DVPLUGINMSG_CLEAR:
      HandleClear(st);
      return 0;

    case DVPLUGINMSG_PRINT:
      StartPrint(st);
      return 0;

    case DVPLUGINMSG_TESTSELECTION:
      // DOpus 用它决定「复制」是否置灰：不实现就一直返回 0，菜单项永远是灰的。
      return st->hasSelection ? TRUE : FALSE;

    case DVPLUGINMSG_SELECTALL:
      // DOpus 菜单里的「全选」只做选中；随后的「复制」走下一分支
      if (st->hasHost && WebHostIsReady()) WebHostPost(st->hwndSelf, L"selectall|");
      return 0;

    case DVPLUGINMSG_COPYSELECTION:
      // DOpus 菜单/命令里的「复制」：有选区就复制选区文本，没有就把全文渲染文本放进剪贴板。
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
      // COPYSELECTION 必须报：不报 DOpus 会认为本查看器不能复制，「复制」菜单项恒灰。
      // 不报 ADDCONTEXTMENU：右键菜单由插件自绘，命令号才回得来。
      return VPCAPABILITY_WANTFOCUS | VPCAPABILITY_SELECTALL | VPCAPABILITY_COPYSELECTION |
             VPCAPABILITY_PRINT;

    case DVPLUGINMSG_PREVENTAUTOSIZE:
      return TRUE;

    case DVPLUGINMSG_SETABORTEVENT:
      // 收下但不轮询：能中止的只有同步前缀扫描（≤8MB，通常几十毫秒）与后台全文
      // 索引，后者自带 cancel 标志。这个事件由宿主自己置位/复位，我们在 LOADW 入口
      // 读到「已置位」时无法区分「本次要中止」和「上次的残留」，误判会让下一个文件
      // 干脆不加载，代价远大于省下那几十毫秒。
      st->hAbortEvent = reinterpret_cast<HANDLE>(lParam);
      return 0;

    case DVPLUGINMSG_REINITIALIZE:
      // 只是通知，不销毁窗口（viewer plugins.h:319）。配置变了就重推皮肤与字体；
      // 字号变化后的位置恢复由前端按锚点处理。
      LogLine(L"VIEWER: REINITIALIZE，重推皮肤与字体");
      if (WebHostIsReady()) PushChrome(st);
      return 0;

    case DVPLUGINMSG_REDRAW:
      // wParam=TRUE 时 lParam 是新背景色（viewer plugins.h:307）。颜色变了就说明
      // 主题换了：重查宿主，皮肤跟着换。
      {
        const bool dark = HostThemeDark(st);
        if (dark != st->darkMode) {
          st->darkMode = dark;
          LogLine(L"VIEWER: DOpus 深浅色切换 dark=%d（REDRAW），重推皮肤", dark ? 1 : 0);
          PushChrome(st);
        }
      }
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;

    case WM_TIMER:
      if (wParam == kStatusResetTimerId) {
        ResetStatus(st);
        return 0;
      }
      if (wParam == kDarkPollTimerId) {
        const bool dark = HostThemeDark(st);
        if (dark != st->darkMode) {
          st->darkMode = dark;
          LogLine(L"VIEWER: DOpus 深浅色切换 dark=%d，重推皮肤", dark ? 1 : 0);
          PushChrome(st);
        }
        // 打印对话框由宿主调起，页面只能靠 afterprint 回报结束；系统对话框这条路
        // 更不能假设它一定来，所以超时兜底恢复打印皮肤
        if (st->pagedSkin && !st->exporting &&
            ::GetTickCount64() - st->pagedStartTick > kPrintLightMaxMs) {
          st->pagedSkin = false;
          LogLine(L"VIEWER: 打印超时未收到 print-done，已恢复跟随皮肤");
          PushChrome(st);
        }
        // 导出在等引擎回调：渲染进程崩掉时回调不会来，别让「正在导出」永久挂住
        if (st->exporting && ::GetTickCount64() - st->pagedStartTick > kExportTimeoutMs) {
          LogLine(L"VIEWER: 导出超时未收到结果，已放弃本次导出");
          EndExport(st, L"导出超时，已放弃");
        }
        // 握手没应答：旧页面不认识 proto-query，明确报出来而不是静默放过
        if (st->protoQueryPending &&
            ::GetTickCount64() - st->protoQueryTick > kProtoQueryTimeoutMs) {
          st->protoQueryPending = false;
          LogLine(L"VIEWER: %llu 毫秒内没收到页面协议应答，按旧资源处理",
                  kProtoQueryTimeoutMs);
          SetStatus(st, L"web 资源与插件版本不一致（页面无应答），请一并更新");
        }
        // 引擎分页这一段拿不到真实进度，按「本份排版量折算的耗时」推进。
        // 每秒刷新一次即可，文案与剩余时间都交给 UpdateExportStatus，别在这里另算一套。
        if (st->exporting && st->waitingPdf) {
          const ULONGLONG sec = (::GetTickCount64() - st->pagedStartTick) / 1000;
          if (sec != st->lastStatusSec) {
            st->lastStatusSec = sec;
            UpdateExportStatus(st);
          }
        }
        // 页面先等全文索引（最长 20 秒）再采样节点密度算计划，超时给足余量
        if (st->exportPlanPending && ::GetTickCount64() - st->planTick > 60000) {
          st->exportPlanPending = false;
          LogLine(L"VIEWER: 60 秒内没收到导出计划，放弃");
          SetStatus(st, L"导出准备超时（页面无响应）");
        }
        return 0;
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

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
      // 从这一刻起 st 归窗口管：创建失败也会走 WM_NCDESTROY，由它释放。
      if (st->pClaimed) *st->pClaimed = true;
      // 认领凭据只用一次。st 此后随时可能被 WM_NCDESTROY 释放，调用方不能再
      // 经它回写栈上的 claimed，否则创建失败时就是一次释放后写入。
      st->pClaimed = nullptr;
      st->hwndSelf = hwnd;
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      // 与下面的 WM_NCDESTROY 配对：创建失败时 DOpus 会紧接着销毁窗口，
      // 计数绝不能只在成功路径上加，否则失败一次就欠一个。
      ::InterlockedIncrement(&g_liveViewers);
    }
  }

  if (uMsg == WM_NCDESTROY) {
    UnregisterViewer(hwnd);
    if (st) {
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      // 先作废存活令牌：正在模态循环里的调用方据此放弃后续操作
      if (st->alive) *st->alive = false;
      if (st->fontName) ::DeleteObject(st->fontName);
      if (st->fontHint) ::DeleteObject(st->fontHint);
      delete st;
      ::InterlockedDecrement(&g_liveViewers);
    }
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

  if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogLine(L"VIEWER: RegisterClassExW 失败 err=%lu", ::GetLastError());
    return false;
  }

  // 同名类已在系统里。多半是上一次加载留下的：DLL 卸载不会带走窗口类，它带着
  // 旧映像的 lpfnWndProc 留在系统里，直接复用就是让消息跳已卸载的代码。
  // 先尝试注销再注册；注销不掉（还有旧窗口活着）只能沿用旧类。
  if (!::UnregisterClassW(kClassName, hInstance)) {
    LogLine(L"VIEWER: 同名窗口类无法注销 err=%lu，沿用旧类", ::GetLastError());
    registered = true;
    return true;
  }
  if (::RegisterClassExW(&wc)) {
    LogLine(L"VIEWER: 已替换上一次加载留下的同名窗口类");
    registered = true;
    return true;
  }
  LogLine(L"VIEWER: 替换同名窗口类失败 err=%lu", ::GetLastError());
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
  st->dlgBusy = false;
  st->pendingLoad = false;
  st->pendingFile[0] = L'\0';
  try {
    st->alive = std::make_shared<bool>(true);
  } catch (...) {
    LogLine(L"VIEWER: 存活令牌分配失败");
    delete st;
    return nullptr;
  }
  st->szFile[0] = L'\0';
  st->ullFileSize = 0;
  st->docKey.clear();
  st->prefixBlocks = 0;
  st->estBlocks = 0;
  st->lastVisible = 0;
  st->encoding = 0;
  st->encOverride = 0;
  st->docComplete = false;
  st->qpcLoad = 0;
  st->docOpened = false;
  st->indexFullPending = false;
  st->webFailed = false;
  st->hasHost = false;
  st->hasSelection = false;
  st->darkMode = HostThemeDark(st);
  st->pagedSkin = false;
  st->pagedStartTick = 0;
  st->exporting = false;
  st->exportPath.clear();
  st->exportSeg = 0;
  st->exportSegs = 1;
  st->exportOk = 0;
  st->exportSegBlocks = 0;
  st->exportSegPages = 0;
  st->exportSegNodes = 0;
  st->exportPredMs = 0;
  st->exportRunStartTick = 0;
  st->exportSegBuiltBlocks = 0;
  st->exportDoneMs = 0;
  st->exportLastPct = -1;
  st->exportLastSeg = -1;
  st->exportLastTail[0] = L'\0';
  st->protoQueryPending = false;
  st->protoQueryTick = 0;
  st->exportPlanPending = false;
  st->planTick = 0;
  st->waitingPdf = false;
  st->lastStatusSec = 0;
  // 初始底色：第一帧占位就要按主题铺，不能等 PushChrome（它要页面就绪才跑）
  st->bgColor = st->darkMode ? RGB(0x28, 0x2C, 0x34) : ::GetSysColor(COLOR_WINDOW);
  {
    const Config cfg = ConfigSnapshot();
    std::wstring css;
    if (SkinLoad(st->darkMode ? cfg.skinDark : cfg.skinLight, &css)) {
      st->bgColor = UtilParseSkinBg(css, st->bgColor);
    }
  }
  st->pClaimed = nullptr;
  st->fontName = nullptr;
  st->fontHint = nullptr;
  st->fontDpi = 0;
  st->fontNamePx = 0;
  st->fontHintPx = 0;
  st->fontFace[0] = L'\0';
  st->szStatus[0] = L'\0';

  const int cx = rc.right - rc.left;
  const int cy = rc.bottom - rc.top;

  // 创建成功与否都以 WM_NCCREATE 是否认领 st 为准：认领过就由 WM_NCDESTROY 释放，
  // 这里再 delete 一次就是双重释放。pClaimed 已在认领时清掉，返回后不能再碰 st——
  // 失败路径上它可能已被 WM_NCDESTROY 释放。
  bool claimed = false;
  st->pClaimed = &claimed;

  HWND hwnd = ::CreateWindowExW(0, kClassName, kTitle, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                rc.left, rc.top, cx, cy, hWndParent, nullptr, g_hInstance, st);
  if (!hwnd) {
    LogLine(L"VIEWER: CreateWindowExW 失败 err=%lu", ::GetLastError());
    if (!claimed) delete st;  // 连 WM_NCCREATE 都没走到：st 从未交给窗口
    return nullptr;
  }

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

// 只给 tools/hosttest：跳过保存对话框，其余流程与右键菜单一致（皮肤、分段、状态栏）。
bool ViewerTestExport(HWND hViewer, const wchar_t* path) {
  if (!hViewer || !path || !*path) return false;
  ViewerState* st = reinterpret_cast<ViewerState*>(::GetWindowLongPtrW(hViewer, GWLP_USERDATA));
  if (!st || !st->docOpened || !st->hasHost || st->webFailed || !WebHostIsReady()) return false;
  if (st->exporting || st->exportPlanPending || st->protoQueryPending) return false;

  // 与右键菜单完全同一条链路（含导出前的协议握手），只是计划到手后不弹对话框
  st->testExportPath = path;
  StartExport(st);
  return st->protoQueryPending;
}
