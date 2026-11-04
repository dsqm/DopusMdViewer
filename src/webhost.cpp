// webhost.cpp - 把 WebView2 全部放到插件自己的一个线程上。
//
// 五条硬事实，都是踩出来的：
//  1. **WebView2 的对象是线程绑定的**。Opus 的预览窗格与独立查看器窗口跑在不同
//     的 UI 线程上（日志里 18112 与 15248），共用全局 controller 会让第二个线程
//     的 postMessage 直接失败（0x802A000C）。因此这里自建一个线程，Environment
//     与 Controller 只活在那条线程上，其余线程通过消息借用。
//  2. controller 的父窗口一旦销毁就永久失效，所以它只认插件自持的宿主窗口，
//     Opus 的查看器窗口通过 SetParent 借用。归还必须在 WM_DESTROY 做：
//     DestroyWindow 会连子窗口一起销毁，等到 WM_NCDESTROY 宿主已经没了。
//  3. 宿主线程与进程同寿命：关掉窗口再打开只是 SetParent 加一次推送，不必再付
//     浏览器进程冷启动的两三秒。预热开着时自持一份 DLL 引用，Uninit 不再收掉
//     线程，controller 也一直保持可用；关掉预热则按 idleReleaseMinutes 空闲回收
//     （ReleaseInstance，下次借用走冷路径）。代价是**同一时刻只有一个查看器有
//     画面**：其它查看器借用会抢走宿主，归还时按借用历史交回上一个还活着的
//     查看器（WM_MDV_HOST_LOST / WM_MDV_RECLAIM），不这样做关掉一个窗口就会
//     留下另一片空白。
//  4. 虚拟主机映射出来的响应由 WebView2 自己合成，加不了响应头；而**映射过的请求
//     也不会进 WebResourceRequested**（实测：通配过滤器 + ALL 上下文一次都不触发）。
//     所以 CSP 只能写在 shell.html 的 meta 里。
//  5. 包含顺序不是可选项：WebView2.h 是 MIDL 生成的，依赖 objbase.h /
//     eventtoken.h / wrl.h 先声明 interface 与事件令牌。

#include "webhost.h"

#include <objbase.h>
#include <objidl.h>
#include <strsafe.h>
#include <shlwapi.h>
#include <eventtoken.h>
#include <wrl.h>
#include <wrl/implements.h>

#include <WebView2.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "config.h"
#include "doc_source.h"
#include "log.h"
#include "util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kHostClass[] = L"DopusMdViewerHostWnd";
const wchar_t kAppHost[] = L"mdprev.localhost";
const wchar_t kAppOrigin[] = L"https://mdprev.localhost";  // CORS 回显用，必须带 scheme
const wchar_t kDocHost[] = L"mddoc.localhost";
const wchar_t kShellUrl[] = L"https://mdprev.localhost/shell.html";

DWORD WINAPI HostThreadProc(LPVOID param);  // ThisModule 用它的地址定位本模块

// 本模块的 HINSTANCE：DLL 里不能用 GetModuleHandleW(nullptr)（那是 dopus.exe）。
// 窗口类必须挂在本模块名下，否则插件卸载后类还留在系统里，lpfnWndProc 指向
// 已卸载的代码，下次加载注册同名类失败又复用旧类 → 消息跳野指针。
HMODULE ThisModule() {
  HMODULE mod = nullptr;
  ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&HostThreadProc), &mod);
  return mod;
}

// 命令号：调用线程 → 宿主线程。参数在 SendMessage 期间有效。
const UINT kCmdBase = WM_APP + 0x200;
const UINT kCmdEnsure = kCmdBase + 1;
const UINT kCmdBorrow = kCmdBase + 2;
const UINT kCmdUnborrow = kCmdBase + 3;
const UINT kCmdBounds = kCmdBase + 4;
const UINT kCmdPost = kCmdBase + 5;
const UINT kCmdDocFolder = kCmdBase + 6;
const UINT kCmdRelease = kCmdBase + 7;
const UINT kCmdScript = kCmdBase + 8;
const UINT kCmdPrewarm = kCmdBase + 9;
const UINT kCmdPrint = kCmdBase + 10;
const UINT kCmdRecover = kCmdBase + 11;  // 进程失效后的自愈，wParam 1 = 连实例一起重建
const UINT kCmdDevTools = kCmdBase + 12;  // 只给 hosttest：发一条 DevTools 方法（造崩溃用）
const UINT kCmdClearUserData = kCmdBase + 13;  // lParam = const std::wstring*
const UINT kCmdCleanupDone = kCmdBase + 14;  // 缓存清理线程完成（无参数）

// 预热：InitEx 后等一会儿再开始，避开 Opus 自己的启动高峰。
const UINT_PTR kTimerPrewarm = 1;
const UINT_PTR kTimerWatch = 2;
const UINT_PTR kTimerIdle = 3;
const UINT kPrewarmDelayMs = 3000;
const UINT kWatchIntervalMs = 2000;
const int kWatchEmptyLimit = 2;  // 连续两次看不见 DO 窗口才释放（约 4 秒）
const int kMaxRoots = 8;
// 自愈预算：一分钟内连续崩这么多次就停手，免得卡在崩—恢复—再崩的循环里。
const int kRecoverBudget = 3;
const LONGLONG kRecoverWindowMs = 60000;

// 以下状态只在宿主线程上访问。
struct Host {
  HWND host = nullptr;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  ComPtr<ICoreWebView2_3> webview3;
  HWND borrower = nullptr;  // 事件接收方
  std::wstring docFolder;
  std::wstring userDataDir;  // 当前环境实际使用的 WebView2 目录
  bool creating = false;
  bool ready = false;
  bool borrowed = false;
  bool prewarmEnabled = false;
  bool prewarmTimerArmed = false;  // 预热定时器已在等：重复 InitEx 不许把它往后推
  bool everSawViewer = false;  // 预热后是否真的被查看器借过
  bool sawDopusWindow = false;
  bool suspended = false;
  int idleReleaseMinutes = 3;
  int watchEmpty = 0;
  // 自愈状态：最近一次恢复的时刻与一分钟内的累计次数（只在宿主线程上读写）。
  LARGE_INTEGER lastRecover = {};
  int recoverCount = 0;
  // 最近一次受理的恢复请求时刻：同一场崩溃会被两个事件各报一次，靠它去重。
  LARGE_INTEGER recoverAcceptedAt = {};
  LARGE_INTEGER tEnsure = {};
  LARGE_INTEGER tController = {};
  LARGE_INTEGER tNav = {};
};

Host g_host;

// 实例世代：每次 ReleaseController/ReleaseInstance 递增。WebView2 的异步完成回调
// 可能晚于释放到达（在途的 CreateController/CreateEnvironment），回调入口比对
// 世代，不符就把传入对象作废，避免旧对象赋回 g_host 造成孤儿实例。
uint64_t g_gen = 0;

// 用过的查看器的顶层祖先（Lister / 独立查看器窗口）。最后一个关掉后释放实例。
HWND g_roots[kMaxRoots] = {};
int g_rootsCount = 0;
int g_memLevel = -1;

// 借用方历史（MRU，最多 8 个）。宿主只有一个窗口，谁最后借用谁显示；归还时按这里
// 把宿主交给上一个还活着的查看器，否则关掉一个窗口就会留下另一片空白。
const int kMaxBorrowHistory = 8;
HWND g_history[kMaxBorrowHistory] = {};
int g_historyCount = 0;

// 跨线程读取的标志与句柄。
volatile LONG g_readyFlag = 0;

// 预热常驻时自持的 DLL 引用。Opus 会在插件仍加载时频繁成对调 Init/Uninit，
// 而 DLL 一旦卸载，宿主线程/搜索线程就会执行已卸载的代码。自持一份引用让
// FreeLibrary 只减一次计数，卸载推迟到进程结束；这份引用不配对释放，关掉预热
// 后也留着，避免在收尾与重建之间出现竞态。
HMODULE g_pin = nullptr;
volatile LONG g_hasThread = 0;
// 宿主线程/搜索线程/缓存清理线程的句柄与失败标志：Uninit 收尾要等三条线程都
// 真正退出后 Opus 才能 FreeLibrary，否则线程还在执行已卸载的代码。
HANDLE g_hHostThread = nullptr;
HANDLE g_hSearchThread = nullptr;
HANDLE g_hCleanThread = nullptr;
HANDLE g_readyEvent = nullptr;
volatile LONG g_threadFailed = 0;
// 预热配置在宿主线程还没建起来时也要记住（预热关闭时 InitEx 不会建线程）。
volatile LONG g_cfgPrewarm = 0;
volatile LONG g_cfgIdleMinutes = 3;
// 自定义运行时建不起来、已回退到系统运行时（设置界面的关于页据此标注）。
volatile LONG g_runtimeFallback = 0;
// 缓存清理在后台线程做：目录大小要递归扫盘，放宿主线程会卡住所有查看器的
// Borrow/Post。清理期间建环境的请求先记下来，完成后在宿主线程上续上。
volatile LONG g_cleanupBusy = 0;
bool g_envQueuedAfterCleanup = false;  // 只在宿主线程访问
DWORD g_threadId = 0;
HWND g_threadHost = nullptr;

std::wstring WebDir() {
  return UtilWebDir();
}

std::wstring UserDataDir() {
  const Config cfg = ConfigSnapshot();
  const std::wstring dir = UtilWebView2UserDataDir(ConfigWebViewBaseDir(cfg));
  if (dir.empty()) return std::wstring();
  if (UtilEnsureDir(dir)) return dir;

  const std::wstring fallback = UtilWebView2UserDataDir(L"");
  if (!fallback.empty() && _wcsicmp(fallback.c_str(), dir.c_str()) != 0 &&
      UtilEnsureDir(fallback)) {
    LogLine(L"WEBHOST: 缓存目录不可用 %s，改用 %s", dir.c_str(), fallback.c_str());
    return fallback;
  }
  LogLine(L"WEBHOST: 缓存目录不可用 %s", dir.c_str());
  return std::wstring();
}

// 只在每个进程第一次建环境前跑一次。缓存是再生的，先删再让 WebView2 重建。
// 注意：目录大小要递归扫盘，只能跑在后台线程（StartCacheCleanAsync）。
void MaybeCleanCacheOnStart(const std::wstring& userData) {
  if (!UtilIsWebView2UserDataDir(userData)) return;

  const uint64_t size = UtilDirSize(userData, kWebViewCacheAutoCleanBytes);
  if (size >= kWebViewCacheAutoCleanBytes) {
    LogLine(L"WEBHOST: 缓存 %llu B 超过上限，自动清理 %s",
            static_cast<unsigned long long>(size), userData.c_str());
    if (UtilRemoveTree(userData)) {
      UtilEnsureDir(userData);
    } else {
      LogLine(L"WEBHOST: 自动清理失败，目录可能仍被占用");
    }
  }

  // 旧版把缓存放在数据根下；默认迁走后顺手清掉，失败下次启动再试。
  const std::wstring legacy = UtilJoin(UtilDataDir(), L"WebView2");
  if (_wcsicmp(legacy.c_str(), userData.c_str()) != 0 && UtilIsWebView2UserDataDir(legacy) &&
      UtilDirExists(legacy)) {
    LogLine(L"WEBHOST: 清理旧缓存 %s", legacy.c_str());
    UtilRemoveTree(legacy);
  }
}

DWORD WINAPI CacheCleanThreadProc(LPVOID param) {
  std::unique_ptr<std::wstring> dir(static_cast<std::wstring*>(param));
  if (dir) MaybeCleanCacheOnStart(*dir);
  HWND host = g_threadHost;
  // 投递不出去（窗口已销毁、消息队列满）就自己解锁：等宿主线程来解会永远卡在
  // g_cleanupBusy 上，之后所有建环境请求都被排队吞掉。
  if (!host || !::IsWindow(host) || !::PostMessageW(host, kCmdCleanupDone, 0, 0)) {
    ::InterlockedExchange(&g_cleanupBusy, 0);
  }
  return 0;
}

void StartCacheCleanAsync() {
  static bool checked = false;  // 只在宿主线程访问：每进程只清一次
  if (checked) return;
  checked = true;

  const std::wstring userData = UserDataDir();
  if (!UtilIsWebView2UserDataDir(userData)) return;

  ::InterlockedExchange(&g_cleanupBusy, 1);
  std::wstring* param = new (std::nothrow) std::wstring(userData);
  HANDLE h = param ? ::CreateThread(nullptr, 0, CacheCleanThreadProc, param, 0, nullptr) : nullptr;
  if (h) {
    // 句柄留到 Uninit：清理线程跑的是本模块代码，结束时还会写 g_cleanupBusy，
    // 随手 CloseHandle 就没有东西可以在卸载前 join 它。
    g_hCleanThread = h;
  } else {
    delete param;
    ::InterlockedExchange(&g_cleanupBusy, 0);
    LogLine(L"WEBHOST: 缓存清理线程创建失败 err=%lu，跳过本次清理", ::GetLastError());
  }
}

// 用户自放的 Fixed Version 运行时：…\DopusMdViewer\Runtime\msedgewebview2.exe。
// 只认这一层，不递归扫描——多层扫描要回答「两份该用哪份」，没有确定答案。
// 结果按进程缓存：换运行时本来就要重启 Opus，没必要每次建实例都去摸盘。
const std::wstring& UserRuntimeDir() {
  static const std::wstring dir = [] {
    const std::wstring dataDir = UtilDataDir();
    if (dataDir.empty()) return std::wstring();
    const std::wstring runtime = UtilJoin(dataDir, L"Runtime");
    return UtilFileExists(UtilJoin(runtime, L"msedgewebview2.exe")) ? runtime : std::wstring();
  }();
  return dir;
}

// ---- 宿主线程 → 借用方 ----

void PostEvent(int kind, const wchar_t* text) {
  HWND target = g_host.borrower;
  if (!target || !::IsWindow(target)) return;

  // 这里跑在 WRL 回调里：分配失败宁可丢事件，也不能让 bad_alloc 逃出 DLL。
  MDV_EVENT* evt = new (std::nothrow) MDV_EVENT();
  if (!evt) return;
  evt->kind = kind;
  evt->text = nullptr;
  if (text && *text) {
    const size_t cch = wcslen(text) + 1;
    evt->text = new (std::nothrow) wchar_t[cch];
    if (!evt->text) {
      delete evt;
      return;
    }
    ::StringCchCopyW(evt->text, cch, text);
  }

  if (!::PostMessageW(target, WM_MDV_EVENT, static_cast<WPARAM>(kind),
                      reinterpret_cast<LPARAM>(evt))) {
    delete[] evt->text;
    delete evt;
  }
}

void Fail(const wchar_t* reason) {
  LogLine(L"WEBHOST: 失败 - %s", reason);
  PostEvent(MDV_EVT_FAILED, reason);
}

// ---- 借用历史（只在宿主线程上访问） ----

void HistoryPush(HWND viewer) {
  if (!viewer) return;
  int slot = -1;
  for (int i = 0; i < g_historyCount; ++i) {
    if (g_history[i] == viewer) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (g_historyCount < kMaxBorrowHistory) {
      slot = g_historyCount++;
    } else {
      slot = kMaxBorrowHistory - 1;  // 溢出时挤掉最旧的一个
    }
  }
  for (int i = slot; i > 0; --i) g_history[i] = g_history[i - 1];
  g_history[0] = viewer;
}

void HistoryDrop(HWND viewer) {
  for (int i = 0; i < g_historyCount; ++i) {
    if (g_history[i] != viewer) continue;
    for (int k = i; k + 1 < g_historyCount; ++k) g_history[k] = g_history[k + 1];
    --g_historyCount;
    return;
  }
}

HWND HistoryFirstLive(HWND except) {
  for (int i = 0; i < g_historyCount; ++i) {
    if (g_history[i] != except && ::IsWindow(g_history[i])) return g_history[i];
  }
  return nullptr;
}

double QpcMs(LONGLONG from, LONGLONG to) {
  static LONGLONG freq = 0;
  if (!freq) {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    freq = f.QuadPart;
  }
  if (!freq || !from) return 0.0;
  return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq);
}

// 空闲时把 WebView2 的内存目标压到 LOW（运行时 1.0.2277+ 才支持，失败就跳过）。
void SetMemoryLevel(bool low) {
  if (!g_host.webview) return;
  ComPtr<ICoreWebView2_19> webview19;
  if (FAILED(g_host.webview.As(&webview19)) || !webview19) return;

  const HRESULT hr = webview19->put_MemoryUsageTargetLevel(
      low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
          : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
  const int level = low ? 1 : 0;
  if (level != g_memLevel) {
    g_memLevel = level;
    LogLine(L"WEBHOST: 内存目标 %s hr=0x%08X", low ? L"LOW" : L"NORMAL",
            static_cast<unsigned>(hr));
  }
}

// 空闲时藏起画面并尝试挂起渲染器；借用时 Resume。挂起失败就只保持隐藏。
void SuspendIdle() {
  if (g_host.suspended || g_host.borrower || !g_host.webview3) return;
  if (g_host.controller) g_host.controller->put_IsVisible(FALSE);

  const HRESULT hr = g_host.webview3->TrySuspend(
      Callback<ICoreWebView2TrySuspendCompletedHandler>(
          [](HRESULT result, BOOL ok) -> HRESULT {
            LogLine(L"WEBHOST: TrySuspend hr=0x%08X ok=%d", static_cast<unsigned>(result),
                    ok ? 1 : 0);
            if (SUCCEEDED(result) && ok) {
              if (!g_host.borrower) {
                g_host.suspended = true;
              } else if (g_host.webview3) {
                // 挂起完成前已经被借走了：立刻恢复，别让画面停在冻结状态。
                g_host.webview3->Resume();
              }
            }
            return S_OK;
          })
          .Get());
  LogLine(L"WEBHOST: 请求挂起 hr=0x%08X", static_cast<unsigned>(hr));
}

void ResumeIfSuspended() {
  if (!g_host.suspended || !g_host.webview3) return;
  LARGE_INTEGER t0;
  LARGE_INTEGER t1;
  ::QueryPerformanceCounter(&t0);
  const HRESULT hr = g_host.webview3->Resume();
  ::QueryPerformanceCounter(&t1);
  g_host.suspended = false;
  LogLine(L"WEBHOST: Resume hr=0x%08X +%.1fms", static_cast<unsigned>(hr),
          QpcMs(t0.QuadPart, t1.QuadPart));
}

// ---- 页内搜索：后台线程 + 延迟响应 ----
//
// __search 要读整个文件（1 GB 约 0.6–0.9 s）。在 WebResourceRequested 回调里直接
// 扫会卡住整条宿主线程——WebView2 子窗口的输入消息也归它，搜一次就是一次假死。
// 所以请求回调只登记 deferral，扫描交给单独的搜索线程，扫完 PostMessage 回来再拼
// 响应。同一时刻只答最新一问：新请求到达时旧请求立即以空结果收尾（前端按 seq
// 丢弃），正在跑的扫描收到取消标志后尽早退出。

const UINT kSearchDone = WM_APP + 0x250;

struct SearchDone {
  uint64_t id = 0;
  int status = 200;
  std::string body;
};

struct PendingSearch {
  ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args;
  ComPtr<ICoreWebView2Deferral> deferral;
};

std::map<uint64_t, PendingSearch> g_searchWait;  // 只在宿主线程访问
uint64_t g_searchNextId = 1;

std::mutex g_searchMu;
std::condition_variable g_searchCv;
std::wstring g_searchReqUri;
uint64_t g_searchReqId = 0;
std::shared_ptr<std::atomic<bool>> g_searchCancel;  // 正在跑的扫描
bool g_searchQuit = false;
volatile LONG g_searchStarted = 0;

std::string EmptySearchFrame() { return std::string(12, '\0'); }

void CompleteSearch(PendingSearch& ps, const std::string& body, int status) {
  if (g_host.env && ps.args) {
    static const BYTE kEmpty = 0;
    const BYTE* data =
        body.empty() ? &kEmpty : reinterpret_cast<const BYTE*>(body.data());
    ComPtr<IStream> stream;
    stream.Attach(::SHCreateMemStream(data, static_cast<UINT>(body.size())));
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (stream &&
        SUCCEEDED(g_host.env->CreateWebResourceResponse(
            stream.Get(), status, L"OK",
            L"Content-Type: application/octet-stream\r\nCache-Control: no-store\r\n"
            L"Access-Control-Allow-Origin: https://mdprev.localhost\r\n",
            &response)) &&
        response) {
      ps.args->put_Response(response.Get());
    }
  }
  if (ps.deferral) ps.deferral->Complete();
}

// 把所有在等的请求以空结果收尾。先摘下来再 Complete：Complete 可能同步把控制权
// 交还渲染进程，别在遍历中改 map。
void DropPendingSearches() {
  std::vector<PendingSearch> wait;
  wait.reserve(g_searchWait.size());
  for (auto& kv : g_searchWait) wait.push_back(std::move(kv.second));
  g_searchWait.clear();
  for (PendingSearch& ps : wait) CompleteSearch(ps, EmptySearchFrame(), 200);
}

DWORD WINAPI SearchThreadProc(LPVOID) {
  for (;;) {
    std::wstring uri;
    uint64_t id = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
      std::unique_lock<std::mutex> lock(g_searchMu);
      g_searchCv.wait(lock, [] { return g_searchQuit || g_searchReqId != 0; });
      if (g_searchQuit) break;
      uri.swap(g_searchReqUri);
      id = g_searchReqId;
      g_searchReqId = 0;
      cancel = std::make_shared<std::atomic<bool>>(false);
      g_searchCancel = cancel;
    }

    std::string body;
    int status = 200;
    docsrc::SearchBody(uri, &body, &status, cancel.get());

    bool stale = false;
    {
      std::lock_guard<std::mutex> lock(g_searchMu);
      if (g_searchCancel == cancel) g_searchCancel.reset();
      stale = g_searchQuit || g_searchReqId != 0;
    }
    if (stale) continue;  // 已经有人顶替，或者正在退出

    SearchDone* done = new (std::nothrow) SearchDone();
    if (!done) continue;
    done->id = id;
    done->status = status;
    done->body.swap(body);
    HWND target = g_threadHost;
    if (!target || !::IsWindow(target) ||
        !::PostMessageW(target, kSearchDone, 0, reinterpret_cast<LPARAM>(done))) {
      delete done;
    }
  }
  return 0;
}

void EnsureSearchThread() {
  if (::InterlockedCompareExchange(&g_searchStarted, 1, 0) != 0) return;
  HANDLE h = ::CreateThread(nullptr, 0, SearchThreadProc, nullptr, 0, nullptr);
  if (h) {
    // 与宿主线程同寿命；Uninit 收尾时要等它退出，句柄不能丢。
    HANDLE old = ::InterlockedExchangePointer(reinterpret_cast<volatile PVOID*>(&g_hSearchThread),
                                              h);
    if (old) ::CloseHandle(old);
  } else {
    ::InterlockedExchange(&g_searchStarted, 0);
    LogLine(L"WEBHOST: 搜索线程创建失败 err=%lu", ::GetLastError());
  }
}

// 返回 false 表示拿不到 deferral，调用方退回同步扫描。
bool BeginAsyncSearch(const std::wstring& uri,
                      ICoreWebView2WebResourceRequestedEventArgs* args) {
  ComPtr<ICoreWebView2Deferral> deferral;
  if (!args || FAILED(args->GetDeferral(&deferral)) || !deferral) return false;

  // 先把旧请求摘干净再登记新请求，最后才 Complete：Complete 有可能同步回调起来
  // 新的请求，那时 g_searchWait 应该已经稳定。
  std::vector<PendingSearch> old;
  old.reserve(g_searchWait.size());
  for (auto& kv : g_searchWait) old.push_back(std::move(kv.second));
  g_searchWait.clear();

  const uint64_t id = g_searchNextId++;
  PendingSearch ps;
  ps.args = args;
  ps.deferral = deferral;
  g_searchWait[id] = ps;

  {
    std::lock_guard<std::mutex> lock(g_searchMu);
    g_searchReqUri = uri;
    g_searchReqId = id;
    if (g_searchCancel) g_searchCancel->store(true, std::memory_order_relaxed);
  }
  g_searchCv.notify_one();
  EnsureSearchThread();

  for (PendingSearch& prev : old) CompleteSearch(prev, EmptySearchFrame(), 200);
  return true;
}

// 释放实例/退出时调用：请求收尾、扫描停下。
void CancelSearches() {
  DropPendingSearches();
  {
    std::lock_guard<std::mutex> lock(g_searchMu);
    g_searchReqUri.clear();
    g_searchReqId = 0;
    if (g_searchCancel) g_searchCancel->store(true, std::memory_order_relaxed);
  }
}

void QuitSearchThread() {
  {
    std::lock_guard<std::mutex> lock(g_searchMu);
    g_searchQuit = true;
    if (g_searchCancel) g_searchCancel->store(true, std::memory_order_relaxed);
  }
  g_searchCv.notify_all();
}

// 预热关闭时，从最后一次预览结束起算空闲释放；预热开着就保持到 DO 窗口关闭。
void ArmIdleTimer() {
  if (!g_host.host) return;
  ::KillTimer(g_host.host, kTimerIdle);
  if (g_host.prewarmEnabled || g_host.idleReleaseMinutes <= 0) return;
  if (g_host.borrower) return;
  if (!g_host.controller && !g_host.env) return;
  ::SetTimer(g_host.host, kTimerIdle,
             static_cast<UINT>(g_host.idleReleaseMinutes) * 60000u, nullptr);
}

void NoteRoot(HWND root) {
  if (!root) return;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(root, &pid);
  if (pid != ::GetCurrentProcessId()) return;

  for (int i = 0; i < g_rootsCount; ++i) {
    if (g_roots[i] == root) return;
  }
  if (g_rootsCount < kMaxRoots) {
    g_roots[g_rootsCount++] = root;
    return;
  }
  for (int i = 1; i < kMaxRoots; ++i) g_roots[i - 1] = g_roots[i];
  g_roots[kMaxRoots - 1] = root;
}

void PruneRoots() {
  int keep = 0;
  for (int i = 0; i < g_rootsCount; ++i) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(g_roots[i], &pid);
    if (::IsWindow(g_roots[i]) && pid == ::GetCurrentProcessId()) {
      g_roots[keep++] = g_roots[i];
    }
  }
  g_rootsCount = keep;
}

struct WindowScan {
  DWORD pid;
  int count;
  std::wstring detail;
};

BOOL CALLBACK ScanWindowProc(HWND hwnd, LPARAM param) {
  WindowScan* scan = reinterpret_cast<WindowScan*>(param);
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (pid != scan->pid || !::IsWindowVisible(hwnd)) return TRUE;

  wchar_t cls[128] = L"";
  ::GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
  // 插件自己的窗口不算 DO 窗口：宿主、查看器、设置窗口都以 DopusMdViewer 开头。
  if (wcsncmp(cls, L"DopusMdViewer", 13) == 0) return TRUE;
  // 无标题的多半是 Opus 的辅助窗口，不作为“窗口还开着”的依据。
  if (::GetWindowTextLengthW(hwnd) == 0) return TRUE;

  ++scan->count;
  if (scan->detail.size() < 240) {
    wchar_t title[96] = L"";
    ::GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    if (!scan->detail.empty()) scan->detail += L"; ";
    scan->detail += cls;
    scan->detail += L" \"";
    scan->detail += title;
    scan->detail += L"\"";
  }
  return TRUE;
}

// ---- 宿主线程内部 ----

void ApplySettings(ICoreWebView2* webview) {
  ComPtr<ICoreWebView2Settings> settings;
  if (FAILED(webview->get_Settings(&settings)) || !settings) return;

  // 只读预览器：不要右键菜单里的「刷新/另存为」，不要状态栏，不要缩放控件。
  settings->put_AreDefaultContextMenusEnabled(FALSE);
  settings->put_AreDevToolsEnabled(FALSE);
  settings->put_IsStatusBarEnabled(FALSE);
  settings->put_IsZoomControlEnabled(FALSE);
  settings->put_IsBuiltInErrorPageEnabled(TRUE);
}

void ReleaseController() {
  // 在途的异步完成回调拿旧对象也进不来（世代不符直接作废），这里同步复位。
  ++g_gen;
  g_host.creating = false;
  // 先把在等的搜索请求收尾：controller 关掉后这些 deferral 就再也没有机会完成。
  CancelSearches();
  if (g_host.controller) {
    g_host.controller->Close();
    g_host.controller.Reset();
  }
  g_host.webview3.Reset();
  g_host.webview.Reset();
  g_host.ready = false;
  g_host.suspended = false;
  g_memLevel = -1;
  ::InterlockedExchange(&g_readyFlag, 0);
  LogLine(L"WEBHOST: controller 已释放");
}

// 关闭 controller 并回收 Environment：浏览器进程树随之退出。宿主窗口与线程保留，
// 下次借用时从 Environment 重新开始建（冷路径）。
void ReleaseInstance() {
  if (g_host.host) {
    ::KillTimer(g_host.host, kTimerWatch);
    ::KillTimer(g_host.host, kTimerPrewarm);
    ::KillTimer(g_host.host, kTimerIdle);
  }
  g_host.prewarmTimerArmed = false;
  ReleaseController();
  g_host.env.Reset();
  g_host.docFolder.clear();
  g_host.userDataDir.clear();
  g_host.watchEmpty = 0;
  g_host.everSawViewer = false;
  g_host.sawDopusWindow = false;
  g_rootsCount = 0;
  LogLine(L"WEBHOST: 实例已释放（Environment 也回收，下次借用重建）");
}

void CreateController();
void StartEnvironment(const std::wstring& runtimeDir, bool triedCustom);

// triedCustom 表示这一轮用的是用户自放的运行时；它建不起来时不能直接判死——
// 放错一个目录不该让整个预览不可用，回退系统运行时再试一次。
void OnEnvironmentReady(HRESULT hr, ICoreWebView2Environment* env, bool triedCustom,
                        uint64_t gen) {
  // 实例在这轮建环境期间被回收（空闲释放/自愈/WatchTick）：本次回调整体作废，
  // env 由回调自身释放，不能赋回 g_host。
  if (gen != g_gen) return;

  if (FAILED(hr) || !env) {
    if (triedCustom) {
      LogLine(L"WEBHOST: 自定义运行时不可用 hr=0x%08X，回退系统运行时",
              static_cast<unsigned>(hr));
      ::InterlockedExchange(&g_runtimeFallback, 1);
      // 第二次是系统运行时，triedCustom=false；再失败直接走 Fail，不能再回退。
      StartEnvironment(std::wstring(), false);
      return;
    }
    g_host.creating = false;
    wchar_t szMsg[160];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg),
                       L"WebView2 运行时不可用 hr=0x%08X（未安装或版本过低）",
                       static_cast<unsigned>(hr));
    Fail(szMsg);
    return;
  }
  g_host.env = env;
  LARGE_INTEGER now;
  ::QueryPerformanceCounter(&now);
  LogLine(L"WEBHOST: Environment 就绪 +%.1fms", QpcMs(g_host.tEnsure.QuadPart, now.QuadPart));

  // 浏览器进程整个没了：controller 一定也跟着失效，只能回收到 Environment 重建。
  ComPtr<ICoreWebView2Environment5> env5;
  if (SUCCEEDED(env->QueryInterface(IID_ICoreWebView2Environment5,
                                    reinterpret_cast<void**>(env5.GetAddressOf()))) &&
      env5) {
    EventRegistrationToken exitToken = {};
    const HRESULT exitHr = env5->add_BrowserProcessExited(
        Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
            [](ICoreWebView2Environment* sender,
               ICoreWebView2BrowserProcessExitedEventArgs* args) -> HRESULT {
              (void)sender;
              (void)args;
              LogLine(L"WEBHOST: 浏览器进程已退出");
              if (g_host.host) ::PostMessageW(g_host.host, kCmdRecover, 1, 0);
              return S_OK;
            })
            .Get(),
        &exitToken);
    if (FAILED(exitHr)) LogLine(L"WEBHOST: 注册浏览器进程退出事件失败 hr=0x%08X",
                                static_cast<unsigned>(exitHr));
  }

  CreateController();
}

void CreateController() {
  if (!g_host.host || !g_host.env) return;
  g_host.creating = true;
  ::QueryPerformanceCounter(&g_host.tController);
  const uint64_t gen = g_gen;

  const HRESULT hr = g_host.env->CreateCoreWebView2Controller(
      g_host.host,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [gen](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            // 释放发生在建 controller 期间：本次回调作废，controller 原地释放。
            if (gen != g_gen) {
              if (controller) controller->Close();
              return S_OK;
            }
            g_host.creating = false;
            if (FAILED(result) || !controller) {
              wchar_t szMsg[160];
              ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg), L"controller 创建失败 hr=0x%08X",
                                 static_cast<unsigned>(result));
              Fail(szMsg);
              return S_OK;
            }

            g_host.controller = controller;
            ::SetTimer(g_host.host, kTimerWatch, kWatchIntervalMs, nullptr);

            LARGE_INTEGER tReady;
            ::QueryPerformanceCounter(&tReady);
            LogLine(L"WEBHOST: controller 回调 +%.1fms",
                    QpcMs(g_host.tController.QuadPart, tReady.QuadPart));

            if (FAILED(controller->get_CoreWebView2(&g_host.webview)) || !g_host.webview) {
              Fail(L"get_CoreWebView2 失败");
              return S_OK;
            }

            ApplySettings(g_host.webview.Get());

            // 渲染进程崩溃时页面会停在内置错误页，界面不会自己回来。
            EventRegistrationToken failedToken = {};
            const HRESULT failHr = g_host.webview->add_ProcessFailed(
                Callback<ICoreWebView2ProcessFailedEventHandler>(
                    [](ICoreWebView2* sender,
                       ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                      (void)sender;
                      COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                          COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                      args->get_ProcessFailedKind(&kind);
                      LogLine(L"WEBHOST: 进程失效 kind=%d", static_cast<int>(kind));
                      switch (kind) {
                        case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
                        case COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED:
                          // 就地重载比整份实例重建便宜，页面会重新发 ready。
                          g_host.ready = false;
                          ::InterlockedExchange(&g_readyFlag, 0);
                          if (g_host.host) ::PostMessageW(g_host.host, kCmdRecover, 0, 0);
                          break;
                        case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
                          if (g_host.host) ::PostMessageW(g_host.host, kCmdRecover, 1, 0);
                          break;
                        default:
                          // GPU / utility / 沙箱助手由 Chromium 自己重启，不用插手。
                          break;
                      }
                      return S_OK;
                    })
                    .Get(),
                &failedToken);
            if (FAILED(failHr)) LogLine(L"WEBHOST: 注册进程失效事件失败 hr=0x%08X",
                                        static_cast<unsigned>(failHr));

            // 虚拟主机：app 资源。DENY_CORS 让同源可用、跨源 fetch 被拒。
            ComPtr<ICoreWebView2_3> webview3;
            if (SUCCEEDED(g_host.webview.As(&webview3)) && webview3) {
              g_host.webview3 = webview3;
              const std::wstring webDir = WebDir();
              const HRESULT mapHr = webview3->SetVirtualHostNameToFolderMapping(
                  kAppHost, webDir.c_str(),
                  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
              LogLine(L"WEBHOST: 映射 %s → %s hr=0x%08X", kAppHost, webDir.c_str(),
                      static_cast<unsigned>(mapHr));
            } else {
              Fail(L"拿不到 ICoreWebView2_3，虚拟主机不可用");
              return S_OK;
            }

            // 块正文走一个没有被映射的主机名：映射过的请求不会进 WebResourceRequested。
            const HRESULT filterHr = g_host.webview->AddWebResourceRequestedFilter(
                L"https://mdblocks.localhost/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            LogLine(L"WEBHOST: 块请求过滤器 hr=0x%08X", static_cast<unsigned>(filterHr));

            EventRegistrationToken resToken = {};
            g_host.webview->add_WebResourceRequested(
                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                    [](ICoreWebView2* sender,
                       ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                      (void)sender;
                      ComPtr<ICoreWebView2WebResourceRequest> request;
                      if (FAILED(args->get_Request(&request)) || !request) return S_OK;

                      LPWSTR rawUri = nullptr;
                      if (FAILED(request->get_Uri(&rawUri)) || !rawUri) return S_OK;
                      const std::wstring uri(rawUri);
                      ::CoTaskMemFree(rawUri);

                      // 搜索要读全文，不能在宿主线程上算。
                      if (docsrc::IsSearchUri(uri)) {
                        if (BeginAsyncSearch(uri, args)) {
                          LogLine(L"WEBHOST: __search 转后台 %s", uri.c_str());
                          return S_OK;
                        }
                        std::string body;
                        int status = 200;
                        docsrc::SearchBody(uri, &body, &status, nullptr);
                        PendingSearch ps;
                        ps.args = args;
                        CompleteSearch(ps, body, status);
                        LogLine(L"WEBHOST: __search 同步兜底 status=%d", status);
                        return S_OK;
                      }

                      std::string body;
                      int status = 404;
                      std::wstring contentType = L"application/octet-stream";
                      // 404 也必须给响应：不响应会让渲染进程一直等到超时
                      // （页面侧看到的是一句没头没尾的 "Failed to fetch"）。
                      if (!docsrc::ServeAsset(uri, &body, &status, &contentType)) {
                        status = 404;
                        // 未匹配的路径也要回一个空响应：只 return 会让请求挂到超时。
                        if (!docsrc::ServeOutline(uri, &body, &status) &&
                            !docsrc::ServeLines(uri, &body, &status) &&
                            !docsrc::ServeDefs(uri, &body, &status) &&
                            !docsrc::Serve(uri, &body, &status)) {
                          status = 404;
                          body.clear();
                        }
                      }

                      if (status >= 400) {
                        LogLine(L"WEBHOST: 资源请求失败 %s -> %d/%zu B", uri.c_str(), status,
                                body.size());
                      }

                      static const BYTE kEmpty = 0;
                      const BYTE* data =
                          body.empty() ? &kEmpty
                                       : reinterpret_cast<const BYTE*>(body.data());
                      ComPtr<IStream> stream;
                      stream.Attach(::SHCreateMemStream(data, static_cast<UINT>(body.size())));

                      const std::wstring headers =
                          L"Content-Type: " + contentType + L"\r\n" +
                          L"Cache-Control: no-store\r\n" +
                          L"Access-Control-Allow-Origin: " + kAppOrigin + L"\r\n";

                      ComPtr<ICoreWebView2WebResourceResponse> response;
                      if (g_host.env && stream) {
                        g_host.env->CreateWebResourceResponse(stream.Get(), status, L"OK",
                                                              headers.c_str(), &response);
                        args->put_Response(response.Get());
                      }
                      return S_OK;
                    })
                    .Get(),
                &resToken);

            EventRegistrationToken msgToken = {};
            g_host.webview->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [](ICoreWebView2* sender,
                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                      (void)sender;
                      LPWSTR raw = nullptr;
                      if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;

                      const std::wstring frame(raw);
                      ::CoTaskMemFree(raw);

                      if (frame == L"ready" || frame.rfind(L"ready|", 0) == 0) {
                        g_host.ready = true;
                        ::InterlockedExchange(&g_readyFlag, 1);
                        LARGE_INTEGER now;
                        ::QueryPerformanceCounter(&now);
                        const std::wstring perf =
                            frame.size() > 6 ? frame.substr(6) : std::wstring();
                        LogLine(L"WEBHOST: 页面已就绪 +%.1fms（导航起）%s",
                                QpcMs(g_host.tNav.QuadPart, now.QuadPart), perf.c_str());
                        if (g_host.borrower && g_host.host) {
                          // 冷启动时宿主一直藏着，让查看器先画“正在加载”；页面好了再露出来。
                          ::ShowWindow(g_host.host, SW_SHOW);
                          ::RedrawWindow(g_host.host, nullptr, nullptr,
                                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW |
                                             RDW_ERASE);
                          SetMemoryLevel(false);
                        } else if (g_host.prewarmEnabled) {
                          // 预热实例要一直保持可用：挂起后首次借用得唤醒渲染器、
                          // 重新布局并重推文档，省下的冷启动时间会全部还回去。
                          SetMemoryLevel(false);
                          ResumeIfSuspended();
                          if (g_host.controller) g_host.controller->put_IsVisible(TRUE);
                        } else {
                          SetMemoryLevel(true);
                          SuspendIdle();
                          ArmIdleTimer();
                        }
                        PostEvent(MDV_EVT_READY, L"");
                      } else {
                        PostEvent(MDV_EVT_MESSAGE, frame.c_str());
                      }
                      return S_OK;
                    })
                    .Get(),
                &msgToken);

            EventRegistrationToken navToken = {};
            g_host.webview->add_NavigationStarting(
                Callback<ICoreWebView2NavigationStartingEventHandler>(
                    [](ICoreWebView2* sender,
                       ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                      (void)sender;
                      (void)args;
                      LARGE_INTEGER now;
                      ::QueryPerformanceCounter(&now);
                      LogLine(L"WEBHOST: NavigationStarting +%.1fms",
                              QpcMs(g_host.tNav.QuadPart, now.QuadPart));
                      return S_OK;
                    })
                    .Get(),
                &navToken);

            EventRegistrationToken navDoneToken = {};
            g_host.webview->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [](ICoreWebView2* sender,
                       ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                      (void)sender;
                      BOOL ok = FALSE;
                      args->get_IsSuccess(&ok);
                      LARGE_INTEGER now;
                      ::QueryPerformanceCounter(&now);
                      LogLine(L"WEBHOST: NavigationCompleted ok=%d +%.1fms", ok ? 1 : 0,
                              QpcMs(g_host.tNav.QuadPart, now.QuadPart));
                      if (!ok) Fail(L"shell.html 导航失败");
                      return S_OK;
                    })
                    .Get(),
                &navDoneToken);

            RECT rc = {0, 0, 0, 0};
            if (g_host.host) ::GetClientRect(g_host.host, &rc);
            controller->put_Bounds(rc);
            controller->put_IsVisible(TRUE);

            ::QueryPerformanceCounter(&g_host.tNav);
            LogLine(L"WEBHOST: controller 就绪 +%.1fms，导航到 %s",
                    QpcMs(g_host.tController.QuadPart, g_host.tNav.QuadPart), kShellUrl);
            g_host.webview->Navigate(kShellUrl);
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    g_host.creating = false;
    wchar_t szMsg[128];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg), L"CreateCoreWebView2Controller 失败 hr=0x%08X",
                       static_cast<unsigned>(hr));
    Fail(szMsg);
  }
}

// 建 Environment。runtimeDir 为空表示用系统 Evergreen；非空是用户自放的运行时目录。
// 同步返回 HRESULT 与回调里的失败走同一条路：自定义那份失败就回退系统再试一次。
void StartEnvironment(const std::wstring& runtimeDir, bool triedCustom) {
  g_host.creating = true;
  if (!triedCustom) ::QueryPerformanceCounter(&g_host.tEnsure);
  const uint64_t gen = g_gen;

  const std::wstring userData = UserDataDir();
  g_host.userDataDir = userData;
  LogLine(L"WEBHOST: 建 Environment（运行时=%s）",
          runtimeDir.empty() ? L"系统 Evergreen" : runtimeDir.c_str());

  const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
      runtimeDir.empty() ? nullptr : runtimeDir.c_str(),
      userData.empty() ? nullptr : userData.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [triedCustom, gen](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            OnEnvironmentReady(result, env, triedCustom, gen);
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) OnEnvironmentReady(hr, nullptr, triedCustom, gen);
}

void EnsureEnvironment() {
  if (g_cleanupBusy) {
    g_envQueuedAfterCleanup = true;
    return;
  }
  if (g_host.env) {
    if (!g_host.controller && !g_host.creating) CreateController();
    return;
  }
  if (g_host.creating) return;

  StartCacheCleanAsync();
  if (g_cleanupBusy) {
    g_envQueuedAfterCleanup = true;
    return;
  }

  const std::wstring& runtimeDir = UserRuntimeDir();
  StartEnvironment(runtimeDir, !runtimeDir.empty());
}

// 自愈：进程失效后把画面救回来。渲染进程死掉只重载页面（页面重新发 ready，借用方
// 按原流程重推皮肤与索引）；浏览器进程死掉则回收实例，从 Environment 重建。
// 一分钟内连续恢复超过预算就停手——再救下去只是让窗格反复闪白，不如报一句失败。
bool RecoveryAllowed() {
  LARGE_INTEGER now;
  ::QueryPerformanceCounter(&now);
  const double elapsed = QpcMs(g_host.lastRecover.QuadPart, now.QuadPart);
  if (!g_host.lastRecover.QuadPart || elapsed > static_cast<double>(kRecoverWindowMs)) {
    g_host.recoverCount = 0;
  }
  if (g_host.recoverCount >= kRecoverBudget) return false;
  g_host.lastRecover = now;
  ++g_host.recoverCount;
  LogLine(L"WEBHOST: 第 %d 次恢复（距上次 %.0fms）", g_host.recoverCount, elapsed);
  return true;
}

void RecoverInstance() {
  LogLine(L"WEBHOST: 回收实例后重建");
  ReleaseInstance();
  EnsureEnvironment();
}

void RecoverPage() {
  if (!g_host.webview || !g_host.controller) {
    RecoverInstance();
    return;
  }
  ResumeIfSuspended();
  if (g_host.controller) g_host.controller->put_IsVisible(TRUE);
  ::QueryPerformanceCounter(&g_host.tNav);
  const HRESULT hr = g_host.webview->Reload();
  LogLine(L"WEBHOST: 重载页面 hr=0x%08X", static_cast<unsigned>(hr));
  // 渲染进程还没退干净时 Reload 会失败，退回到整份实例重建这条确定的路。
  if (FAILED(hr)) RecoverInstance();
}

void SetChildStyle(bool child) {
  if (!g_host.host) return;
  LONG_PTR style = ::GetWindowLongPtrW(g_host.host, GWL_STYLE);
  if (child) {
    style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
  } else {
    style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
  }
  ::SetWindowLongPtrW(g_host.host, GWL_STYLE, style);
}

// 把宿主塞进 viewer 的客户区。别人正在借用时先通知它「宿主被拿走了」。
bool BorrowHost(HWND viewer, const RECT* rc) {
  if (!g_host.host || !viewer || !::IsWindow(viewer)) return false;

  RECT rcClient = {0, 0, 0, 0};
  if (rc) {
    rcClient = *rc;
  } else {
    ::GetClientRect(viewer, &rcClient);
  }

  HWND prev = g_host.borrower;
  if (prev && prev != viewer && ::IsWindow(prev)) {
    LogLine(L"WEBHOST: 宿主改借 %p（原 %p 转空白）", viewer, prev);
    ::PostMessageW(prev, WM_MDV_HOST_LOST, 0, 0);
  } else {
    LogLine(L"WEBHOST: 借用 → viewer=%p size=%ldx%ld", viewer, rcClient.right - rcClient.left,
            rcClient.bottom - rcClient.top);
  }

  HistoryPush(viewer);
  g_host.borrower = viewer;
  g_host.borrowed = true;
  g_host.everSawViewer = true;
  g_host.sawDopusWindow = true;
  NoteRoot(::GetAncestor(viewer, GA_ROOT));
  ::KillTimer(g_host.host, kTimerIdle);

  SetChildStyle(true);
  ::SetParent(g_host.host, viewer);
  ::SetWindowPos(g_host.host, nullptr, 0, 0, rcClient.right - rcClient.left,
                 rcClient.bottom - rcClient.top, SWP_NOZORDER | SWP_FRAMECHANGED);
  // 页面还没就绪时先别露宿主，让查看器窗口里的“正在加载”占位可见。
  ::ShowWindow(g_host.host, g_host.ready ? SW_SHOW : SW_HIDE);

  if (g_host.controller) g_host.controller->put_Bounds(rcClient);
  ResumeIfSuspended();
  if (g_host.controller) g_host.controller->put_IsVisible(TRUE);
  SetMemoryLevel(false);
  // 宿主可能刚被藏过一阵，强制重画一次，避免回来时是一片旧像素。
  if (g_host.ready) {
    ::RedrawWindow(g_host.host, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
  }
  // 空闲释放的定时器可能刚好在借用前把实例回收了：这里补一次 ensure。
  EnsureEnvironment();
  return true;
}

void UnborrowHost(HWND viewer) {
  if (!g_host.host) return;

  HistoryDrop(viewer);

  if (g_host.borrower != viewer) {
    // 已经被人抢走了，这里只把自己从历史里摘掉，不能动别人的宿主。
    LogLine(L"WEBHOST: 归还忽略（%p 已不是借用方）", viewer);
    return;
  }

  g_host.borrower = nullptr;
  g_host.borrowed = false;
  ::ShowWindow(g_host.host, SW_HIDE);
  ::SetParent(g_host.host, nullptr);
  SetChildStyle(false);
  ::SetWindowPos(g_host.host, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_FRAMECHANGED);
  if (g_host.prewarmEnabled) {
    // 预热开着时 controller 一直留着，归还也不挂起：换窗口只需重新 SetParent。
    SetMemoryLevel(false);
  } else {
    SetMemoryLevel(true);
    SuspendIdle();
  }

  HWND next = HistoryFirstLive(viewer);
  if (!next) ArmIdleTimer();
  LogLine(L"WEBHOST: 归还宿主（controller 保留）%s%p", next ? L"，交回 " : L"", next);
  if (next) ::PostMessageW(next, WM_MDV_RECLAIM, 0, 0);
}

// 预热：宿主隐藏时先用一个正常尺寸把页面布局好，借用时再改成查看器大小。
void StartPrewarm() {
  if (!g_host.host || g_host.controller || g_host.creating || g_host.borrower) return;
  ::SetWindowPos(g_host.host, nullptr, 0, 0, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
  LogLine(L"WEBHOST: 预热开始");
  EnsureEnvironment();
}

void SchedulePrewarm(UINT delayMs) {
  if (!g_host.host || !g_host.prewarmEnabled) return;
  if (g_host.controller || g_host.creating || g_host.borrower) return;
  if (g_host.prewarmTimerArmed) return;
  ::SetTimer(g_host.host, kTimerPrewarm, delayMs, nullptr);
  g_host.prewarmTimerArmed = true;
}

// 每 2 秒看一次：本进程还有没有 DO 的可见顶层窗口。连续两次都没有才释放。
void WatchTick() {
  if (!g_host.host) return;
  if (g_host.borrower || (!g_host.controller && !g_host.env)) {
    g_host.watchEmpty = 0;
    return;
  }

  PruneRoots();
  WindowScan scan = {::GetCurrentProcessId(), 0, std::wstring()};
  ::EnumWindows(ScanWindowProc, reinterpret_cast<LPARAM>(&scan));
  if (scan.count > 0) g_host.sawDopusWindow = true;

  const bool noWindows = scan.count == 0;
  const bool gone = noWindows && (g_host.everSawViewer ? g_rootsCount == 0
                                                       : g_host.sawDopusWindow);
  if (!gone) {
    g_host.watchEmpty = 0;
    return;
  }
  if (++g_host.watchEmpty < kWatchEmptyLimit) return;

  LogLine(L"WEBHOST: DO 窗口已关闭（活跃根 %d 个，可见顶层 %d 个 %s），释放 WebView2",
          g_rootsCount, scan.count, scan.detail.c_str());
  ReleaseInstance();
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case kCmdEnsure:
      if (g_host.host) ::KillTimer(g_host.host, kTimerPrewarm);
      g_host.prewarmTimerArmed = false;
      EnsureEnvironment();
      return 0;

    case kCmdPrewarm: {
      g_host.prewarmEnabled = wParam != 0;
      int minutes = static_cast<int>(lParam);
      if (minutes < 0) minutes = 0;
      if (minutes > 1440) minutes = 1440;
      g_host.idleReleaseMinutes = minutes;
      ::KillTimer(hwnd, kTimerIdle);
      if (g_host.prewarmEnabled) {
        if (g_host.controller || g_host.env || g_host.creating) {
          // 实例已在建或已就绪：只把可能挂着的实例唤醒，不重排定时器。
          ResumeIfSuspended();
          SetMemoryLevel(false);
          if (g_host.controller) g_host.controller->put_IsVisible(TRUE);
          EnsureEnvironment();  // env 在但 controller 没建成时补一次
          LogLine(L"WEBHOST: 预热开启，已有实例保持活跃");
        } else {
          // Opus 刷新插件列表会反复送同一条命令；已在等的定时器不许被一直往后推。
          SchedulePrewarm(kPrewarmDelayMs);
        }
      } else {
        ::KillTimer(hwnd, kTimerPrewarm);
        g_host.prewarmTimerArmed = false;
        ArmIdleTimer();
      }
      return 1;
    }

    case WM_TIMER:
      if (wParam == kTimerPrewarm) {
        ::KillTimer(hwnd, kTimerPrewarm);
        g_host.prewarmTimerArmed = false;
        StartPrewarm();
        return 0;
      }
      if (wParam == kTimerWatch) {
        WatchTick();
        return 0;
      }
      if (wParam == kTimerIdle) {
        ::KillTimer(hwnd, kTimerIdle);
        if (g_host.prewarmEnabled || g_host.borrower) return 0;
        if (g_host.controller || g_host.env) {
          LogLine(L"WEBHOST: 预热关闭，空闲 %d 分钟，释放 WebView2",
                  g_host.idleReleaseMinutes);
          ReleaseInstance();
        }
        return 0;
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    case kCmdBorrow:
      return BorrowHost(reinterpret_cast<HWND>(wParam),
                        reinterpret_cast<const RECT*>(lParam))
                 ? 1
                 : 0;

    case kCmdUnborrow:
      UnborrowHost(reinterpret_cast<HWND>(wParam));
      return 1;

    case kCmdBounds: {
      const RECT* rc = reinterpret_cast<const RECT*>(lParam);
      if (!rc || !g_host.host) return 0;
      if (g_host.borrower != reinterpret_cast<HWND>(wParam)) return 0;
      ::SetWindowPos(g_host.host, nullptr, 0, 0, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      if (g_host.controller) g_host.controller->put_Bounds(*rc);
      return 1;
    }

    case kCmdPost: {
      const std::wstring* text = reinterpret_cast<const std::wstring*>(lParam);
      if (!text || !g_host.webview || !g_host.ready) return 0;
      if (g_host.borrower != reinterpret_cast<HWND>(wParam)) {
        // 宿主在别人手里：投递出去只会画进那个看不见的宿主窗口。
        LogLine(L"WEBHOST: 拒绝投递（%p 不是借用方，当前 %p）",
                reinterpret_cast<HWND>(wParam), g_host.borrower);
        return 0;
      }

      const HRESULT hr = g_host.webview->PostWebMessageAsString(text->c_str());
      if (FAILED(hr)) {
        LogLine(L"WEBHOST: PostWebMessageAsString 失败 hr=0x%08X（%zu 字符）",
                static_cast<unsigned>(hr), text->size());
        // 0x8007139F = RO_E_CLOSED：controller 或它的父窗口已经没了。
        if (static_cast<unsigned>(hr) == 0x8007139Fu) {
          LogLine(L"WEBHOST: controller 已失效，重置后等下次借用重建");
          ReleaseController();
        }
        return 0;
      }
      return 1;
    }

    case kCmdDocFolder: {
      const std::wstring* dir = reinterpret_cast<const std::wstring*>(lParam);
      if (!dir) return 0;
      // 与其它命令一样认借用方：不是当前借用者就不能改映射，否则会悄悄把
      // 正在显示的那个窗口的文档目录换成别人的。
      if (g_host.borrower != reinterpret_cast<HWND>(wParam)) {
        LogLine(L"WEBHOST: 拒绝设置文档目录（%p 不是借用方）", reinterpret_cast<HWND>(wParam));
        return 0;
      }
      g_host.docFolder = *dir;
      if (!g_host.webview3) return 0;

      if (dir->empty()) {
        g_host.webview3->ClearVirtualHostNameToFolderMapping(kDocHost);
        LogLine(L"WEBHOST: 取消 %s 映射", kDocHost);
        return 1;
      }
      // 图片已改走 mdblocks.localhost/__asset：虚拟主机映射出来的跨源 <img> 会
      // 直接 onerror，这台映射留给后续需要同源读取的场景，仍按 DENY_CORS 收口。
      const HRESULT hr = g_host.webview3->SetVirtualHostNameToFolderMapping(
          kDocHost, dir->c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
      LogLine(L"WEBHOST: 映射 %s → %s hr=0x%08X", kDocHost, dir->c_str(),
              static_cast<unsigned>(hr));
      return SUCCEEDED(hr) ? 1 : 0;
    }

    case kCmdRelease:
      ReleaseController();
      return 1;

    case kCmdClearUserData: {
      // 目录本身的删除由调用线程做（WebHostClearUserData），宿主线程只负责释放
      // 正在使用的实例，最多一两毫秒，不做 Sleep 重试。
      const std::wstring* target = reinterpret_cast<const std::wstring*>(lParam);
      if (!target || !UtilIsWebView2UserDataDir(*target)) return 0;

      const bool active = !g_host.userDataDir.empty() &&
                          _wcsicmp(g_host.userDataDir.c_str(), target->c_str()) == 0;
      if (active) {
        if (g_host.borrower || g_host.creating) return 0;
        ReleaseInstance();
      }
      return 1;
    }

    case kCmdCleanupDone:
      ::InterlockedExchange(&g_cleanupBusy, 0);
      if (g_envQueuedAfterCleanup) {
        g_envQueuedAfterCleanup = false;
        EnsureEnvironment();
      }
      return 0;

    case kCmdPrint: {
      if (g_host.borrower != reinterpret_cast<HWND>(wParam)) return 0;
      if (!g_host.webview) return 0;
      ComPtr<ICoreWebView2_16> wv16;
      if (FAILED(g_host.webview.As(&wv16)) || !wv16) {
        LogLine(L"WEBHOST: 运行时没有 ICoreWebView2_16，打印不可用");
        return 0;
      }
      const HRESULT hr = wv16->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_BROWSER);
      LogLine(L"WEBHOST: ShowPrintUI hr=0x%08X", static_cast<unsigned>(hr));
      return SUCCEEDED(hr) ? 1 : 0;
    }

    case kCmdRecover: {
      // 实例已经被回收过就什么都不用做：POST 可能来两次（ProcessFailed 与
      // BrowserProcessExited 都会报浏览器进程退出）。但还有查看器在借用时不能
      // 直接丢掉——env/webview 为空说明实例刚被收走，得整份重建。
      // 借用方还在时下面两个短路都不成立，所以还得靠时间窗去重，否则同一场崩溃
      // 会重建两次、多扣一次自愈预算。
      LARGE_INTEGER now;
      ::QueryPerformanceCounter(&now);
      if (g_host.recoverAcceptedAt.QuadPart &&
          QpcMs(g_host.recoverAcceptedAt.QuadPart, now.QuadPart) < 1500.0) {
        LogLine(L"WEBHOST: 1.5 秒内已受理过一次自愈，忽略重复的恢复请求");
        return 0;
      }
      const bool full = wParam != 0;
      if (full && !g_host.env && !g_host.borrower) return 0;
      if (!full && !g_host.webview && !g_host.borrower) return 0;
      if (!RecoveryAllowed()) {
        LogLine(L"WEBHOST: 恢复次数超预算，停止自愈");
        Fail(L"预览引擎反复崩溃，已停止自动恢复（重启 Opus 后再试）");
        return 0;
      }
      g_host.recoverAcceptedAt = now;
      PostEvent(MDV_EVT_RECOVERING, L"");
      if (full) {
        RecoverInstance();
      } else {
        RecoverPage();
      }
      return 1;
    }

    case kCmdScript: {
      const std::wstring* script = reinterpret_cast<const std::wstring*>(lParam);
      if (!script || !g_host.webview || !g_host.ready) return 0;
      if (g_host.borrower != reinterpret_cast<HWND>(wParam)) return 0;
      return SUCCEEDED(g_host.webview->ExecuteScript(script->c_str(), nullptr)) ? 1 : 0;
    }

    case kCmdDevTools: {
      const std::wstring* method = reinterpret_cast<const std::wstring*>(lParam);
      if (!method || !g_host.webview) return 0;
      const HRESULT hr = g_host.webview->CallDevToolsProtocolMethod(
          method->c_str(), L"{}",
          Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
              [](HRESULT result, LPCWSTR json) -> HRESULT {
                LogLine(L"WEBHOST: DevTools 返回 hr=0x%08X %s", static_cast<unsigned>(result),
                        json ? json : L"");
                return S_OK;
              })
              .Get());
      LogLine(L"WEBHOST: DevTools 调用 %s hr=0x%08X", method->c_str(),
              static_cast<unsigned>(hr));
      return SUCCEEDED(hr) ? 1 : 0;
    }

    case kSearchDone: {
      std::unique_ptr<SearchDone> done(reinterpret_cast<SearchDone*>(lParam));
      if (!done) return 0;
      auto it = g_searchWait.find(done->id);
      if (it == g_searchWait.end()) return 0;  // 已被更新的查询顶替
      PendingSearch ps = std::move(it->second);
      g_searchWait.erase(it);
      CompleteSearch(ps, done->body, done->status);
      return 0;
    }

    case WM_NCDESTROY:
      QuitSearchThread();
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      ReleaseInstance();
      g_host.host = nullptr;
      g_threadHost = nullptr;
      LogLine(L"WEBHOST: 宿主窗口被销毁");
      // 宿主窗口只在收尾时销毁：让线程的消息泵跟着退出，Uninit 才能等线程落地。
      ::PostQuitMessage(0);
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

// 宿主线程：建窗口、跑消息泵，进程结束时才退。
DWORD WINAPI HostThreadProc(LPVOID param) {
  HANDLE readyEvent = static_cast<HANDLE>(param);

  // 宿主线程可能重建（插件重新加载）：搜索线程的启停标志要跟着复位，
  // 否则 g_searchStarted 还是 1，__search 请求就永远没人接、deferral 也不会收尾。
  g_searchQuit = false;
  ::InterlockedExchange(&g_searchStarted, 0);

  // Opus 的 UI 线程多半已是 STA；这里在自己的线程上重新初始化一次。
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const HMODULE self = ThisModule();
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HostWndProc;
  wc.hInstance = self;
  wc.lpszClassName = kHostClass;

  if (!::RegisterClassExW(&wc)) {
    const DWORD err = ::GetLastError();
    if (err != ERROR_CLASS_ALREADY_EXISTS) {
      LogLine(L"WEBHOST: RegisterClassExW 失败 err=%lu", err);
      ::InterlockedExchange(&g_threadFailed, 1);
      ::SetEvent(readyEvent);
      return 1;
    }
    // 同名类还在系统里：本模块上一次加载注册的（线程只建一次，不该发生）。
    // 留着它就会用旧的 WndProc，先注销再注册一次；注销也失败就不建窗口。
    LogLine(L"WEBHOST: 宿主窗口类已存在，尝试注销后重注册");
    ::UnregisterClassW(kHostClass, self);
    if (!::RegisterClassExW(&wc)) {
      LogLine(L"WEBHOST: 重注册宿主窗口类失败 err=%lu", ::GetLastError());
      ::InterlockedExchange(&g_threadFailed, 1);
      ::SetEvent(readyEvent);
      return 1;
    }
  }

  // WS_POPUP + 不可见：先游离在桌面上，被借用时改成 WS_CHILD 塞进查看器窗口。
  g_host.host = ::CreateWindowExW(WS_EX_NOACTIVATE, kHostClass, L"DopusMdViewerHost",
                                  WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr,
                                  self, nullptr);
  g_host.prewarmEnabled = ::InterlockedCompareExchange(&g_cfgPrewarm, 0, 0) != 0;
  g_host.idleReleaseMinutes = ::InterlockedCompareExchange(&g_cfgIdleMinutes, 0, 0);
  g_threadHost = g_host.host;
  LogLine(L"WEBHOST: 宿主线程 %lu 就绪，窗口 %p（预热=%d，空闲释放=%d 分钟）",
          ::GetCurrentThreadId(), g_host.host, g_host.prewarmEnabled ? 1 : 0,
          g_host.idleReleaseMinutes);
  ::SetEvent(readyEvent);

  if (!g_host.host) {
    ::InterlockedExchange(&g_threadFailed, 1);
    return 1;
  }

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  ReleaseController();
  g_host.env.Reset();
  // 窗口已销毁，类可以注销了：不注销就会带着本模块的旧 lpfnWndProc 留在系统里，
  // 插件重新加载后注册同名类失败、拿旧类建窗口，消息跳到已卸载的代码上。
  ::UnregisterClassW(kHostClass, self);
  return 0;
}

// 建线程（只建一次）。返回宿主窗口句柄，失败返回 nullptr。
// 注意：并发的第二个调用者会跳过建线程直接等 readyEvent——宿主线程从建线程到
// 建好窗口有几秒窗口期，不等待的话借用方会拿到 null 然后永远空白。
HWND EnsureHostThread() {
  if (::InterlockedCompareExchange(&g_threadFailed, 0, 0) != 0) return nullptr;

  // 事件在 CAS 之前建好，后来者才能等到同一个事件。
  if (!g_readyEvent) {
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ev) {
      if (::InterlockedCompareExchangePointer(
              reinterpret_cast<volatile PVOID*>(&g_readyEvent), ev, nullptr) != nullptr) {
        ::CloseHandle(ev);  // 别的线程先建好了
      }
    }
  }
  if (!g_readyEvent) return nullptr;

  if (::InterlockedCompareExchange(&g_hasThread, 1, 0) == 0) {
    DWORD tid = 0;
    HANDLE hThread = ::CreateThread(nullptr, 0, HostThreadProc, g_readyEvent, 0, &tid);
    if (!hThread) {
      ::InterlockedExchange(&g_hasThread, 0);
      LogLine(L"WEBHOST: CreateThread 失败 err=%lu", ::GetLastError());
      return nullptr;
    }
    g_hHostThread = hThread;  // 与进程同寿命；Uninit 收尾时等它退出
    g_threadId = tid;
  }

  ::WaitForSingleObject(g_readyEvent, 5000);
  return g_threadHost;
}

// 向宿主线程发命令。宿主线程没建起来或已消失时返回 false。
bool SendCommand(UINT cmd, WPARAM wParam, LPARAM lParam, LRESULT* result = nullptr) {
  HWND host = EnsureHostThread();
  if (!host || !::IsWindow(host)) return false;
  const LRESULT r = ::SendMessageW(host, cmd, wParam, lParam);
  if (result) *result = r;
  return true;
}

}  // namespace

bool WebHostEnsure(void) { return SendCommand(kCmdEnsure, 0, 0); }

void WebHostPrewarm(bool enabled, int idleReleaseMinutes) {
  if (idleReleaseMinutes < 0) idleReleaseMinutes = 0;
  if (idleReleaseMinutes > 1440) idleReleaseMinutes = 1440;

  // 预热常驻时插件不能在这些线程还活着的时候被 FreeLibrary。自持一份引用，
  // 让 Opus 的 FreeLibrary 只是减一次计数；引用与进程同寿命，不配对释放。
  if (enabled && !g_pin) {
    HMODULE self = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(&HostThreadProc), &self)) {
      g_pin = self;
      LogLine(L"WEBHOST: 预热常驻，本模块自持一份引用，Uninit 不再收掉宿主线程");
    } else {
      LogLine(L"WEBHOST: 自持引用失败 err=%lu，Uninit 仍按原路径收尾", ::GetLastError());
    }
  }

  ::InterlockedExchange(&g_cfgPrewarm, enabled ? 1 : 0);
  ::InterlockedExchange(&g_cfgIdleMinutes, idleReleaseMinutes);
  // 关闭预热时如果宿主线程还没建起来，配置留在上面，等线程起来再套用。
  if (!enabled && ::InterlockedCompareExchange(&g_hasThread, 0, 0) == 0) return;
  SendCommand(kCmdPrewarm, enabled ? 1 : 0, static_cast<LPARAM>(idleReleaseMinutes));
}

bool WebHostIsReady(void) { return ::InterlockedCompareExchange(&g_readyFlag, 0, 0) != 0; }

bool WebHostIsPinned(void) { return g_pin != nullptr; }

bool WebHostClearUserData(const std::wstring& userDataDir) {
  if (!UtilIsWebView2UserDataDir(userDataDir)) return false;
  const DWORD attr = ::GetFileAttributesW(userDataDir.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
  LRESULT result = 0;
  SendCommand(kCmdClearUserData, 0, reinterpret_cast<LPARAM>(&userDataDir), &result);
  if (result == 0) return false;  // 目录正被本进程占用（借出中/建环境中）

  // 删除与重试在调用线程上做：宿主线程不能被 Sleep 循环占住，调用方（设置界面）
  // 本来就挂着等待光标同步等结果。
  bool ok = false;
  for (int i = 0; i < 30; ++i) {
    if (!UtilDirExists(userDataDir)) {
      ok = true;
      break;
    }
    if (UtilRemoveTree(userDataDir)) {
      ok = true;
      break;
    }
    ::Sleep(100);
  }
  if (ok) ok = UtilEnsureDir(userDataDir);
  if (ok) {
    LogLine(L"WEBHOST: 已清理缓存 %s", userDataDir.c_str());
  } else {
    LogLine(L"WEBHOST: 清理缓存失败（目录仍被占用）%s", userDataDir.c_str());
  }
  return ok;
}

bool WebHostPost(HWND hViewer, const std::wstring& text) {
  if (!hViewer || !WebHostIsReady()) return false;
  LRESULT result = 0;
  if (!SendCommand(kCmdPost, reinterpret_cast<WPARAM>(hViewer),
                   reinterpret_cast<LPARAM>(&text), &result)) {
    return false;
  }
  return result != 0;
}

bool WebHostBorrow(HWND hViewer, const RECT& client) {
  if (!hViewer) return false;
  LRESULT result = 0;
  if (!SendCommand(kCmdBorrow, reinterpret_cast<WPARAM>(hViewer),
                   reinterpret_cast<LPARAM>(&client), &result)) {
    return false;
  }
  return result != 0;
}

void WebHostUnborrow(HWND hViewer) {
  SendCommand(kCmdUnborrow, reinterpret_cast<WPARAM>(hViewer), 0);
}

bool WebHostSetBounds(HWND hViewer, const RECT& client) {
  if (!hViewer) return false;
  LRESULT result = 0;
  if (!SendCommand(kCmdBounds, reinterpret_cast<WPARAM>(hViewer),
                   reinterpret_cast<LPARAM>(&client), &result)) {
    return false;
  }
  return result != 0;
}

bool WebHostSetDocFolder(HWND hViewer, const std::wstring& dir) {
  if (!hViewer) return false;
  HWND host = EnsureHostThread();
  if (!host || !::IsWindow(host)) return false;
  return ::SendMessageW(host, kCmdDocFolder, reinterpret_cast<WPARAM>(hViewer),
                        reinterpret_cast<LPARAM>(&dir)) != 0;
}

void WebHostReleaseController(void) { SendCommand(kCmdRelease, 0, 0); }

bool WebHostShowPrintUI(HWND hViewer) {
  if (!hViewer) return false;
  LRESULT result = 0;
  if (!SendCommand(kCmdPrint, reinterpret_cast<WPARAM>(hViewer), 0, &result)) return false;
  return result != 0;
}

bool WebHostExecuteScript(HWND hViewer, const std::wstring& script) {
  if (!hViewer) return false;
  LRESULT result = 0;
  if (!SendCommand(kCmdScript, reinterpret_cast<WPARAM>(hViewer),
                   reinterpret_cast<LPARAM>(&script), &result)) {
    return false;
  }
  return result != 0;
}

bool WebHostCallDevTools(HWND hViewer, const std::wstring& method) {
  if (!hViewer) return false;
  LRESULT result = 0;
  if (!SendCommand(kCmdDevTools, reinterpret_cast<WPARAM>(hViewer),
                   reinterpret_cast<LPARAM>(&method), &result)) {
    return false;
  }
  return result != 0;
}

// 宿主窗口是否存在（不管在谁手里）。false = 宿主线程没起来或窗口已销毁。
bool WebHostHostExists(void) {
  HWND host = g_threadHost;
  return host && ::IsWindow(host);
}

// 插件 Uninit 时收尾：销毁宿主窗口（线程随之退出）并等待宿主/搜索线程落地。
// Opus 之后可能 FreeLibrary——线程还活着就会执行已卸载的代码，必须等干净。
// 等不到就不放行：标志与句柄全部留着，下次 Uninit 再等（放行才是真崩）。
void WebHostShutdown(void) {
  if (::InterlockedCompareExchange(&g_hasThread, 0, 0) == 0) return;
  LogLine(L"WEBHOST: 插件卸载，收掉宿主线程");

  const HWND host = g_threadHost;
  if (host && ::IsWindow(host)) ::PostMessageW(host, WM_CLOSE, 0, 0);

  bool clean = true;
  if (g_hHostThread) {
    if (::WaitForSingleObject(g_hHostThread, 5000) == WAIT_OBJECT_0) {
      ::CloseHandle(g_hHostThread);
      g_hHostThread = nullptr;
    } else {
      LogLine(L"WEBHOST: 宿主线程 5 秒内没退出，保留句柄与标志，下次卸载再等");
      clean = false;
    }
  }
  // 搜索线程在 WM_NCDESTROY 里收到退出标志，这里只等它跑完手头的扫描。
  if (g_hSearchThread) {
    if (::WaitForSingleObject(g_hSearchThread, 5000) == WAIT_OBJECT_0) {
      ::CloseHandle(g_hSearchThread);
      g_hSearchThread = nullptr;
    } else {
      LogLine(L"WEBHOST: 搜索线程 5 秒内没退出，保留句柄与标志，下次卸载再等");
      clean = false;
    }
  }
  // 缓存清理线程同样跑在 DLL 代码里，投递失败时还会自己写 g_cleanupBusy。
  // 宿主窗口已销毁，kCmdCleanupDone 可能永远送不到，join 之后顺手解锁，
  // 免得下次建环境时永远卡在 g_cleanupBusy 上。
  if (g_hCleanThread) {
    if (::WaitForSingleObject(g_hCleanThread, 5000) == WAIT_OBJECT_0) {
      ::CloseHandle(g_hCleanThread);
      g_hCleanThread = nullptr;
      ::InterlockedExchange(&g_cleanupBusy, 0);
    } else {
      LogLine(L"WEBHOST: 缓存清理线程 5 秒内没退出，保留句柄与标志，下次卸载再等");
      clean = false;
    }
  }
  if (!clean) return;

  ::InterlockedExchange(&g_hasThread, 0);
  ::InterlockedExchange(&g_threadFailed, 0);
  g_threadId = 0;
  g_threadHost = nullptr;
  if (g_readyEvent) ::ResetEvent(g_readyEvent);  // 下次重建时让等待者重新等
}

std::wstring WebHostRuntimeInfo(void) {
  const std::wstring& custom = UserRuntimeDir();
  if (!custom.empty() && !g_runtimeFallback) {
    const std::wstring version = UtilFileVersion(UtilJoin(custom, L"msedgewebview2.exe"));
    return version.empty() ? std::wstring(L"自定义（版本未知）") : L"自定义 " + version;
  }

  LPWSTR version = nullptr;
  const HRESULT hr = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
  std::wstring out;
  if (SUCCEEDED(hr) && version) {
    out.assign(version);
    ::CoTaskMemFree(version);
  }
  if (out.empty()) return std::wstring();
  if (!custom.empty()) return L"系统 " + out + L"（自定义运行时不可用，已回退）";
  return L"系统 " + out;
}
