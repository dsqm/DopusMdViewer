// hosttest.cpp - 把查看器与 WebView2 这条链路搬到 Opus 外面跑。
//
// 它不复制任何渲染逻辑：直接调用 src\viewer_window.cpp 的 ViewerCreate 与
// DVPLUGINMSG_LOADW，所以跑通就等于「Opus 里那条路」跑通，只是省掉每次重启 Opus。
// 用来验证：宿主窗口、controller、mdprev.local 映射、CSP 头、皮肤/字体/正文推送。
//
// 用法：hosttest.exe "C:\path\to\sample.md"

#include <windows.h>

#include <new>
#include <stdlib.h>
#include <objidl.h>
#include <strsafe.h>

#include <viewer plugins.h>  // 在 ..\SDK\Headers，走 INCLUDE

#include "../../src/viewer_window.h"
#include "../../src/settings_dlg.h"
#include "../../src/webhost.h"

namespace {

const wchar_t kMainClass[] = L"DopusMdViewerHostTest";
const UINT_PTR kQuitTimerId = 1;
const UINT_PTR kShotTimerId = 2;
const UINT_PTR kCycleTimerId = 3;
const UINT_PTR kTwoTimerId = 4;
const UINT_PTR kTwoCloseTimerId = 5;
const UINT_PTR kScrollTimerId = 6;
const UINT_PTR kSettingsTimerId = 7;
const UINT_PTR kDumpTimerId = 8;
const UINT_PTR kSeamTimerId = 9;
const UINT kTwoAfterMs = 3000;
const UINT kTwoCloseAfterMs = 6500;  // two 模式下关掉第二个查看器，验宿主交回
const UINT kCycleAfterMs = 5000;
const UINT kScrollAfterMs = 5000;
const UINT kSettingsAfterMs = 3000;
const UINT kDumpAfterMs = 8000;
const UINT kQuitAfterMs = 40000;
const UINT kShotAfterDefaultMs = 5000;

wchar_t g_szFile[MAX_PATH * 2] = L"";
wchar_t g_szShot[MAX_PATH * 2] = L"";
UINT g_shotAfterMs = kShotAfterDefaultMs;
bool g_cycle = false;  // 中途销毁查看器再建一个，复现"切走再切回"
bool g_twoThreads = false;  // 第二个查看器开在另一个线程上，复现预览窗格 + 独立查看器
bool g_scroll = false;      // 5 秒后滚动到底，验虚拟滚动与滚动条
bool g_settings = false;    // 3 秒后打开设置窗口，验 DVP_Configure 与 config 写盘
bool g_dump = false;        // 5 秒后把视口/常驻块/段几何回传到日志
bool g_seam = false;        // 5 秒后滚到第 10 万行（第一段与第二段的接缝）
HWND g_hwndMain = nullptr;
HWND g_viewer = nullptr;
HINSTANCE g_hInst = nullptr;

// 建一个查看器并让它加载当前文件。cycle 模式用它做第二次。
HWND CreateViewerOnce() {
  RECT rc;
  ::GetClientRect(g_hwndMain, &rc);

  HWND viewer = ViewerCreate(g_hwndMain, &rc, 0);
  if (!viewer) return nullptr;

  if (g_szFile[0]) {
    ::PostMessageW(viewer, DVPLUGINMSG_LOADW, 0, reinterpret_cast<LPARAM>(g_szFile));
  }
  return viewer;
}

const wchar_t kThreadFrameClass[] = L"DopusMdViewerHostTestThread";

struct ThreadCtx {
  HWND frame;
  HWND viewer;
  DWORD tid;
};

HANDLE g_thread = nullptr;
ThreadCtx* g_threadCtx = nullptr;

LRESULT CALLBACK ThreadFrameProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (uMsg == WM_DESTROY) {
    ::PostQuitMessage(0);
    return 0;
  }
  return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// Opus 的独立查看器窗口跑在另一个 UI 线程上，WebView2 的对象是线程绑定的。
// 这个线程照做一遍：自己的窗口、自己的消息泵、自己的查看器。
DWORD WINAPI SecondThreadProc(LPVOID param) {
  ThreadCtx* ctx = static_cast<ThreadCtx*>(param);
  ctx->tid = ::GetCurrentThreadId();

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = ThreadFrameProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = kThreadFrameClass;
  ::RegisterClassExW(&wc);

  ctx->frame = ::CreateWindowExW(0, kThreadFrameClass, L"thread2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 1080, 60, 620, 470, nullptr, nullptr, g_hInst, nullptr);

  RECT rc;
  ::GetClientRect(ctx->frame, &rc);
  ctx->viewer = ViewerCreate(ctx->frame, &rc, 0);
  if (ctx->viewer && g_szFile[0]) {
    ::PostMessageW(ctx->viewer, DVPLUGINMSG_LOADW, 0, reinterpret_cast<LPARAM>(g_szFile));
  }

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  return 0;
}

// 从屏幕抓客户区，写成 BMP。WebView2 的内容由合成器直接上屏，PrintWindow 拿不到，
// 只能从窗口实际占的屏幕区域取。
bool SaveClientBmp(const wchar_t* path) {
  if (!g_hwndMain || !path || !*path) return false;

  RECT rc;
  ::GetWindowRect(g_hwndMain, &rc);
  const int cx = rc.right - rc.left;
  const int cy = rc.bottom - rc.top;
  if (cx <= 0 || cy <= 0) return false;

  const POINT origin = {rc.left, rc.top};

  HDC hScreen = ::GetDC(nullptr);
  HDC hMem = ::CreateCompatibleDC(hScreen);
  HBITMAP hbm = ::CreateCompatibleBitmap(hScreen, cx, cy);
  HGDIOBJ oldObj = ::SelectObject(hMem, hbm);
  ::BitBlt(hMem, 0, 0, cx, cy, hScreen, origin.x, origin.y, SRCCOPY);
  ::SelectObject(hMem, oldObj);

  BITMAPINFOHEADER bih = {};
  bih.biSize = sizeof(bih);
  bih.biWidth = cx;
  bih.biHeight = cy;  // 正数 = 自下而上
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;

  const DWORD cbBits = static_cast<DWORD>(cx) * cy * 4;
  BITMAPFILEHEADER bfh = {};
  bfh.bfType = 0x4D42;  // 'BM'
  bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
  bfh.bfSize = bfh.bfOffBits + cbBits;

  bool ok = false;
  HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ok = ::WriteFile(h, &bfh, sizeof(bfh), &written, nullptr) && written == sizeof(bfh);
    if (ok) ok = ::WriteFile(h, &bih, sizeof(bih), &written, nullptr) && written == sizeof(bih);

    // GetDIBits 要一个自下而上的缓冲区
    BYTE* bits = new (std::nothrow) BYTE[cbBits];
    if (ok && bits) {
      BITMAPINFO bi = {};
      bi.bmiHeader = bih;
      if (::GetDIBits(hMem, hbm, 0, cy, bits, &bi, DIB_RGB_COLORS) != 0) {
        ok = ::WriteFile(h, bits, cbBits, &written, nullptr) && written == cbBits;
      } else {
        ok = false;
      }
    } else {
      ok = false;
    }
    delete[] bits;
    ::CloseHandle(h);
  }

  ::DeleteObject(hbm);
  ::DeleteDC(hMem);
  ::ReleaseDC(nullptr, hScreen);
  return ok;
}

void LayoutChildren() {
  RECT rc;
  ::GetClientRect(g_hwndMain, &rc);
  for (HWND child = ::GetWindow(g_hwndMain, GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    ::MoveWindow(child, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
  }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_SIZE:
      LayoutChildren();
      return 0;

    case WM_TIMER:
      if (wParam == kQuitTimerId) {
        ::KillTimer(hwnd, kQuitTimerId);
        ::PostQuitMessage(0);
      } else if (wParam == kShotTimerId) {
        ::KillTimer(hwnd, kShotTimerId);
        SaveClientBmp(g_szShot);
      } else if (wParam == kTwoTimerId) {
        ::KillTimer(hwnd, kTwoTimerId);
        g_threadCtx = new ThreadCtx();
        g_thread = ::CreateThread(nullptr, 0, SecondThreadProc, g_threadCtx, 0, nullptr);
      } else if (wParam == kTwoCloseTimerId) {
        // 关掉第二个查看器所在的窗口：宿主应当按借用历史交回主线程的那个，
        // 主窗口的画面要自己回来（这正是 Opus 里关掉独立查看器后侧边栏恢复的路径）。
        ::KillTimer(hwnd, kTwoCloseTimerId);
        if (g_threadCtx && g_threadCtx->frame) {
          ::PostMessageW(g_threadCtx->frame, WM_CLOSE, 0, 0);
        }
      } else if (wParam == kSettingsTimerId) {
        ::KillTimer(hwnd, kSettingsTimerId);
        SettingsShow(g_hwndMain);
      } else if (wParam == kScrollTimerId) {
        // 通过页面脚本走一遍 wheel 路径：更新 target、收敛动画、重新摆位、取块。
        ::KillTimer(hwnd, kScrollTimerId);
        const wchar_t* script =
            L"window.dispatchEvent(new WheelEvent('wheel',{deltaY:1000000000,deltaMode:0,"
            L"bubbles:true,cancelable:true}));"
            L"setTimeout(function(){var V=window.__mdv.V;"
            L"window.chrome.webview.postMessage('event|scrolltest|off='+Math.round(V.off)+"
            L"' total='+Math.round(V.totalH)+' blocks='+V.blockCount"
            L"+' win='+V.windowStart+'-'+V.windowEnd+' resident='+V.resident.size);},2500);"
            L"setTimeout(function(){var n=0,last=performance.now(),max=0,t0=last;"
            L"function step(t){window.dispatchEvent(new WheelEvent('wheel',{deltaY:-160,"
            L"deltaMode:0,cancelable:true}));var d=t-last;last=t;if(d>max)max=d;n++;"
            L"if(t-t0<1200)requestAnimationFrame(step);"
            L"else window.chrome.webview.postMessage('event|fps|frames='+n+\" max=\"+Math.round(max));}"
            L"requestAnimationFrame(step);},3000);";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kDumpTimerId) {
        ::KillTimer(hwnd, kDumpTimerId);
        const wchar_t* script =
            L"(function(){var V=window.__mdv.V,out=[];"
            L"out.push('off='+Math.round(V.off)+' sb='+Math.round(V.stageBase)+' total='+Math.round(V.totalH)"
            L"+' win='+V.windowStart+'-'+V.windowEnd+' big='+V.big.size);"
            L"for(const [i,rec] of V.resident){"
            L" if(rec.big){for(const [k,seg] of rec.big.segs){"
            L"  var r=seg.el.getBoundingClientRect(),q=seg.pre.getBoundingClientRect();"
            L"  out.push('seg'+k+' top='+Math.round(parseFloat(seg.el.style.top))+' h='+Math.round(parseFloat(seg.el.style.height))"
            L"+' rect='+Math.round(r.top)+','+Math.round(r.left)+','+Math.round(r.width)+'x'+Math.round(r.height)"
            L"+' pre='+Math.round(q.top)+','+Math.round(q.left)+' tf='+seg.pre.style.transform+' txt='+seg.code.textContent.length);}}"
            L" else if(rec.el){var r=rec.el.getBoundingClientRect();"
            L"  out.push('blk'+i+' rect='+Math.round(r.top)+','+Math.round(r.left)+','+Math.round(r.width)+'x'+Math.round(r.height));}}"
            L"var d=document.getElementById('doc'),dr=d.getBoundingClientRect();"
            L"out.push('doc tf='+d.style.transform+' rect='+Math.round(dr.left)+','+Math.round(dr.width));"
            L"window.chrome.webview.postMessage('event|dump|'+out.join(' ; '));})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kSeamTimerId) {
        ::KillTimer(hwnd, kSeamTimerId);
        // 段边界 10 万行：把视口对到接缝上，肉眼查边框是否连续
        const wchar_t* script =
            L"(function(){var V=window.__mdv.V;"
            L"var y=40.1+100000*24.75;"
            L"V.off=V.target=Math.max(0,y-120);"
            L"window.dispatchEvent(new WheelEvent('wheel',{deltaY:0,deltaMode:0,"
            L"bubbles:true,cancelable:true}));})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kCycleTimerId) {
        // Opus 换文件时会销毁并重建查看器；这里照做一遍，验证宿主与 controller 能否撑住
        ::KillTimer(hwnd, kCycleTimerId);
        if (g_viewer) {
          ::DestroyWindow(g_viewer);
          g_viewer = nullptr;
        }
        g_viewer = CreateViewerOnce();
        LayoutChildren();
      }
      return 0;

    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

// 命令行里的路径可能带引号；第二个参数是可选截图延时（毫秒）
void ExtractArgs(LPWSTR pszCmdLine) {
  if (!pszCmdLine) return;
  wchar_t* p = pszCmdLine;
  while (*p == L' ' || *p == L'\t') ++p;

  if (*p == L'"') {
    ++p;
    const wchar_t* pEnd = p;
    while (*pEnd && *pEnd != L'"') ++pEnd;
    ::StringCchCopyNW(g_szFile, ARRAYSIZE(g_szFile), p,
                      static_cast<size_t>(pEnd - p));
    p = const_cast<wchar_t*>(pEnd);
    if (*p == L'"') ++p;
  } else {
    const wchar_t* pEnd = p;
    while (*pEnd && *pEnd != L' ' && *pEnd != L'\t') ++pEnd;
    ::StringCchCopyNW(g_szFile, ARRAYSIZE(g_szFile), p, static_cast<size_t>(pEnd - p));
    p = const_cast<wchar_t*>(pEnd);
  }

  while (*p == L' ' || *p == L'\t') ++p;
  if (*p) {
    const int ms = _wtoi(p);
    if (ms > 0) g_shotAfterMs = static_cast<UINT>(ms);
    while (*p && *p != L' ' && *p != L'\t') ++p;
  }

  while (*p == L' ' || *p == L'\t') ++p;
  if (*p && wcsstr(p, L"cycle")) g_cycle = true;
  if (*p && wcsstr(p, L"two")) g_twoThreads = true;
  if (*p && wcsstr(p, L"scroll")) g_scroll = true;
  if (*p && wcsstr(p, L"settings")) g_settings = true;
  if (*p && wcsstr(p, L"dump")) g_dump = true;
  if (*p && wcsstr(p, L"seam")) g_seam = true;

  wchar_t szTemp[MAX_PATH];
  const DWORD n = ::GetTempPathW(MAX_PATH, szTemp);
  if (n > 0 && n < MAX_PATH) {
    ::StringCchPrintfW(g_szShot, ARRAYSIZE(g_szShot), L"%shosttest-shot.bmp", szTemp);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
  (void)hPrev;
  (void)nCmdShow;

  // 不声明 DPI 感知的话，窗口坐标被系统虚拟化，BitBlt 会抓到错位的区域
  if (!::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    ::SetProcessDPIAware();
  }

  g_hInst = hInstance;
  ExtractArgs(lpCmdLine);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = MainProc;
  wc.hInstance = hInstance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kMainClass;
  if (!::RegisterClassExW(&wc)) return 2;

  g_hwndMain = ::CreateWindowExW(0, kMainClass, L"DopusMdViewer host test",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 1000, 780, nullptr,
                                 nullptr, hInstance, nullptr);
  if (!g_hwndMain) return 3;

  if (!ViewerRegisterClass(hInstance)) return 4;

  g_viewer = CreateViewerOnce();
  if (!g_viewer) return 5;

  LayoutChildren();

  if (g_cycle) {
    ::SetTimer(g_hwndMain, kCycleTimerId, kCycleAfterMs, nullptr);
  }
  if (g_twoThreads) {
    ::SetTimer(g_hwndMain, kTwoTimerId, kTwoAfterMs, nullptr);
    ::SetTimer(g_hwndMain, kTwoCloseTimerId, kTwoCloseAfterMs, nullptr);
  }
  if (g_scroll) {
    ::SetTimer(g_hwndMain, kScrollTimerId, kScrollAfterMs, nullptr);
  }
  if (g_settings) {
    ::SetTimer(g_hwndMain, kSettingsTimerId, kSettingsAfterMs, nullptr);
  }
  if (g_dump) {
    ::SetTimer(g_hwndMain, kDumpTimerId, kDumpAfterMs, nullptr);
  }
  if (g_seam) {
    ::SetTimer(g_hwndMain, kSeamTimerId, kDumpAfterMs, nullptr);
  }

  if (g_szShot[0]) {
    ::SetForegroundWindow(g_hwndMain);
    ::SetTimer(g_hwndMain, kShotTimerId, g_shotAfterMs, nullptr);
  }
  ::SetTimer(g_hwndMain, kQuitTimerId, kQuitAfterMs, nullptr);

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  return 0;
}
