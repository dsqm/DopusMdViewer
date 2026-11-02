// settings_preview.cpp - 见 settings_preview.h。
//
// 流程：建子窗口 → 异步建 Environment（独立用户数据目录）→ 建 Controller →
// NavigateToString 一个极简页面 → 之后只靠 ExecuteScript 改 font-family 与字号。
//
// 生命周期：内部状态由 shared_ptr 持有，异步回调各拿一份。设置窗口关掉时把
// hwnd 置空并释放 COM 引用，晚到的回调看到 hwnd 为空直接退出——否则「开了就关」
// 会让回调去读已经 delete 的对象。

#include "settings_preview.h"

#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>

#include <memory>
#include <new>

#include "log.h"
#include "util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kPreviewClass[] = L"DopusMdViewerSettingsPreview";

// 预览页：两行示例，字体与字号都由脚本改，避免每次改字体重建页面。
const wchar_t kPreviewHtml[] =
    L"<!doctype html><html><head><meta charset=\"utf-8\"><style>"
    L"html,body{margin:0;height:100%;background:#fff;color:#1a1a1a;"
    L"font-family:'Segoe UI';overflow:hidden}"
    L"#box{display:flex;flex-direction:column;justify-content:center;height:100%;"
    L"padding:0 10px;box-sizing:border-box}"
    L".l1,.l2{line-height:1.5}"
    L".l2{color:#5a5a5a}"
    L"</style></head><body><div id=box>"
    L"<div class=l1 id=a>中文示例：永和九年，岁在癸丑</div>"
    L"<div class=l2 id=b>English sample 0123 — Aa Bb Cc</div>"
    L"</div><script>window.__mdv=function(f,s){document.getElementById('box').style.fontFamily=f;"
    L"document.getElementById('a').style.fontSize=s+'px';"
    L"document.getElementById('b').style.fontSize=s+'px';}</script></body></html>";

// 本模块的 HINSTANCE：DLL 里不能用 GetModuleHandle(nullptr)（那是宿主 exe）。
HMODULE ThisModule() {
  HMODULE mod = nullptr;
  ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SettingsPreviewCreate), &mod);
  return mod;
}

// CSS 里的字体名要加引号；反斜杠与引号转义掉，免得一个怪名字把整条声明弄坏。
std::wstring QuoteFamily(const std::wstring& name) {
  std::wstring out = L"'";
  for (wchar_t c : name) {
    if (c == L'\\' || c == L'\'') out.push_back(L'\\');
    out.push_back(c);
  }
  out.push_back(L'\'');
  return out;
}

std::wstring BuildCssList(const std::vector<std::wstring>& chain) {
  if (chain.empty()) return L"'Segoe UI', sans-serif";
  std::wstring out;
  for (size_t i = 0; i < chain.size(); ++i) {
    if (i) out += L", ";
    out += QuoteFamily(chain[i]);
  }
  return out;
}

struct PreviewState {
  HWND hwnd = nullptr;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> web;
  bool ready = false;
  std::wstring css = L"'Segoe UI', sans-serif";
  int sizePx = 15;
};

// 窗口还在吗？晚到的异步回调靠它判断要不要继续。
bool Alive(const std::shared_ptr<PreviewState>& s) {
  return s && s->hwnd && ::IsWindow(s->hwnd);
}

// 页面就绪前后都调用：就绪了就立刻下发，否则留着等导航完成。
void PushFont(const std::shared_ptr<PreviewState>& s) {
  if (!Alive(s) || !s->ready || !s->web) return;
  std::wstring script =
      L"window.__mdv(" + s->css + L"," + std::to_wstring(s->sizePx) + L");";
  s->web->ExecuteScript(script.c_str(), nullptr);
}

}  // namespace

struct SettingsPreview {
  std::shared_ptr<PreviewState> s;
};

HWND SettingsPreviewHwnd(SettingsPreview* p) { return p && p->s ? p->s->hwnd : nullptr; }

SettingsPreview* SettingsPreviewCreate(HWND parent, const RECT& rc) {
  if (!parent) return nullptr;

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ThisModule();
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPreviewClass;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    registered = ::RegisterClassExW(&wc) != 0;
    if (!registered) return nullptr;
  }

  SettingsPreview* p = new (std::nothrow) SettingsPreview();
  if (!p) return nullptr;
  p->s = std::make_shared<PreviewState>();
  std::shared_ptr<PreviewState> s = p->s;

  s->hwnd = ::CreateWindowExW(WS_EX_CLIENTEDGE, kPreviewClass, L"", WS_CHILD | WS_VISIBLE, rc.left,
                              rc.top, rc.right - rc.left, rc.bottom - rc.top, parent, nullptr,
                              ThisModule(), nullptr);
  if (!s->hwnd) {
    delete p;
    return nullptr;
  }

  // 用户数据目录独立：同一份目录不能被两个 Environment 同时用。
  std::wstring dataDir = UtilDefaultWebView2BaseDir();
  if (dataDir.empty()) {
    ::DestroyWindow(s->hwnd);
    s->hwnd = nullptr;
    delete p;
    return nullptr;
  }
  dataDir += L"\\WebView2-Settings";

  // 已经在别的套间里初始化过就沿用，WebView2 两种套间都能跑。
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, dataDir.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [s](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (!Alive(s)) return S_OK;  // 设置窗口已经关了
            if (FAILED(result) || !env) {
              LogLine(L"SETTINGS: 预览 Environment 建不起来 hr=0x%08lX", result);
              return S_OK;
            }
            s->env = env;
            env->CreateCoreWebView2Controller(
                s->hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [s](HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT {
                      if (!Alive(s)) return S_OK;
                      if (FAILED(r2) || !ctrl) {
                        LogLine(L"SETTINGS: 预览 Controller 建不起来 hr=0x%08lX", r2);
                        return S_OK;
                      }
                      s->controller = ctrl;
                      RECT client = {};
                      ::GetClientRect(s->hwnd, &client);
                      ctrl->put_Bounds(client);
                      ctrl->put_IsVisible(TRUE);
                      if (FAILED(ctrl->get_CoreWebView2(&s->web)) || !s->web) return S_OK;

                      // 设置窗口里不需要右键菜单、状态栏、缩放这些浏览器的痕迹
                      ComPtr<ICoreWebView2Settings> settings;
                      if (SUCCEEDED(s->web->get_Settings(&settings)) && settings) {
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE);
                        settings->put_IsZoomControlEnabled(FALSE);
                      }
                      s->web->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [s](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)
                                  -> HRESULT {
                                if (!Alive(s)) return S_OK;
                                s->ready = true;
                                PushFont(s);
                                return S_OK;
                              })
                              .Get(),
                          nullptr);
                      s->web->NavigateToString(kPreviewHtml);
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    LogLine(L"SETTINGS: 预览 Environment 创建失败 hr=0x%08lX", hr);
  }
  return p;
}

void SettingsPreviewResize(SettingsPreview* p, const RECT& rc) {
  if (!p || !p->s || !p->s->hwnd) return;
  std::shared_ptr<PreviewState> s = p->s;
  ::SetWindowPos(s->hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  if (s->controller) {
    RECT client = {};
    ::GetClientRect(s->hwnd, &client);
    s->controller->put_Bounds(client);
  }
}

void SettingsPreviewSetFont(SettingsPreview* p, const std::vector<std::wstring>& chain,
                            int sizePx) {
  if (!p || !p->s) return;
  p->s->css = BuildCssList(chain);
  p->s->sizePx = sizePx > 0 ? sizePx : 15;
  PushFont(p->s);
}

void SettingsPreviewDestroy(SettingsPreview* p) {
  if (!p) return;
  std::shared_ptr<PreviewState> s = p->s;
  if (s) {
    // 先把 hwnd 摘掉：还在路上的异步回调看到它为空就不再往下走
    HWND hwnd = s->hwnd;
    s->hwnd = nullptr;
    if (s->controller) s->controller->Close();
    s->web.Reset();
    s->controller.Reset();
    s->env.Reset();
    if (hwnd && ::IsWindow(hwnd)) ::DestroyWindow(hwnd);
  }
  delete p;
}
