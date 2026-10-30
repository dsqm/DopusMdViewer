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
//     一次 SetParent 加一次推送，不必再付浏览器进程冷启动的两三秒。
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

#include "log.h"
#include "util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kHostClass[] = L"DopusMdViewerHostWnd";
const wchar_t kAppHost[] = L"mdprev.local";
const wchar_t kDocHost[] = L"mddoc.local";
const wchar_t kShellUrl[] = L"https://mdprev.local/shell.html";

// 命令号：调用线程 → 宿主线程。参数在 SendMessage 期间有效。
const UINT kCmdBase = WM_APP + 0x200;
const UINT kCmdEnsure = kCmdBase + 1;
const UINT kCmdBorrow = kCmdBase + 2;
const UINT kCmdUnborrow = kCmdBase + 3;
const UINT kCmdBounds = kCmdBase + 4;
const UINT kCmdPost = kCmdBase + 5;
const UINT kCmdDocFolder = kCmdBase + 6;
const UINT kCmdRelease = kCmdBase + 7;

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
};

Host g_host;

// 跨线程读取的标志与句柄。
volatile LONG g_readyFlag = 0;
volatile LONG g_hasThread = 0;
DWORD g_threadId = 0;
HWND g_threadHost = nullptr;

std::wstring WebDir() {
  const std::wstring moduleDir = UtilModuleDir();
  if (moduleDir.empty()) return std::wstring();
  return UtilJoin(moduleDir, L"web");
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
  ::InterlockedExchange(&g_readyFlag, 0);
  LogLine(L"WEBHOST: controller 已释放");
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
  LogLine(L"WEBHOST: Environment 就绪");
  CreateController();
}

void CreateController() {
  if (!g_host.host || !g_host.env) return;
  g_host.creating = true;

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

                      if (frame == L"ready") {
                        g_host.ready = true;
                        ::InterlockedExchange(&g_readyFlag, 1);
                        LogLine(L"WEBHOST: 页面已就绪");
                        PostEvent(MDV_EVT_READY, L"");
                      } else {
                        PostEvent(MDV_EVT_MESSAGE, frame.c_str());
                      }
                      return S_OK;
                    })
                    .Get(),
                &msgToken);

            EventRegistrationToken navToken = {};
            g_host.webview->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [](ICoreWebView2* sender,
                       ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                      (void)sender;
                      BOOL ok = FALSE;
                      args->get_IsSuccess(&ok);
                      LogLine(L"WEBHOST: NavigationCompleted ok=%d", ok ? 1 : 0);
                      if (!ok) Fail(L"shell.html 导航失败");
                      return S_OK;
                    })
                    .Get(),
                &navToken);

            RECT rc = {0, 0, 0, 0};
            if (g_host.host) ::GetClientRect(g_host.host, &rc);
            controller->put_Bounds(rc);
            controller->put_IsVisible(TRUE);

            LogLine(L"WEBHOST: controller 就绪，导航到 %s", kShellUrl);
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

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case kCmdEnsure:
      EnsureEnvironment();
      return 0;

    case kCmdBorrow: {
      HWND viewer = reinterpret_cast<HWND>(wParam);
      const RECT* rc = reinterpret_cast<const RECT*>(lParam);
      if (!viewer || !rc || !g_host.host) return 0;

      g_host.borrower = viewer;
      SetChildStyle(true);
      ::SetParent(g_host.host, viewer);
      ::SetWindowPos(g_host.host, nullptr, 0, 0, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
      ::ShowWindow(g_host.host, SW_SHOW);
      g_host.borrowed = true;

      if (g_host.controller) g_host.controller->put_Bounds(*rc);
      LogLine(L"WEBHOST: 借用 → viewer=%p size=%ldx%ld", viewer, rc->right - rc->left,
              rc->bottom - rc->top);
      return 1;
    }

    case kCmdUnborrow:
      if (!g_host.host) return 0;
      g_host.borrowed = false;
      g_host.borrower = nullptr;
      ::ShowWindow(g_host.host, SW_HIDE);
      ::SetParent(g_host.host, nullptr);
      SetChildStyle(false);
      ::SetWindowPos(g_host.host, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_FRAMECHANGED);
      LogLine(L"WEBHOST: 归还宿主（controller 保留）");
      return 1;

    case kCmdBounds: {
      const RECT* rc = reinterpret_cast<const RECT*>(lParam);
      if (!rc || !g_host.host) return 0;
      ::SetWindowPos(g_host.host, nullptr, 0, 0, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      if (g_host.controller) g_host.controller->put_Bounds(*rc);
      return 1;
    }

    case kCmdPost: {
      const std::wstring* text = reinterpret_cast<const std::wstring*>(lParam);
      if (!text || !g_host.webview || !g_host.ready) return 0;

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
      const HRESULT hr = g_host.webview3->SetVirtualHostNameToFolderMapping(
          kDocHost, dir->c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
      LogLine(L"WEBHOST: 映射 %s → %s hr=0x%08X", kDocHost, dir->c_str(),
              static_cast<unsigned>(hr));
      return SUCCEEDED(hr) ? 1 : 0;
    }

    case kCmdRelease:
      ReleaseController();
      return 1;

    case WM_NCDESTROY:
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      ReleaseController();
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
bool SendCommand(UINT cmd, WPARAM wParam, LPARAM lParam) {
  HWND host = EnsureHostThread();
  if (!host || !::IsWindow(host)) return false;
  ::SendMessageW(host, cmd, wParam, lParam);
  return true;
}

}  // namespace

bool WebHostEnsure(void) { return SendCommand(kCmdEnsure, 0, 0); }

bool WebHostIsReady(void) { return ::InterlockedCompareExchange(&g_readyFlag, 0, 0) != 0; }

bool WebHostPost(const std::wstring& text) {
  if (!WebHostIsReady()) return false;
  return SendCommand(kCmdPost, 0, reinterpret_cast<LPARAM>(&text));
}

void WebHostBorrow(HWND hViewer, const RECT& client) {
  if (!hViewer) return;
  SendCommand(kCmdBorrow, reinterpret_cast<WPARAM>(hViewer),
              reinterpret_cast<LPARAM>(&client));
}

void WebHostUnborrow(void) { SendCommand(kCmdUnborrow, 0, 0); }

void WebHostSetBounds(const RECT& client) {
  SendCommand(kCmdBounds, 0, reinterpret_cast<LPARAM>(&client));
}

bool WebHostSetDocFolder(const std::wstring& dir) {
  HWND host = EnsureHostThread();
  if (!host || !::IsWindow(host)) return false;
  return ::SendMessageW(host, kCmdDocFolder, 0, reinterpret_cast<LPARAM>(&dir)) != 0;
}

void WebHostReleaseController(void) { SendCommand(kCmdRelease, 0, 0); }
