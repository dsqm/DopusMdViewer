// webhost.cpp - 按线程隔离的隐藏宿主 + Environment/Controller + 虚拟主机映射。
//
// 四条硬事实，都是踩出来的：
//  1. controller 的父窗口一旦销毁就永久失效，所以它只认插件自持的宿主窗口，
//     Opus 的查看器窗口通过 SetParent 借用（票 10）。归还必须在 WM_DESTROY 做：
//     DestroyWindow 会连子窗口一起销毁，等到 WM_NCDESTROY 宿主已经没了。
//  2. **WebView2 的对象是线程绑定的**。Opus 的预览窗格与独立查看器窗口跑在不同的
//     UI 线程上（日志里 18112 与 15248），所以实例必须按线程隔离；共用一个全局
//     controller 会让第二个线程的 postMessage 直接失败（0x802A000C）。
//  3. 虚拟主机映射出来的响应由 WebView2 自己合成，加不了响应头；而**映射过的请求
//     也不会进 WebResourceRequested**（实测：通配过滤器 + ALL 上下文一次都不触发）。
//     所以 CSP 只能写在 shell.html 的 meta 里。
//  4. 包含顺序不是可选项：WebView2.h 是 MIDL 生成的，依赖 objbase.h /
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

#include <vector>

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

// 每个 Opus UI 线程最多一份实例；超过这个数的线程（没人关窗？）只记日志。
const size_t kMaxInstances = 8;

// 一个 UI 线程的全部 WebView2 家当。所有成员只能在 threadId 那个线程上访问。
struct Instance {
  DWORD threadId = 0;
  HWND host = nullptr;

  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  ComPtr<ICoreWebView2_3> webview3;

  WebHostSink sink = {};
  std::wstring docFolder;

  bool creating = false;
  bool ready = false;
  bool borrowed = false;
};

std::vector<Instance*> g_instances;

Instance* FindByThread(DWORD threadId) {
  for (Instance* inst : g_instances) {
    if (inst->threadId == threadId) return inst;
  }
  return nullptr;
}

Instance* FindByHost(HWND host) {
  for (Instance* inst : g_instances) {
    if (inst->host == host) return inst;
  }
  return nullptr;
}

void RemoveInstance(Instance* inst, bool closeController) {
  if (!inst) return;

  if (inst->host && ::IsWindow(inst->host)) {
    ::KillTimer(inst->host, kIdleTimerId);
    ::SetWindowLongPtrW(inst->host, GWLP_USERDATA, 0);
  }

  // Close 必须在拥有 controller 的线程上调用；线程已经没了就只放引用。
  if (closeController && inst->controller) {
    inst->controller->Close();
  }
  inst->controller.Reset();
  inst->webview3.Reset();
  inst->webview.Reset();
  inst->env.Reset();

  for (size_t i = 0; i < g_instances.size(); ++i) {
    if (g_instances[i] == inst) {
      g_instances.erase(g_instances.begin() + static_cast<ptrdiff_t>(i));
      break;
    }
  }
  delete inst;
}

// 线程已经没了的实例：它的窗口被系统带走了，controller 无法再 Close，
// 只能把引用放掉，别让它一直吊着。
void SweepDeadInstances(DWORD currentThread) {
  for (size_t i = g_instances.size(); i > 0; --i) {
    Instance* inst = g_instances[i - 1];
    if (inst->threadId == currentThread) continue;
    if (inst->host && ::IsWindow(inst->host)) continue;

    LogLine(L"WEBHOST: 线程 %lu 已结束，释放它留下的实例", inst->threadId);
    RemoveInstance(inst, /*closeController=*/false);
  }
}

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

void NotifyFailed(Instance* inst, const wchar_t* reason) {
  LogLine(L"WEBHOST[%lu]: 失败 - %s", inst->threadId, reason);
  if (inst->sink.onFailed) inst->sink.onFailed(inst->sink.ctx, reason);
}

void NotifyReady(Instance* inst) {
  LogLine(L"WEBHOST[%lu]: 页面已就绪", inst->threadId);
  if (inst->sink.onReady) inst->sink.onReady(inst->sink.ctx);
}

void ReleaseController(Instance* inst) {
  if (!inst) return;
  if (inst->controller) {
    inst->controller->Close();
    inst->controller.Reset();
  }
  inst->webview3.Reset();
  inst->webview.Reset();
  inst->ready = false;
  LogLine(L"WEBHOST[%lu]: controller 已释放", inst->threadId);
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

void CreateController(Instance* inst);

void OnEnvironmentReady(Instance* inst, HRESULT hr, ICoreWebView2Environment* env) {
  if (FAILED(hr) || !env) {
    inst->creating = false;
    wchar_t szMsg[128];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg), L"Environment 创建失败 hr=0x%08X",
                       static_cast<unsigned>(hr));
    NotifyFailed(inst, szMsg);
    return;
  }
  inst->env = env;
  LogLine(L"WEBHOST[%lu]: Environment 就绪", inst->threadId);
  CreateController(inst);
}

void CreateController(Instance* inst) {
  if (!inst->host || !inst->env) return;
  inst->creating = true;

  const HRESULT hr = inst->env->CreateCoreWebView2Controller(
      inst->host,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [inst](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            inst->creating = false;
            if (FAILED(result) || !controller) {
              wchar_t szMsg[200];
              ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg),
                                 L"controller 创建失败 hr=0x%08X host=%p isWindow=%d",
                                 static_cast<unsigned>(result), inst->host,
                                 (inst->host && ::IsWindow(inst->host)) ? 1 : 0);
              NotifyFailed(inst, szMsg);
              return S_OK;
            }

            inst->controller = controller;

            if (FAILED(controller->get_CoreWebView2(&inst->webview)) || !inst->webview) {
              NotifyFailed(inst, L"get_CoreWebView2 失败");
              return S_OK;
            }

            ApplySettings(inst->webview.Get());

            // 虚拟主机：app 资源。DENY_CORS 让同源可用、跨源 fetch 被拒（票 08）。
            ComPtr<ICoreWebView2_3> webview3;
            if (SUCCEEDED(inst->webview.As(&webview3)) && webview3) {
              inst->webview3 = webview3;
              const std::wstring webDir = WebDir();
              const HRESULT mapHr = webview3->SetVirtualHostNameToFolderMapping(
                  kAppHost, webDir.c_str(),
                  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
              LogLine(L"WEBHOST[%lu]: 映射 %s → %s hr=0x%08X", inst->threadId, kAppHost,
                      webDir.c_str(), static_cast<unsigned>(mapHr));
              if (!inst->docFolder.empty()) {
                webview3->SetVirtualHostNameToFolderMapping(
                    kDocHost, inst->docFolder.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
              }
            } else {
              NotifyFailed(inst, L"拿不到 ICoreWebView2_3，虚拟主机不可用");
              return S_OK;
            }

            EventRegistrationToken msgToken = {};
            const HRESULT msgHr = inst->webview->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [inst](ICoreWebView2* sender,
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
                        inst->ready = true;
                        NotifyReady(inst);
                      } else if (inst->sink.onEvent) {
                        inst->sink.onEvent(inst->sink.ctx, type, rest);
                      }
                      return S_OK;
                    })
                    .Get(),
                &msgToken);
            LogLine(L"WEBHOST[%lu]: add_WebMessageReceived hr=0x%08X", inst->threadId,
                    static_cast<unsigned>(msgHr));

            EventRegistrationToken navToken = {};
            const HRESULT navHr = inst->webview->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [inst](ICoreWebView2* sender,
                           ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                      (void)sender;
                      BOOL ok = FALSE;
                      args->get_IsSuccess(&ok);
                      LogLine(L"WEBHOST[%lu]: NavigationCompleted ok=%d", inst->threadId,
                              ok ? 1 : 0);
                      if (!ok) NotifyFailed(inst, L"shell.html 导航失败");
                      return S_OK;
                    })
                    .Get(),
                &navToken);
            LogLine(L"WEBHOST[%lu]: add_NavigationCompleted hr=0x%08X", inst->threadId,
                    static_cast<unsigned>(navHr));

            RECT rc = {0, 0, 0, 0};
            if (inst->host) ::GetClientRect(inst->host, &rc);
            controller->put_Bounds(rc);
            controller->put_IsVisible(TRUE);

            LogLine(L"WEBHOST[%lu]: controller 就绪，导航到 %s", inst->threadId, kShellUrl);
            inst->webview->Navigate(kShellUrl);
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    inst->creating = false;
    wchar_t szMsg[128];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg), L"CreateCoreWebView2Controller 失败 hr=0x%08X",
                       static_cast<unsigned>(hr));
    NotifyFailed(inst, szMsg);
  }
}

void EnsureEnvironment(Instance* inst) {
  if (inst->env) {
    if (!inst->controller && !inst->creating) CreateController(inst);
    return;
  }
  if (inst->creating) return;

  // Opus 的 UI 线程大概率已经是 STA；冲突（RPC_E_CHANGED_MODE）也无所谓，
  // 这里只保证"至少被初始化过一次"，且不做 CoUninitialize（插件与进程同寿命）。
  const HRESULT coHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  LogLine(L"WEBHOST[%lu]: CoInitializeEx hr=0x%08X", inst->threadId,
          static_cast<unsigned>(coHr));

  inst->creating = true;

  const std::wstring userData = UserDataDir();
  const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, userData.empty() ? nullptr : userData.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [inst](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            OnEnvironmentReady(inst, result, env);
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    inst->creating = false;
    wchar_t szMsg[160];
    ::StringCchPrintfW(szMsg, ARRAYSIZE(szMsg),
                       L"WebView2 运行时不可用 hr=0x%08X（未安装或版本过低）",
                       static_cast<unsigned>(hr));
    NotifyFailed(inst, szMsg);
  }
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  Instance* inst = reinterpret_cast<Instance*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (uMsg) {
    case WM_TIMER:
      if (wParam == kIdleTimerId) {
        ::KillTimer(hwnd, kIdleTimerId);
        if (inst && !inst->borrowed) {
          LogLine(L"WEBHOST[%lu]: 空闲超时，释放 controller", inst->threadId);
          ReleaseController(inst);
        }
      }
      return 0;

    case WM_NCDESTROY: {
      ::KillTimer(hwnd, kIdleTimerId);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      if (inst) {
        if (inst->controller) {
          inst->controller->Close();
          inst->controller.Reset();
        }
        inst->webview3.Reset();
        inst->webview.Reset();
        inst->host = nullptr;
        inst->ready = false;
        inst->borrowed = false;
        LogLine(L"WEBHOST[%lu]: 宿主窗口被销毁，状态已重置", inst->threadId);
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

bool EnsureHostWindow(Instance* inst) {
  if (inst->host && ::IsWindow(inst->host)) return true;
  inst->host = nullptr;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HostWndProc;
  wc.hInstance = static_cast<HINSTANCE>(::GetModuleHandleW(nullptr));
  wc.lpszClassName = kHostClass;

  if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogLine(L"WEBHOST[%lu]: RegisterClassExW 失败 err=%lu", inst->threadId, ::GetLastError());
    return false;
  }

  // WS_POPUP + 不可见：先游离在桌面上，被借用时改成 WS_CHILD 挂进查看器窗口。
  inst->host = ::CreateWindowExW(WS_EX_NOACTIVATE, kHostClass, L"DopusMdViewerHost",
                                 WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr,
                                 static_cast<HINSTANCE>(::GetModuleHandleW(nullptr)),
                                 inst);
  if (!inst->host) {
    LogLine(L"WEBHOST[%lu]: 创建宿主窗口失败 err=%lu", inst->threadId, ::GetLastError());
    return false;
  }

  ::SetWindowLongPtrW(inst->host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(inst));
  LogLine(L"WEBHOST[%lu]: 宿主窗口 %p 已创建", inst->threadId, inst->host);
  return true;
}

// 取当前线程的实例；没有就建一个（不建 controller）。
Instance* AcquireCurrent() {
  const DWORD tid = ::GetCurrentThreadId();
  Instance* inst = FindByThread(tid);
  if (inst) {
    SweepDeadInstances(tid);
    return inst;
  }

  if (g_instances.size() >= kMaxInstances) {
    LogLine(L"WEBHOST: 实例数已达上限 %zu，线程 %lu 复用失败", kMaxInstances, tid);
    return nullptr;
  }

  inst = new Instance();
  inst->threadId = tid;
  if (!EnsureHostWindow(inst)) {
    delete inst;
    return nullptr;
  }

  g_instances.push_back(inst);
  SweepDeadInstances(tid);
  LogLine(L"WEBHOST: 为线程 %lu 建实例（当前 %zu 个）", tid, g_instances.size());
  return inst;
}

void SetChildStyle(Instance* inst, bool child) {
  if (!inst->host) return;
  LONG_PTR style = ::GetWindowLongPtrW(inst->host, GWL_STYLE);
  if (child) {
    style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
  } else {
    style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
  }
  ::SetWindowLongPtrW(inst->host, GWL_STYLE, style);
}

}  // namespace

bool WebHostEnsure(const WebHostSink& sink) {
  Instance* inst = AcquireCurrent();
  if (!inst) return false;

  inst->sink = sink;
  EnsureEnvironment(inst);
  return true;
}

void WebHostSetSink(const WebHostSink& sink) {
  Instance* inst = FindByThread(::GetCurrentThreadId());
  if (inst) inst->sink = sink;
}

bool WebHostIsReady(void) {
  Instance* inst = FindByThread(::GetCurrentThreadId());
  return inst && inst->ready && inst->webview && inst->host && ::IsWindow(inst->host);
}

bool WebHostPost(const std::wstring& text) {
  Instance* inst = FindByThread(::GetCurrentThreadId());
  if (!inst || !inst->webview || !inst->ready) return false;

  const HRESULT hr = inst->webview->PostWebMessageAsString(text.c_str());
  if (FAILED(hr)) {
    LogLine(L"WEBHOST[%lu]: PostWebMessageAsString 失败 hr=0x%08X（%zu 字符）", inst->threadId,
            static_cast<unsigned>(hr), text.size());
    // 0x8007139F = RO_E_CLOSED：controller 或它的父窗口已经没了。
    if (static_cast<unsigned>(hr) == 0x8007139Fu) {
      LogLine(L"WEBHOST[%lu]: controller 已失效，重置后等下次借用重建", inst->threadId);
      ReleaseController(inst);
    }
    return false;
  }
  return true;
}

void WebHostBorrow(HWND hViewer, const RECT& client) {
  if (!hViewer) return;

  Instance* inst = AcquireCurrent();
  if (!inst || !inst->host) return;

  ::KillTimer(inst->host, kIdleTimerId);

  SetChildStyle(inst, true);
  ::SetParent(inst->host, hViewer);
  ::SetWindowPos(inst->host, nullptr, 0, 0, client.right - client.left,
                 client.bottom - client.top, SWP_NOZORDER | SWP_FRAMECHANGED);
  ::ShowWindow(inst->host, SW_SHOW);
  inst->borrowed = true;

  if (inst->controller) inst->controller->put_Bounds(client);

  LogLine(L"WEBHOST[%lu]: 借用 → viewer=%p size=%ldx%ld", inst->threadId, hViewer,
          client.right - client.left, client.bottom - client.top);
}

void WebHostUnborrow(void) {
  Instance* inst = FindByThread(::GetCurrentThreadId());
  if (!inst || !inst->host || !::IsWindow(inst->host)) {
    if (inst) inst->borrowed = false;
    return;
  }

  inst->borrowed = false;
  ::ShowWindow(inst->host, SW_HIDE);
  ::SetParent(inst->host, nullptr);
  SetChildStyle(inst, false);
  ::SetWindowPos(inst->host, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_FRAMECHANGED);

  // 保活 + 空闲 60s 释放（票 10）：还有下一个文件时不重建 controller
  ::SetTimer(inst->host, kIdleTimerId, kIdleTimeoutMs, nullptr);
  LogLine(L"WEBHOST[%lu]: 归还宿主，%u ms 后空闲释放", inst->threadId, kIdleTimeoutMs);
}

void WebHostSetBounds(const RECT& client) {
  Instance* inst = FindByThread(::GetCurrentThreadId());
  if (!inst || !inst->host || !::IsWindow(inst->host)) return;

  ::SetWindowPos(inst->host, nullptr, 0, 0, client.right - client.left,
                 client.bottom - client.top, SWP_NOZORDER | SWP_NOACTIVATE);
  if (inst->controller) inst->controller->put_Bounds(client);
}

bool WebHostSetDocFolder(const std::wstring& dir) {
  Instance* inst = FindByThread(::GetCurrentThreadId());
  if (!inst) return false;
  if (dir == inst->docFolder) return true;
  inst->docFolder = dir;

  if (!inst->webview3) return false;

  if (dir.empty()) {
    inst->webview3->ClearVirtualHostNameToFolderMapping(kDocHost);
    LogLine(L"WEBHOST[%lu]: 取消 %s 映射", inst->threadId, kDocHost);
    return true;
  }

  const HRESULT hr = inst->webview3->SetVirtualHostNameToFolderMapping(
      kDocHost, dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
  LogLine(L"WEBHOST[%lu]: 映射 %s → %s hr=0x%08X", inst->threadId, kDocHost, dir.c_str(),
          static_cast<unsigned>(hr));
  return SUCCEEDED(hr);
}

void WebHostReleaseController(void) {
  Instance* inst = FindByThread(::GetCurrentThreadId());
  if (!inst) return;
  if (inst->host) ::KillTimer(inst->host, kIdleTimerId);
  ReleaseController(inst);
}
