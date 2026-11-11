// settings_preview.cpp - 见 settings_preview.h。
//
// 流程：建子窗口 → 异步建 Environment（独立用户数据目录）→ 建 Controller →
// NavigateToString 一个极简页面 → 之后只靠 ExecuteScript 改 font-family 与字号。
//
// 生命周期：内部状态由 shared_ptr 持有，异步回调各拿一份。关窗或清理缓存换状态时
// 都把旧状态的 hwnd 置空并释放 COM 引用，晚到的回调看到 hwnd 为空直接退出——
// 否则「开了就关」会让回调去读已经作废的实例。清理缓存复用同一个窗口，只换状态，
// 设置页的显隐与布局都不用改。

#include "settings_preview.h"

#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>

#include <memory>
#include <new>

#include "config.h"
#include "log.h"
#include "util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kPreviewClass[] = L"DopusMdViewerSettingsPreview";

// 内置示例文字：配置为空时用这一份，弹窗里「恢复默认」也填这一份。
const wchar_t kDefaultPreviewText[] =
    L"中文示例：永和九年，岁在癸丑\nEnglish sample 0123 — Aa Bb Cc";

// 预览页：默认两行示例，文字、字体与字号都由脚本改，避免每次改动重建页面。
// 文字走 textContent，自定义内容里的尖括号不会被当成 HTML；#txt 自己滚动，
// 行数多了也不会被 flex 居中裁掉顶部。底色与字色走 CSS 变量，由 __mdvTheme
// 切到深色一组，页面不会是白块。
const wchar_t kPreviewHtml[] =
    L"<!doctype html><html><head><meta charset=\"utf-8\"><style>"
    L":root{--mdv-bg:#fff;--mdv-fg:#1a1a1a;--mdv-dim:#5a5a5a}"
    L"html.mdv-dark{--mdv-bg:#000;--mdv-fg:#d0d0d0;--mdv-dim:#969696}"
    L"html,body{margin:0;height:100%;background:var(--mdv-bg);color:var(--mdv-fg);"
    L"font-family:'Segoe UI';overflow:hidden}"
    L"#box{display:flex;flex-direction:column;justify-content:center;height:100%;"
    L"padding:0 10px;box-sizing:border-box;overflow:hidden}"
    L"#txt{max-height:100%;overflow-y:auto;scrollbar-width:thin}"
    L".l1,.l2{line-height:1.5;white-space:pre-wrap;overflow-wrap:anywhere}"
    L".l2{color:var(--mdv-dim)}"
    L"</style></head><body><div id=box><div id=txt>"
    L"<div class=l1 id=a>中文示例：永和九年，岁在癸丑</div>"
    L"<div class=l2 id=b>English sample 0123 — Aa Bb Cc</div>"
    L"</div></div><script>window.__mdv=function(f,s){"
    L"document.getElementById('txt').style.fontFamily=f;"
    L"document.getElementById('a').style.fontSize=s+'px';"
    L"document.getElementById('b').style.fontSize=s+'px';};"
    L"window.__mdvText=function(t){var p=String(t).split('\\n');"
    L"document.getElementById('a').textContent=p.length?p[0]:'';"
    L"document.getElementById('b').textContent=p.length>1?p.slice(1).join('\\n'):'';};"
    L"window.__mdvTheme=function(d){document.documentElement.classList.toggle('mdv-dark',d);}"
    L"</script></body></html>";

// 本模块的 HINSTANCE：DLL 里不能用 GetModuleHandle(nullptr)（那是宿主 exe）。
HMODULE ThisModule() {
  HMODULE mod = nullptr;
  ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SettingsPreviewCreate), &mod);
  return mod;
}

// CSS 通用族名：加了引号就变成"找这个字体文件"，通用回退随之失效。
bool IsGenericFamily(const std::wstring& name) {
  static const wchar_t* const kGenerics[] = {
      L"system-ui", L"sans-serif",    L"serif",     L"monospace",    L"cursive",
      L"fantasy",   L"ui-sans-serif", L"ui-serif",  L"ui-monospace", L"ui-rounded",
      L"emoji",     L"math",          L"fangsong",
  };
  for (const wchar_t* g : kGenerics) {
    if (::CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, name.c_str(), -1, g, -1) ==
        CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

// 与 msg.cpp 的 FontStackToCss 同口径：通用族名与不含空格的族名都裸写，
// 只有带空格的才加引号（引号里的反斜杠与引号本身要转义）。
std::wstring QuoteFamily(const std::wstring& name) {
  if (IsGenericFamily(name) || name.find(L' ') == std::wstring::npos) return name;
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

// CSS 字体链与预览文字都要作为 JS 字符串实参传进去，不能直接拼进实参列表：
// 链里的逗号会被当成参数分隔符，字号就落到了第三个参数上被浏览器丢掉。
// 控制字符与 U+2028/2029 一律写成 \uXXXX：后两个在 JS 里等价于换行，直接放进
// 字符串字面量会截断脚本；其余控制字符写成转义才不会丢字。
std::wstring JsString(const std::wstring& text) {
  static const wchar_t kHex[] = L"0123456789ABCDEF";
  std::wstring out = L"\"";
  for (wchar_t c : text) {
    if (c == L'\\' || c == L'"') {
      out.push_back(L'\\');
      out.push_back(c);
    } else if (c == L'\n') {
      out += L"\\n";
    } else if (c == L'\r') {
      out += L"\\r";
    } else if (c == L'\t') {
      out += L"\\t";
    } else if (c < 0x20 || c == 0x2028 || c == 0x2029) {
      const unsigned u = static_cast<unsigned>(c);
      out += L"\\u";
      out.push_back(kHex[(u >> 12) & 0xF]);
      out.push_back(kHex[(u >> 8) & 0xF]);
      out.push_back(kHex[(u >> 4) & 0xF]);
      out.push_back(kHex[u & 0xF]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back(L'"');
  return out;
}

struct PreviewState {
  HWND hwnd = nullptr;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> web;
  bool ready = false;
  bool dark = false;
  std::wstring css = L"'Segoe UI', sans-serif";
  int sizePx = 15;
  std::wstring text = kDefaultPreviewText;
};

// WebView2 建起来之前窗口是空的：类背景刷不跟宿主主题走，会先露一块白，
// 所以自己刷。子类化只挂在这个窗口实例上——窗口类的 lpfnWndProc 仍是
// DefWindowProcW，DLL 卸载后残留的旧类不会指向已卸载的代码。
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) {
    auto* s = reinterpret_cast<PreviewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (s) {
      RECT rc = {};
      ::GetClientRect(hwnd, &rc);
      HBRUSH bg = ::CreateSolidBrush(s->dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
      ::FillRect(reinterpret_cast<HDC>(wp), &rc, bg);
      ::DeleteObject(bg);
      return 1;
    }
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// 窗口还在吗？晚到的异步回调靠它判断要不要继续。
bool Alive(const std::shared_ptr<PreviewState>& s) {
  return s && s->hwnd && ::IsWindow(s->hwnd);
}

// 页面就绪前后都调用：就绪了就立刻下发，否则留着等导航完成。
void PushFont(const std::shared_ptr<PreviewState>& s) {
  if (!Alive(s) || !s->ready || !s->web) return;
  std::wstring script = L"window.__mdv(" + JsString(s->css) + L"," +
                        std::to_wstring(s->sizePx) + L");";
  s->web->ExecuteScript(script.c_str(), nullptr);
}

void PushText(const std::shared_ptr<PreviewState>& s) {
  if (!Alive(s) || !s->ready || !s->web) return;
  std::wstring script = L"window.__mdvText(" + JsString(s->text) + L");";
  s->web->ExecuteScript(script.c_str(), nullptr);
}

void PushTheme(const std::shared_ptr<PreviewState>& s) {
  if (!Alive(s) || !s->ready || !s->web) return;
  std::wstring script = L"window.__mdvTheme(" + std::wstring(s->dark ? L"true" : L"false") +
                        L");";
  s->web->ExecuteScript(script.c_str(), nullptr);
}

}  // namespace

struct SettingsPreview {
  std::shared_ptr<PreviewState> s;
};

HWND SettingsPreviewHwnd(SettingsPreview* p) { return p && p->s ? p->s->hwnd : nullptr; }

namespace {

// 建 Environment → Controller → 导航极简页面。清理缓存后会在同一个窗口上换一份
// 新的 PreviewState 再走一遍，所以这段必须能重复调用，且只依赖传入的状态。
void StartEnvironment(const std::shared_ptr<PreviewState>& s, const std::wstring& dataDir) {
  // 目录可能刚被清理按钮删掉；建环境前备好它，不把这件事推给运行时。
  if (!UtilEnsureDir(dataDir)) {
    LogLine(L"SETTINGS: 预览缓存目录不可写 %s", dataDir.c_str());
    return;
  }

  // 已经在别的套间里初始化过就沿用，WebView2 两种套间都能跑。同一线程重复
  // CoInitializeEx 只会累增引用计数，这里只做一次。
  static bool comReady = false;
  if (!comReady) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comReady = true;
  }

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
                                PushText(s);
                                PushTheme(s);
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
}

}  // namespace

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
    // 同名旧类可能来自上一次加载的 DLL：它的窗口过程也是 DefWindowProcW，
    // 直接复用不会指向已卸载的代码。
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return nullptr;
    }
    registered = true;
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
  ::SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s.get()));
  ::SetWindowLongPtrW(s->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PreviewWndProc));

  // 用户数据目录独立：同一份目录不能被两个 Environment 同时用。位置与查看器缓存
  // 同一个父目录（U 盘模式下按同一套选路），设置里改了缓存位置这里也跟着走。
  std::wstring dataDir = UtilWebView2SettingsDir(ConfigWebViewBaseDir(ConfigSnapshot()));
  if (dataDir.empty()) {
    ::DestroyWindow(s->hwnd);
    s->hwnd = nullptr;
    delete p;
    return nullptr;
  }

  StartEnvironment(s, dataDir);
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

const wchar_t* SettingsPreviewDefaultText(void) { return kDefaultPreviewText; }

void SettingsPreviewSetText(SettingsPreview* p, const std::wstring& text) {
  if (!p || !p->s || p->s->text == text) return;
  p->s->text = text;
  PushText(p->s);
}

void SettingsPreviewSetTheme(SettingsPreview* p, bool dark) {
  if (!p || !p->s || p->s->dark == dark) return;
  p->s->dark = dark;
  PushTheme(p->s);
  // 页面还没导航完时，先把窗口底色换成同一档，免得加载期间闪一块白。
  if (p->s->hwnd) ::InvalidateRect(p->s->hwnd, nullptr, TRUE);
}

// 释放预览实例 → 删掉它的用户数据目录 → 在同一个窗口上用新状态重建。
// 旧状态摘掉 hwnd 后，在路上的异步回调会自行退出；窗口句柄不变，设置页的
// 页显隐表、DPI 重排表不用改，字体链与配色也能原样续上。
bool SettingsPreviewClearCache(SettingsPreview* p, const std::wstring& userDataDir) {
  if (!p || !p->s || !p->s->hwnd || !UtilIsWebView2UserDataDir(userDataDir)) return false;

  const HWND hwnd = p->s->hwnd;
  const std::shared_ptr<PreviewState> old = p->s;
  old->hwnd = nullptr;
  if (old->controller) old->controller->Close();
  old->web.Reset();
  old->controller.Reset();
  old->env.Reset();

  // 浏览器进程退出是异步的，给目录一点时间放手。删不掉也照常重建，
  // 不能因为一次清理失败就把预览永久留成空白。
  bool removed = false;
  for (int i = 0; i < 20; ++i) {
    if (!UtilDirExists(userDataDir)) {
      removed = true;
      break;
    }
    if (UtilRemoveTree(userDataDir)) {
      removed = true;
      break;
    }
    ::Sleep(50);
  }
  if (!removed) {
    LogLine(L"SETTINGS: 预览缓存清理失败（目录仍被占用）%s", userDataDir.c_str());
  }

  auto next = std::make_shared<PreviewState>();
  next->hwnd = hwnd;
  next->css = old->css;
  next->sizePx = old->sizePx;
  next->text = old->text;
  next->dark = old->dark;
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(next.get()));
  p->s = next;
  StartEnvironment(next, userDataDir);
  return removed;
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
