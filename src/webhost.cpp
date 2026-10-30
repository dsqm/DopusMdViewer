// webhost.cpp - 隐藏宿主 + Environment/Controller + 虚拟主机映射。
//
// 三件事值得记下来：
//  1. controller 的父窗口一旦销毁就永久失效，所以它只认插件自持的宿主窗口，
//     Opus 的查看器窗口通过 SetParent 借用（票 10）。
//  2. 虚拟主机映射出来的响应由 WebView2 自己合成，加不了响应头；而**映射过的
//     请求也不会进 WebResourceRequested**（实测：通配过滤器 + ALL 上下文一次都
//     不触发）。所以 CSP 只能写在 shell.html 的 meta 里。
//  3. 包含顺序不是可选项：WebView2.h 是 MIDL 生成的，依赖 objbase.h /
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
#include "msg.h"
#include "util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kHostClass[] = L"DopusMdViewerHostWnd";
const wchar_t kAppHost[] = L"mdprev.local";
const wchar_t kDocHost[] = L"mddoc.local";
const wchar_t kShellUrl[] = L"https://mdprev.local/shell.html";


const UINT_PTR kIdleTimerId = 0xD0D1;
const UINT kIdleTimeoutMs = 60 * 1000;  // 票 10：保活 + 空闲 60s 释放

HWND g_host = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<ICoreWebView2_3> g_webview3;

WebHostSink g_sink = {};

bool g_creating = false;
bool g_ready = false;
bool g_borrowed = false;
std::wstring g_docFolder;

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

void NotifyFailed(const wchar_t* reason) {
  LogLine(L"WEBHOST: 失败 - %s", reason);
  if (g_sink.onFailed) g_sink.onFailed(g_sink.ctx, reason);
}

void NotifyReady() {
  LogLine(L"WEBHOST: 页面已就绪");
  if (g_sink.onReady) g_sink.onReady(g_sink.ctx);
}

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
  if (g_controller) {
    g_controller->Close();
    g_controller.Reset();
  }
  g_webview3.Reset();
  g_webview.Reset();
  g_ready = false;
  LogLine(L"WEBHOST: controller 已释放");
}

void CreateController();

void OnEnvironmentReady(HRESULT hr, ICoreWebView2Environment* env) {
  if (FAILED(hr) || !env) {
    g_creating = false;
    wchar_t szMsg[128];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg), L"Environment 创建失败 hr=0x%08X",
                       static_cast<unsigned>(hr));
    NotifyFailed(szMsg);
    return;
  }
  g_env = env;
  LogLine(L"WEBHOST: Environment 就绪");
  CreateController();
}

void CreateController() {
  if (!g_host || !g_env) return;
  g_creating = true;

  HRESULT hr = g_env->CreateCoreWebView2Controller(
      g_host, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                  [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    g_creating = false;
                    if (FAILED(result) || !controller) {
                      wchar_t szMsg[128];
                      ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg),
                                         L"controller 创建失败 hr=0x%08X",
                                         static_cast<unsigned>(result));
                      NotifyFailed(szMsg);
                      return S_OK;
                    }

                    g_controller = controller;

                    if (FAILED(controller->get_CoreWebView2(&g_webview)) || !g_webview) {
                      NotifyFailed(L"get_CoreWebView2 失败");
                      return S_OK;
                    }

                    ApplySettings(g_webview.Get());

                    // 虚拟主机：app 资源。DENY_CORS 让同源可用、跨源 fetch 被拒（票 08）。
                    ComPtr<ICoreWebView2_3> webview3;
                    if (SUCCEEDED(g_webview.As(&webview3)) && webview3) {
                      g_webview3 = webview3;
                      const std::wstring webDir = WebDir();
                      const HRESULT mapHr = webview3->SetVirtualHostNameToFolderMapping(
                          kAppHost, webDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                      LogLine(L"WEBHOST: 映射 %s → %s hr=0x%08X", kAppHost, webDir.c_str(),
                              static_cast<unsigned>(mapHr));
                      if (!g_docFolder.empty()) {
                        webview3->SetVirtualHostNameToFolderMapping(
                            kDocHost, g_docFolder.c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                      }
                    } else {
                      NotifyFailed(L"拿不到 ICoreWebView2_3，虚拟主机不可用");
                      return S_OK;
                    }

                    EventRegistrationToken msgToken = {};
                    const HRESULT msgHr = g_webview->add_WebMessageReceived(
                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [](ICoreWebView2* sender,
                               ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                              (void)sender;
                              LPWSTR raw = nullptr;
                              if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;

                              const std::wstring frame(raw);
                              ::CoTaskMemFree(raw);

                              std::wstring type;
                              std::wstring rest;
                              if (!msg::SplitType(frame, &type, &rest)) return S_OK;

                              if (type == L"ready") {
                                g_ready = true;
                                NotifyReady();
                              } else if (g_sink.onEvent) {
                                g_sink.onEvent(g_sink.ctx, type, rest);
                              }
                              return S_OK;
                            })
                            .Get(),
                        &msgToken);
                    LogLine(L"WEBHOST: add_WebMessageReceived hr=0x%08X",
                            static_cast<unsigned>(msgHr));

                    EventRegistrationToken navToken = {};
                    const HRESULT navHr = g_webview->add_NavigationCompleted(
                        Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [](ICoreWebView2* sender,
                               ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                              (void)sender;
                              BOOL ok = FALSE;
                              args->get_IsSuccess(&ok);
                              LogLine(L"WEBHOST: NavigationCompleted ok=%d", ok ? 1 : 0);
                              if (!ok) NotifyFailed(L"shell.html 导航失败");
                              return S_OK;
                            })
                            .Get(),
                        &navToken);
                    LogLine(L"WEBHOST: add_NavigationCompleted hr=0x%08X",
                            static_cast<unsigned>(navHr));

                    RECT rc = {0, 0, 0, 0};
                    if (g_host) ::GetClientRect(g_host, &rc);
                    controller->put_Bounds(rc);
                    controller->put_IsVisible(TRUE);

                    LogLine(L"WEBHOST: controller 就绪，导航到 %s", kShellUrl);
                    g_webview->Navigate(kShellUrl);
                    return S_OK;
                  })
                  .Get());

  if (FAILED(hr)) {
    g_creating = false;
    wchar_t szMsg[128];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg), L"CreateCoreWebView2Controller 失败 hr=0x%08X",
                       static_cast<unsigned>(hr));
    NotifyFailed(szMsg);
  }
}

void EnsureEnvironment() {
  if (g_env) {
    if (!g_controller && !g_creating) CreateController();
    return;
  }
  if (g_creating) return;

  // Opus 的 UI 线程大概率已经是 STA；冲突（RPC_E_CHANGED_MODE）也无所谓，
  // 这里只保证"至少被初始化过一次"，且不做 CoUninitialize（插件与进程同寿命）。
  const HRESULT coHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  LogLine(L"WEBHOST: CoInitializeEx hr=0x%08X", static_cast<unsigned>(coHr));

  g_creating = true;

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
    g_creating = false;
    wchar_t szMsg[160];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg),
                       L"WebView2 运行时不可用 hr=0x%08X（未安装或版本过低）",
                       static_cast<unsigned>(hr));
    NotifyFailed(szMsg);
  }
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_TIMER:
      if (wParam == kIdleTimerId) {
        ::KillTimer(hwnd, kIdleTimerId);
        if (!g_borrowed) {
          LogLine(L"WEBHOST: 空闲超时，释放 controller");
          ReleaseController();
        }
      }
      return 0;

    case WM_NCDESTROY:
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

bool EnsureHostWindow() {
  if (g_host) return true;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HostWndProc;
  wc.hInstance = static_cast<HINSTANCE>(::GetModuleHandleW(nullptr));
  wc.lpszClassName = kHostClass;

  if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogLine(L"WEBHOST: RegisterClassExW 失败 err=%lu", ::GetLastError());
    return false;
  }

  // WS_POPUP + 不可见：先游离在桌面上，被借用时改成 WS_CHILD 挂进查看器窗口。
  g_host = ::CreateWindowExW(WS_EX_NOACTIVATE, kHostClass, L"DopusMdViewerHost",
                             WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr,
                             static_cast<HINSTANCE>(::GetModuleHandleW(nullptr)), nullptr);
  if (!g_host) {
    LogLine(L"WEBHOST: 创建宿主窗口失败 err=%lu", ::GetLastError());
    return false;
  }

  LogLine(L"WEBHOST: 宿主窗口 %p 已创建", g_host);
  return true;
}

void SetChildStyle(bool child) {
  if (!g_host) return;
  LONG_PTR style = ::GetWindowLongPtrW(g_host, GWL_STYLE);
  if (child) {
    style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
  } else {
    style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
  }
  ::SetWindowLongPtrW(g_host, GWL_STYLE, style);
}

}  // namespace

bool WebHostEnsure(const WebHostSink& sink) {
  g_sink = sink;
  if (!EnsureHostWindow()) {
    NotifyFailed(L"宿主窗口创建失败");
    return false;
  }
  EnsureEnvironment();
  return true;
}

void WebHostSetSink(const WebHostSink& sink) { g_sink = sink; }

bool WebHostIsReady(void) { return g_ready && g_webview; }

bool WebHostPost(const std::wstring& text) {
  if (!g_webview || !g_ready) return false;

  const HRESULT hr = g_webview->PostWebMessageAsString(text.c_str());
  if (FAILED(hr)) {
    LogLine(L"WEBHOST: PostWebMessageAsString 失败 hr=0x%08X（%zu 字符）",
            static_cast<unsigned>(hr), text.size());
    return false;
  }
  return true;
}

void WebHostBorrow(HWND hViewer, const RECT& client) {
  if (!g_host || !hViewer) return;

  ::KillTimer(g_host, kIdleTimerId);

  SetChildStyle(true);
  ::SetParent(g_host, hViewer);
  ::SetWindowPos(g_host, nullptr, 0, 0, client.right - client.left, client.bottom - client.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED);
  ::ShowWindow(g_host, SW_SHOW);
  g_borrowed = true;

  if (g_controller) g_controller->put_Bounds(client);

  LogLine(L"WEBHOST: 借用 → viewer=%p size=%ldx%ld", hViewer, client.right - client.left,
          client.bottom - client.top);
}

void WebHostUnborrow(void) {
  if (!g_host) return;

  g_borrowed = false;
  ::ShowWindow(g_host, SW_HIDE);
  ::SetParent(g_host, nullptr);
  SetChildStyle(false);
  ::SetWindowPos(g_host, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_FRAMECHANGED);

  // 保活 + 空闲 60s 释放（票 10）：还有下一个文件时不重建 controller
  ::SetTimer(g_host, kIdleTimerId, kIdleTimeoutMs, nullptr);
  LogLine(L"WEBHOST: 归还宿主，%u ms 后空闲释放", kIdleTimeoutMs);
}

void WebHostSetBounds(const RECT& client) {
  if (!g_host) return;
  ::SetWindowPos(g_host, nullptr, 0, 0, client.right - client.left, client.bottom - client.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  if (g_controller) g_controller->put_Bounds(client);
}

bool WebHostSetDocFolder(const std::wstring& dir) {
  if (dir == g_docFolder) return true;
  g_docFolder = dir;

  if (!g_webview3) return false;

  if (dir.empty()) {
    g_webview3->ClearVirtualHostNameToFolderMapping(kDocHost);
    LogLine(L"WEBHOST: 取消 %s 映射", kDocHost);
    return true;
  }

  const HRESULT hr = g_webview3->SetVirtualHostNameToFolderMapping(
      kDocHost, dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
  LogLine(L"WEBHOST: 映射 %s → %s hr=0x%08X", kDocHost, dir.c_str(),
          static_cast<unsigned>(hr));
  return SUCCEEDED(hr);
}

void WebHostReleaseController(void) {
  if (g_host) ::KillTimer(g_host, kIdleTimerId);
  ReleaseController();
}
