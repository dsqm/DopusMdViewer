// hosttest.cpp - 把查看器与 WebView2 这条链路搬到 Opus 外面跑。
//
// 它不复制任何渲染逻辑：直接调用 src\viewer_window.cpp 的 ViewerCreate 与
// DVPLUGINMSG_LOADW，所以跑通就等于「Opus 里那条路」跑通，只是省掉每次重启 Opus。
// 用来验证：宿主窗口、controller、mdprev.localhost 映射、CSP 头、皮肤/字体/正文推送。
//
// 用法：hosttest.exe "C:\path\to\sample.md"

#include <windows.h>

#include <new>
#include <stdlib.h>
#include <string>
#include <objidl.h>
#include <strsafe.h>

#include <viewer plugins.h>  // 在 ..\SDK\Headers，走 INCLUDE

#include "../../src/viewer_window.h"
#include "../../src/settings_dlg.h"
#include "../../src/webhost.h"
#include "../../src/log.h"

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
const UINT_PTR kConsentTimerId = 10;
const UINT_PTR kReloadTimerId = 11;
const UINT_PTR kFindTimerId = 12;
const UINT_PTR kOutlineTimerId = 13;
const UINT_PTR kPrewarmTimerId = 14;
const UINT_PTR kReleaseCloseTimerId = 15;
const UINT_PTR kReleaseQuitTimerId = 16;
const UINT_PTR kReleaseReopenTimerId = 17;
const UINT_PTR kSlugTimerId = 18;
const UINT_PTR kDropCloseTimerId = 19;
const UINT_PTR kCopyTimerId = 20;
const UINT_PTR kCopyCheckTimerId = 21;
const UINT_PTR kPinTimerId = 23;
const UINT_PTR kPrintTimerId = 26;
const UINT_PTR kCopyLateTimerId = 27;
const UINT_PTR kFindPingTimerId = 28;
const UINT_PTR kFindPing2TimerId = 29;
const UINT_PTR kFindRaceTimerId = 30;
const UINT kTwoAfterMs = 3000;
const UINT kTwoCloseAfterMs = 6500;  // two 模式下关掉第二个查看器，验宿主交回
const UINT kCycleAfterMs = 5000;
const UINT kScrollAfterMs = 5000;
const UINT kSettingsAfterMs = 3000;
const UINT kDumpAfterMs = 8000;
const UINT kQuitAfterMs = 40000;
const UINT kShotAfterDefaultMs = 5000;
const UINT kPrewarmAfterMs = 7000;      // 预热 3 秒开始、页面约 2.6 秒就绪，7 秒时借用
const UINT kReleaseCloseAfterMs = 5000; // 销毁主窗口，模拟 DO 窗口关闭
const UINT kReleaseReopenAfterMs = 10000; // 释放后重新开窗口，验 Environment 重建
const UINT kReleaseQuitAfterMs = 19000;
const UINT kDropQuitAfterMs = 80000;

wchar_t g_szFile[MAX_PATH * 2] = L"";
wchar_t g_szShot[MAX_PATH * 2] = L"";
UINT g_shotAfterMs = kShotAfterDefaultMs;
bool g_cycle = false;  // 中途销毁查看器再建一个，复现"切走再切回"
bool g_twoThreads = false;  // 第二个查看器开在另一个线程上，复现预览窗格 + 独立查看器
bool g_scroll = false;      // 5 秒后滚动到底，验虚拟滚动与滚动条
bool g_settings = false;    // 3 秒后打开设置窗口，验 DVP_Configure 与 config 写盘
bool g_dump = false;        // 5 秒后把视口/常驻块/段几何回传到日志
bool g_seam = false;        // 5 秒后滚到第 10 万行（第一段与第二段的接缝）
bool g_consent = false;     // 5 秒后点页内网络图确认面板的“允许”，4 秒后回流图片状态
bool g_reload = false;      // 6.5 秒后重新 LOADW 同一个文件，验进程内记忆跨文件保留
bool g_find = false;        // 5 秒后打开搜索、查 g_findQuery、翻到下一个，回流计数与 mark
bool g_findWord = false;    // 配合 find：先打开整词匹配
bool g_findRace = false;    // 5 秒后连打两个查询：验证旧扫描被取消、后一问生效
bool g_prewarm = false;     // 先预热，7 秒后再建查看器，验首开耗时
bool g_release = false;     // 5 秒后销毁主窗口，验 DO 窗口关闭后实例释放
bool g_idle = false;        // 配合 prewarm：只预热、不建查看器，量常驻内存
bool g_drop = false;        // 关预热、设 1 分钟空闲释放；5 秒后只销毁查看器，验空闲回收
wchar_t g_findQuery[128] = L"中文";
bool g_outline = false;     // 5 秒后打开大纲、点第 3 项，回流条目数与滚动位置
bool g_slug = false;        // 5 秒后查标题 slug 表、点页内锚点，回流映射与滚动位置
bool g_copy = false;        // 5 秒后按 Ctrl+A，7 秒读剪贴板，9 秒再走 SELECTALL 消息
bool g_pin = false;         // 5 秒后选区钉块 + 拖边滚动，滚到底验钉块没被换出
bool g_print = false;       // 5 秒后准备打印容器（不弹对话框），回流块数/图表/图片
HWND g_hwndMain = nullptr;
HWND g_viewer = nullptr;
HWND g_hwndTimer = nullptr;  // release 模式：主窗口销毁后用它继续跑计时器
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

void LogClipboard(const wchar_t* tag) {
  if (!::OpenClipboard(g_hwndMain)) {
    LogLine(L"TEST: %s 剪贴板被占用", tag);
    return;
  }
  HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
  const wchar_t* text = data ? static_cast<const wchar_t*>(::GlobalLock(data)) : nullptr;
  if (text) {
    std::wstring all(text);
    const size_t tail = all.size() > 20 ? all.size() - 20 : 0;
    LogLine(L"TEST: %s %zu 字符 head=\"%.40s\" tail=\"%.20s\"", tag, all.size(), all.c_str(),
            all.c_str() + tail);
    ::GlobalUnlock(data);
  } else {
    LogLine(L"TEST: %s 剪贴板没有文本", tag);
  }
  ::CloseClipboard();
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
            L"var bl='';for(const [i,m] of V.big)bl+=i+':'+m.lines+',';"
            L"var ims=[].map.call(document.querySelectorAll('img'),function(t){"
            L"return (t.getAttribute('src')||'-').slice(0,72)+'|nw='+t.naturalWidth;});"
            L"out.push('imgs='+ims.join(' ; '));"
            L"out.push('panel='+(document.querySelector('.net-consent:not([hidden])')?1:0));"
            L"out.push('mermaid='+document.querySelectorAll('[data-mermaid=\"done\"]').length"
            L"+'/'+document.querySelectorAll('.mermaid-block').length);"
            L"var b5=document.querySelector('.blk[data-i=\"5\"]');"
            L"out.push('blk5='+String(b5&&b5.innerHTML).slice(0,240));"
            L"out.push('bigmap='+bl+' heights='+Array.from(V.heights.slice(0,4)).join(',')"
            L"+' default='+Math.round(V.defaultH)+' lastVis='+V.lastVisible);"
            L"window.chrome.webview.postMessage('event|dump|'+out.join(' ; '));})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kConsentTimerId) {
        ::KillTimer(hwnd, kConsentTimerId);
        const wchar_t* script =
            L"(function(){var b=document.querySelector('.net-consent:not([hidden])');"
            L"var shown=!!b;"
            L"if(b){var a=b.querySelector('[data-act=\"allow\"]');if(a)a.click();}"
            L"setTimeout(function(){var imgs=[].map.call("
            L"document.querySelectorAll('img[data-mdv-src]'),function(i){"
            L"return 'nw='+i.naturalWidth+'@'+String(i.getAttribute('src')).slice(0,48);});"
            L"window.chrome.webview.postMessage('event|consent|shown='+(shown?1:0)"
            L"+' imgs='+imgs.join(' ; '));},4000);})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kReloadTimerId) {
        ::KillTimer(hwnd, kReloadTimerId);
        if (g_viewer && g_szFile[0]) {
          ::PostMessageW(g_viewer, DVPLUGINMSG_LOADW, 0, reinterpret_cast<LPARAM>(g_szFile));
        }
      } else if (wParam == kFindTimerId) {
        ::KillTimer(hwnd, kFindTimerId);
        wchar_t script[4096];
        const wchar_t* wordBtn =
            g_findWord ? L"var wb=document.getElementById('find-word');if(wb)wb.click();" : L"";
        ::StringCchPrintfW(
            script, ARRAYSIZE(script),
            L"(function(){var b=document.querySelector('[data-act=\"find\"]');if(b)b.click();"
            L"%svar inp=document.getElementById('find-input');inp.value='%s';"
            L"inp.dispatchEvent(new Event('input',{bubbles:true}));"
            L"function rep(tag){var m=[].map.call(document.querySelectorAll('mark.hit'),"
            L"function(x){return x.textContent;});"
            L"window.chrome.webview.postMessage('event|find|'+tag"
            L"+' count='+document.getElementById('find-count').textContent"
            L"+' marks='+m.join('|')+' off='+Math.round(window.__mdv.V.off));}"
            L"setTimeout(function(){rep('first');var n=document.getElementById('find-next');"
            L"if(n)n.click();setTimeout(function(){rep('next');},700);},1500);"
            L"setTimeout(function(){var c=document.getElementById('find-case');if(c)c.click();"
            L"setTimeout(function(){rep('case');},1200);},3200);})();",
            wordBtn, g_findQuery);
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
        // 搜索期间向宿主线程要一次执行：同步扫描会把这条消息堵住几百毫秒。
        ::SetTimer(hwnd, kFindPingTimerId, 600, nullptr);
        ::SetTimer(hwnd, kFindPing2TimerId, 900, nullptr);
      } else if (wParam == kFindPingTimerId || wParam == kFindPing2TimerId) {
        ::KillTimer(hwnd, wParam);
        const ULONGLONG t0 = ::GetTickCount64();
        if (g_viewer) WebHostExecuteScript(g_viewer, L"1");
        LogLine(L"TEST: 搜索期间宿主线程响应 %llu ms", ::GetTickCount64() - t0);
      } else if (wParam == kFindRaceTimerId) {
        ::KillTimer(hwnd, kFindRaceTimerId);
        const wchar_t* script =
            L"(function(){var b=document.querySelector('[data-act=\"find\"]');if(b)b.click();"
            L"var inp=document.getElementById('find-input');"
            L"function type(v){inp.value=v;inp.dispatchEvent(new Event('input',"
            L"{bubbles:true}));}"
            L"type('parser');"
            L"setTimeout(function(){type('parsery');},400);"
            L"setTimeout(function(){var m=[].map.call(document.querySelectorAll('mark.hit'),"
            L"function(x){return x.textContent;});"
            L"window.chrome.webview.postMessage('event|findrace|count='"
            L"+document.getElementById('find-count').textContent+' marks='+m.join('|'));},3000);"
            L"})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kOutlineTimerId) {
        ::KillTimer(hwnd, kOutlineTimerId);
        const wchar_t* script =
            L"(function(){var b=document.querySelector('[data-act=\"outline\"]');if(b)b.click();"
            L"setTimeout(function(){var items=document.querySelectorAll('.outline-item');"
            L"var n=items.length;var k=n?Math.min(2,n-1):0;"
            L"var t=n?items[k].textContent:'';if(n)items[k].click();"
            L"setTimeout(function(){window.chrome.webview.postMessage('event|outline|items='+n"
            L"+' text='+t+' off='+Math.round(window.__mdv.V.off)"
            L"+' open='+(document.getElementById('outline').hidden?0:1));},700);},400);})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kCopyTimerId) {
        ::KillTimer(hwnd, kCopyTimerId);
        const wchar_t* script =
            L"(function(){var V=window.__mdv.V;"
            L"function rep(tag){var s=window.getSelection();"
            L"window.chrome.webview.postMessage('event|copy|'+tag"
            L"+' pinned='+V.pinned.size+' collapsed='+(s.isCollapsed?1:0)"
            L"+' ranges='+s.rangeCount);}"
            L"window.dispatchEvent(new KeyboardEvent('keydown',{key:'a',ctrlKey:true,"
            L"bubbles:true,cancelable:true}));"
            L"setTimeout(function(){rep('selected');"
            L"window.dispatchEvent(new KeyboardEvent('keydown',{key:'c',ctrlKey:true,"
            L"bubbles:true,cancelable:true}));},400);})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kCopyCheckTimerId) {
        ::KillTimer(hwnd, kCopyCheckTimerId);
        LogClipboard(L"Ctrl+C");
        // 再走一遍 Opus 菜单「全选」的 SELECTALL 消息路径（只选中，不复制）
        if (g_viewer) ::PostMessageW(g_viewer, DVPLUGINMSG_SELECTALL, 0, 0);
        ::SetTimer(hwnd, kCopyLateTimerId, 25000, nullptr);
      } else if (wParam == kCopyLateTimerId) {
        ::KillTimer(hwnd, kCopyLateTimerId);
        LogClipboard(L"Ctrl+C-late");
      } else if (wParam == kPrintTimerId) {
        ::KillTimer(hwnd, kPrintTimerId);
        const wchar_t* script =
            L"window.__mdv.printTest().then(function(r){"
            L"window.chrome.webview.postMessage('event|print|'+r);})"
            L".catch(function(e){window.chrome.webview.postMessage('event|print|err='"
            L"+(e&&e.message||e));});";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kPinTimerId) {
        ::KillTimer(hwnd, kPinTimerId);
        const wchar_t* script =
            L"(function(){var V=window.__mdv.V;"
            L"var a=document.querySelector('.blk[data-i=\"1\"]'),"
            L"b=document.querySelector('.blk[data-i=\"5\"]');"
            L"var sel=window.getSelection();var r=document.createRange();"
            L"r.setStart(a,0);r.setEnd(b,b.childNodes.length);"
            L"sel.removeAllRanges();sel.addRange(r);"
            L"function rep(tag){var s=window.getSelection();"
            L"window.chrome.webview.postMessage('event|pin|'+tag+' pinned='+V.pinned.size"
            L"+' resident='+V.resident.size+' has1='+(V.resident.has(1)?1:0)"
            L"+' has5='+(V.resident.has(5)?1:0)+' collapsed='+(s.isCollapsed?1:0)"
            L"+' target='+Math.round(V.target)+' off='+Math.round(V.off));}"
            L"setTimeout(function(){rep('sel');"
            L"document.dispatchEvent(new MouseEvent('mousedown',{button:0,clientX:200,clientY:120,"
            L"bubbles:true,cancelable:true}));"
            L"document.dispatchEvent(new MouseEvent('mousemove',{clientX:200,"
            L"clientY:window.innerHeight-2,bubbles:true,cancelable:true}));},300);"
            L"setTimeout(function(){rep('autoscroll');"
            L"document.dispatchEvent(new MouseEvent('mouseup',{button:0,bubbles:true}));"
            L"window.dispatchEvent(new WheelEvent('wheel',{deltaY:1000000000,deltaMode:0,"
            L"bubbles:true,cancelable:true}));},1200);"
            L"setTimeout(function(){rep('scrolled');sel.removeAllRanges();},2600);"
            L"setTimeout(function(){rep('cleared');},3200);})();";
        if (g_viewer) WebHostExecuteScript(g_viewer, script);
      } else if (wParam == kSlugTimerId) {
        ::KillTimer(hwnd, kSlugTimerId);
        const wchar_t* script =
            L"(function(){var out;try{"
            L"var S=window.__mdv.SLUG,V=window.__mdv.V;"
            L"var ids=[];for(const [s,b] of S.bySlug)ids.push(s+'@'+b);"
            L"var hid=[].map.call(document.querySelectorAll('h1,h2,h3'),function(h){"
            L"return (h.id||'-')+'='+h.textContent.slice(0,8);});"
            L"var a=document.createElement('a');"
            L"a.href='#%E9%87%8D%E5%A4%8D%E6%A0%87%E9%A2%98-1';a.textContent='x';"
            L"document.getElementById('doc').appendChild(a);a.click();a.remove();"
            L"var r=document.querySelector('.blk[data-i=\"34\"]');"
            L"out='map='+ids.join(',')+' ids='+hid.join(',')+' off='+Math.round(V.off)"
            L"+' target='+Math.round(V.target)+' blk34='+(r?Math.round(r.getBoundingClientRect().top):-1)"
            L"+' vh='+V.vh;"
            L"}catch(e){out='err='+(e&&e.stack||e);}"
            L"window.chrome.webview.postMessage('event|slug|'+out);})();";
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
      } else if (wParam == kPrewarmTimerId) {
        ::KillTimer(hwnd, kPrewarmTimerId);
        LogLine(L"TEST: 预热完成，建查看器");
        g_viewer = CreateViewerOnce();
        if (g_viewer) LayoutChildren();
      } else if (wParam == kReleaseCloseTimerId) {
        ::KillTimer(hwnd, kReleaseCloseTimerId);
        LogLine(L"TEST: 销毁主窗口（模拟 DO 窗口关闭）");
        HWND main = g_hwndMain;
        g_hwndMain = nullptr;
        g_viewer = nullptr;
        ::DestroyWindow(main);
      } else if (wParam == kDropCloseTimerId) {
        ::KillTimer(hwnd, kDropCloseTimerId);
        LogLine(L"TEST: 销毁查看器，主窗口保留");
        if (g_viewer) {
          ::DestroyWindow(g_viewer);
          g_viewer = nullptr;
        }
      } else if (wParam == kReleaseReopenTimerId) {
        ::KillTimer(hwnd, kReleaseReopenTimerId);
        LogLine(L"TEST: 重新打开窗口（实例应已释放，走冷路径重建）");
        g_hwndMain = ::CreateWindowExW(0, kMainClass, L"DopusMdViewer host test",
                                       WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 1000, 780,
                                       nullptr, nullptr, g_hInst, nullptr);
        if (g_hwndMain) {
          g_viewer = CreateViewerOnce();
          if (g_viewer) LayoutChildren();
          ::SetForegroundWindow(g_hwndMain);
        }
      } else if (wParam == kReleaseQuitTimerId) {
        ::KillTimer(hwnd, kReleaseQuitTimerId);
        ::PostQuitMessage(0);
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

    case WM_NOTIFY: {
      const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lParam);
      if (hdr && hdr->code == DVPN_STATUSTEXT) {
        const DVPNMSTATUSTEXT* t = reinterpret_cast<const DVPNMSTATUSTEXT*>(lParam);
        LogLine(L"TEST: 状态 \"%s\"", t->lpszStatusText ? t->lpszStatusText : L"");
      }
      return 0;
    }

    case WM_DESTROY:
      // release 模式故意销毁主窗口但让进程继续跑，验证宿主自己回收实例。
      if (!g_release) ::PostQuitMessage(0);
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
  if (*p && wcsstr(p, L"consent")) g_consent = true;
  if (*p && wcsstr(p, L"reload")) g_reload = true;
  if (*p && wcsstr(p, L"find")) g_find = true;
  if (*p && wcsstr(p, L"findww")) {
    g_find = false;
    g_findWord = true;
  }
  if (*p && wcsstr(p, L"findrace")) {
    g_find = false;
    g_findRace = true;
  }
  if (*p && wcsstr(p, L"outline")) g_outline = true;
  if (*p && wcsstr(p, L"slug")) g_slug = true;
  if (*p && wcsstr(p, L"copy")) g_copy = true;
  if (*p && wcsstr(p, L"pin")) g_pin = true;
  if (*p && wcsstr(p, L"print")) g_print = true;
  if (*p && wcsstr(p, L"prewarm")) g_prewarm = true;
  if (*p && wcsstr(p, L"release")) g_release = true;
  if (*p && wcsstr(p, L"idle")) g_idle = true;
  if (*p && wcsstr(p, L"drop")) g_drop = true;
  if (*p) {
    const wchar_t* q = wcsstr(p, L"q=");
    if (q) {
      q += 2;
      const wchar_t* end = q;
      while (*end && *end != L' ' && *end != L'\t') ++end;
      if (end > q) ::StringCchCopyNW(g_findQuery, ARRAYSIZE(g_findQuery), q,
                                     static_cast<size_t>(end - q));
    }
  }

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

  // 模拟 InitEx 之后的延迟预热（预热在宿主线程里再等 3 秒）。
  if (g_prewarm) WebHostPrewarm(true, 3);
  // 预热关闭 + 空闲 1 分钟释放：此时宿主线程还没建，配置要能留到线程启动后生效。
  if (g_drop) WebHostPrewarm(false, 1);

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

  if (g_prewarm) {
    if (!g_idle) ::SetTimer(g_hwndMain, kPrewarmTimerId, kPrewarmAfterMs, nullptr);
  } else {
    g_viewer = CreateViewerOnce();
    if (!g_viewer) return 5;
    LayoutChildren();
  }

  if (g_release) {
    // 主窗口马上会被销毁；用一个隐藏窗口把计时器撑到进程退出。
    g_hwndTimer = ::CreateWindowExW(0, kMainClass, L"release-timer", WS_POPUP, 0, 0, 1, 1,
                                    nullptr, nullptr, hInstance, nullptr);
    if (g_hwndTimer) {
      ::SetTimer(g_hwndTimer, kReleaseCloseTimerId, kReleaseCloseAfterMs, nullptr);
      ::SetTimer(g_hwndTimer, kReleaseReopenTimerId, kReleaseReopenAfterMs, nullptr);
      ::SetTimer(g_hwndTimer, kReleaseQuitTimerId, kReleaseQuitAfterMs, nullptr);
    }
  }

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
  if (g_consent) {
    ::SetTimer(g_hwndMain, kConsentTimerId, 5000, nullptr);
  }
  if (g_reload) {
    ::SetTimer(g_hwndMain, kReloadTimerId, 6500, nullptr);
  }
  if (g_find || g_findWord) {
    ::SetTimer(g_hwndMain, kFindTimerId, 5000, nullptr);
  }
  if (g_findRace) {
    ::SetTimer(g_hwndMain, kFindRaceTimerId, 5000, nullptr);
  }
  if (g_outline) {
    ::SetTimer(g_hwndMain, kOutlineTimerId, 5000, nullptr);
  }
  if (g_slug) {
    ::SetTimer(g_hwndMain, kSlugTimerId, 5000, nullptr);
  }
  if (g_copy) {
    ::SetTimer(g_hwndMain, kCopyTimerId, 5000, nullptr);
    ::SetTimer(g_hwndMain, kCopyCheckTimerId, 7000, nullptr);
  }
  if (g_pin) {
    ::SetTimer(g_hwndMain, kPinTimerId, 5000, nullptr);
  }
  if (g_print) {
    ::SetTimer(g_hwndMain, kPrintTimerId, 5000, nullptr);
  }

  if (g_drop) {
    ::SetTimer(g_hwndMain, kDropCloseTimerId, 5000, nullptr);
  }
  if (g_szShot[0] && !g_release) {
    ::SetForegroundWindow(g_hwndMain);
    ::SetTimer(g_hwndMain, kShotTimerId, g_shotAfterMs, nullptr);
  }
  if (!g_release) {
    ::SetTimer(g_hwndMain, kQuitTimerId, g_drop ? kDropQuitAfterMs : kQuitAfterMs, nullptr);
  }

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  return 0;
}
