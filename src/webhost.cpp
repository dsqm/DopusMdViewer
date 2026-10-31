// webhost.cpp - 把 WebView2 全部放到插件自己的一个线程上。
//
// 五条硬事实，都是踩出来的：
//  1. **WebView2 的对象是线程绑定的**。Opus 的预览窗格与独立查看器窗口跑在不同
//     的 UI 线程上（日志里 18112 与 15248），共用全局 controller 会让第二个线程
//     的 postMessage 直接失败（0x802A000C）。因此这里自建一个线程，Environment
//     与 Controller 只活在那条线程上，其余线程通过消息借用。
//  2. controller 的父窗口一旦销毁就永久失效，所以它只认插件自持的宿主窗口，
//     Opus 的查看器窗口通过 SetParent 借用（票 10）。归还必须在 WM_DESTROY 做：
//     DestroyWindow 会连子窗口一起销毁，等到 WM_NCDESTROY 宿主已经没了。
//  3. 宿主线程与进程同寿命，controller 不按空闲回收——这样关掉窗口再打开只是
//     一次 SetParent 加一次推送，不必再付浏览器进程冷启动的两三秒。代价是**同一
//     时刻只有一个查看器有画面**：其它查看器借用会抢走宿主，归还时按借用历史交回
//     上一个还活着的查看器（WM_MDV_HOST_LOST / WM_MDV_RECLAIM），不这样做关掉一个
//     窗口就会留下另一片空白。
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

#include "doc_source.h"
#include "log.h"
#include "util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kHostClass[] = L"DopusMdViewerHostWnd";
const wchar_t kAppHost[] = L"mdprev.localhost";
const wchar_t kDocHost[] = L"mddoc.localhost";
const wchar_t kShellUrl[] = L"https://mdprev.localhost/shell.html";

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

// 预热：InitEx 后等一会儿再开始，避开 Opus 自己的启动高峰。
const UINT_PTR kTimerPrewarm = 1;
const UINT_PTR kTimerWatch = 2;
const UINT kPrewarmDelayMs = 3000;
const UINT kWatchIntervalMs = 2000;
const int kWatchEmptyLimit = 2;  // 连续两次看不见 DO 窗口才释放（约 4 秒）
const int kMaxRoots = 8;

// 以下状态只在宿主线程上访问。
struct Host {
  HWND host = nullptr;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  ComPtr<ICoreWebView2_3> webview3;
  HWND borrower = nullptr;  // 事件接收方
  std::wstring docFolder;
  bool creating = false;
  bool ready = false;
  bool borrowed = false;
  bool prewarmEnabled = false;
  bool everSawViewer = false;  // 预热后是否真的被查看器借过
  bool sawDopusWindow = false;
  bool suspended = false;
  int watchEmpty = 0;
  LARGE_INTEGER tEnsure = {};
  LARGE_INTEGER tController = {};
  LARGE_INTEGER tNav = {};
};

Host g_host;

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
volatile LONG g_hasThread = 0;
DWORD g_threadId = 0;
HWND g_threadHost = nullptr;

std::wstring WebDir() {
  return UtilWebDir();
}

std::wstring UserDataDir() {
  const std::wstring dataDir = UtilDataDir();
  if (dataDir.empty()) return std::wstring();
  const std::wstring dir = UtilJoin(dataDir, L"WebView2");
  UtilEnsureDir(dir);
  return dir;
}

// ---- 宿主线程 → 借用方 ----

void PostEvent(int kind, const wchar_t* text) {
  HWND target = g_host.borrower;
  if (!target || !::IsWindow(target)) return;

  MDV_EVENT* evt = new MDV_EVENT();
  evt->kind = kind;
  evt->text = nullptr;
  if (text && *text) {
    const size_t cch = wcslen(text) + 1;
    evt->text = new wchar_t[cch];
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
  }
  ReleaseController();
  g_host.env.Reset();
  g_host.docFolder.clear();
  g_host.watchEmpty = 0;
  g_host.everSawViewer = false;
  g_host.sawDopusWindow = false;
  g_rootsCount = 0;
  LogLine(L"WEBHOST: 实例已释放（Environment 也回收，下次借用重建）");
}

void CreateController();

void OnEnvironmentReady(HRESULT hr, ICoreWebView2Environment* env) {
  if (FAILED(hr) || !env) {
    g_host.creating = false;
    wchar_t szMsg[128];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg), L"Environment 创建失败 hr=0x%08X",
                       static_cast<unsigned>(hr));
    Fail(szMsg);
    return;
  }
  g_host.env = env;
  LARGE_INTEGER now;
  ::QueryPerformanceCounter(&now);
  LogLine(L"WEBHOST: Environment 就绪 +%.1fms", QpcMs(g_host.tEnsure.QuadPart, now.QuadPart));
  CreateController();
}

void CreateController() {
  if (!g_host.host || !g_host.env) return;
  g_host.creating = true;
  ::QueryPerformanceCounter(&g_host.tController);

  const HRESULT hr = g_host.env->CreateCoreWebView2Controller(
      g_host.host,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
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

            // 虚拟主机：app 资源。DENY_CORS 让同源可用、跨源 fetch 被拒（票 08）。
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

                      std::string body;
                      int status = 404;
                      std::wstring contentType = L"application/octet-stream";
                      // 404 也必须给响应：不响应会让渲染进程一直等到超时
                      // （页面侧看到的是一句没头没尾的 "Failed to fetch"）。
                      if (!docsrc::ServeAsset(uri, &body, &status, &contentType)) {
                        status = 404;
                        // 未匹配的路径也要回一个空响应：只 return 会让请求挂到超时。
                        if (!docsrc::ServeSearch(uri, &body, &status) &&
                            !docsrc::ServeOutline(uri, &body, &status) &&
                            !docsrc::ServeLines(uri, &body, &status) &&
                            !docsrc::ServeDefs(uri, &body, &status) &&
                            !docsrc::Serve(uri, &body, &status)) {
                          status = 404;
                          body.clear();
                        }
                      }

                      static int s_requests = 0;
                      if (s_requests < 200) {
                        ++s_requests;
                        LogLine(L"WEBHOST: 资源请求 %s -> %d/%zu B", uri.c_str(), status,
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
                          L"Access-Control-Allow-Origin: *\r\n";

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
                        } else {
                          SetMemoryLevel(true);
                          SuspendIdle();
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

void EnsureEnvironment() {
  if (g_host.env) {
    if (!g_host.controller && !g_host.creating) CreateController();
    return;
  }
  if (g_host.creating) return;

  g_host.creating = true;
  ::QueryPerformanceCounter(&g_host.tEnsure);

  const std::wstring userData = UserDataDir();
  const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, userData.empty() ? nullptr : userData.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            OnEnvironmentReady(result, env);
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    g_host.creating = false;
    wchar_t szMsg[160];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg),
                       L"WebView2 运行时不可用 hr=0x%08X（未安装或版本过低）",
                       static_cast<unsigned>(hr));
    Fail(szMsg);
  }
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
  SetMemoryLevel(true);
  SuspendIdle();

  HWND next = HistoryFirstLive(viewer);
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
  ::SetTimer(g_host.host, kTimerPrewarm, delayMs, nullptr);
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
      EnsureEnvironment();
      return 0;

    case kCmdPrewarm:
      g_host.prewarmEnabled = wParam != 0;
      if (g_host.host) ::KillTimer(g_host.host, kTimerPrewarm);
      if (g_host.prewarmEnabled) {
        SchedulePrewarm(kPrewarmDelayMs);
      } else if (!g_host.borrower && (g_host.controller || g_host.env)) {
        LogLine(L"WEBHOST: 预热已关闭，释放空闲实例");
        ReleaseInstance();
      }
      return 1;

    case WM_TIMER:
      if (wParam == kTimerPrewarm) {
        ::KillTimer(hwnd, kTimerPrewarm);
        StartPrewarm();
        return 0;
      }
      if (wParam == kTimerWatch) {
        WatchTick();
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

    case kCmdScript: {
      const std::wstring* script = reinterpret_cast<const std::wstring*>(lParam);
      if (!script || !g_host.webview || !g_host.ready) return 0;
      if (g_host.borrower != reinterpret_cast<HWND>(wParam)) return 0;
      return SUCCEEDED(g_host.webview->ExecuteScript(script->c_str(), nullptr)) ? 1 : 0;
    }

    case WM_NCDESTROY:
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      ReleaseInstance();
      g_host.host = nullptr;
      g_threadHost = nullptr;
      LogLine(L"WEBHOST: 宿主窗口被销毁");
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

// 宿主线程：建窗口、跑消息泵，进程结束时才退。
DWORD WINAPI HostThreadProc(LPVOID param) {
  HANDLE readyEvent = static_cast<HANDLE>(param);

  // Opus 的 UI 线程多半已是 STA；这里在自己的线程上重新初始化一次。
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HostWndProc;
  wc.hInstance = static_cast<HINSTANCE>(::GetModuleHandleW(nullptr));
  wc.lpszClassName = kHostClass;

  if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogLine(L"WEBHOST: RegisterClassExW 失败 err=%lu", ::GetLastError());
    ::SetEvent(readyEvent);
    return 1;
  }

  // WS_POPUP + 不可见：先游离在桌面上，被借用时改成 WS_CHILD 塞进查看器窗口。
  g_host.host = ::CreateWindowExW(WS_EX_NOACTIVATE, kHostClass, L"DopusMdViewerHost",
                                  WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr,
                                  static_cast<HINSTANCE>(::GetModuleHandleW(nullptr)), nullptr);
  g_threadHost = g_host.host;
  LogLine(L"WEBHOST: 宿主线程 %lu 就绪，窗口 %p", ::GetCurrentThreadId(), g_host.host);
  ::SetEvent(readyEvent);

  if (!g_host.host) return 1;

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  ReleaseController();
  g_host.env.Reset();
  return 0;
}

// 建线程（只建一次）。返回宿主窗口句柄，失败返回 nullptr。
HWND EnsureHostThread() {
  if (::InterlockedCompareExchange(&g_hasThread, 1, 0) == 0) {
    HANDLE readyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
      ::InterlockedExchange(&g_hasThread, 0);
      return nullptr;
    }

    DWORD tid = 0;
    HANDLE hThread = ::CreateThread(nullptr, 0, HostThreadProc, readyEvent, 0, &tid);
    if (!hThread) {
      ::CloseHandle(readyEvent);
      ::InterlockedExchange(&g_hasThread, 0);
      LogLine(L"WEBHOST: CreateThread 失败 err=%lu", ::GetLastError());
      return nullptr;
    }
    ::CloseHandle(hThread);  // 与进程同寿命，不需要 join

    ::WaitForSingleObject(readyEvent, 5000);
    ::CloseHandle(readyEvent);
    g_threadId = tid;
  }

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

void WebHostPrewarm(bool enabled) {
  // 关闭预热时如果宿主线程还没建起来，就别为了关一个定时器去建线程。
  if (!enabled && ::InterlockedCompareExchange(&g_hasThread, 0, 0) == 0) return;
  SendCommand(kCmdPrewarm, enabled ? 1 : 0, 0);
}

bool WebHostIsReady(void) { return ::InterlockedCompareExchange(&g_readyFlag, 0, 0) != 0; }

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

bool WebHostSetDocFolder(const std::wstring& dir) {
  HWND host = EnsureHostThread();
  if (!host || !::IsWindow(host)) return false;
  return ::SendMessageW(host, kCmdDocFolder, 0, reinterpret_cast<LPARAM>(&dir)) != 0;
}

void WebHostReleaseController(void) { SendCommand(kCmdRelease, 0, 0); }

bool WebHostExecuteScript(HWND hViewer, const std::wstring& script) {
  if (!hViewer) return false;
  LRESULT result = 0;
  if (!SendCommand(kCmdScript, reinterpret_cast<WPARAM>(hViewer),
                   reinterpret_cast<LPARAM>(&script), &result)) {
    return false;
  }
  return result != 0;
}

std::wstring WebHostRuntimeVersion(void) {
  LPWSTR version = nullptr;
  const HRESULT hr = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
  if (FAILED(hr) || !version) return std::wstring();
  std::wstring out(version);
  ::CoTaskMemFree(version);
  return out;
}
