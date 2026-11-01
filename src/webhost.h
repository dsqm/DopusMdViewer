// webhost.h - 插件自持的 WebView2 宿主。
//
// WebView2 的对象是**线程绑定**的，而 Opus 的预览窗格与独立查看器窗口跑在不同的
// UI 线程上。所以这里把 Environment/Controller 全部放到插件自己的一个线程上，
// 其余线程只通过消息借用：一个进程里只有一份实例、一个渲染进程，切换文件与
// 关窗再开都不需要重建。
//
// 事件（页面就绪、渲染完成、外部链接）由宿主线程 PostMessage 给借用方窗口，
// 消息号与载荷格式见下面的 WM_MDV_EVENT。

#pragma once

#include <windows.h>

#include <string>

// 宿主 → 借用方窗口（查看器窗口）的事件消息。
// wParam = MDV_EVT_*，lParam = MDV_EVENT*（接收方负责 delete）。
#define WM_MDV_EVENT (WM_APP + 0x40)

// 宿主只有一个：被别的查看器借走时，原借用方收到 WM_MDV_HOST_LOST，
// 归还时按借用历史把宿主交给上一个还活着的查看器，它会收到 WM_MDV_RECLAIM。
#define WM_MDV_HOST_LOST (WM_APP + 0x41)
#define WM_MDV_RECLAIM (WM_APP + 0x42)

enum {
  MDV_EVT_READY = 1,    // 页面首帧就绪（text 为空）
  MDV_EVT_FAILED = 2,   // 建不起来（text = 原因）
  MDV_EVT_MESSAGE = 3,  // 页面发来的消息（text = 完整帧，如 "event|rendered|12.3"）
  MDV_EVT_RECOVERING = 4,  // 进程失效后正在自愈，随后会再发一次 READY（text 为空）
};

struct MDV_EVENT {
  int kind;
  wchar_t* text;
};

// 建宿主线程并按需创建 controller。幂等：已就绪或正在创建时立即返回。
bool WebHostEnsure(void);

// 启动时预热：延迟一段时间后在宿主线程上建好 Environment/controller 并导航 shell；
// 预热关闭时，空闲 idleReleaseMinutes 分钟（0 = 不释放）回收实例。
void WebHostPrewarm(bool enabled, int idleReleaseMinutes);

// 页面是否已发过 ready（未就绪时 Post 会失败，调用方要先缓存内容）。
bool WebHostIsReady(void);

// 发一条控制消息。页面未就绪、controller 不在，或 hViewer 不是当前借用方时返回
// false——**宿主只有一个**，非借用方投递出去的内容会画进看不见的宿主窗口。
bool WebHostPost(HWND hViewer, const std::wstring& text);

// 把宿主窗口挂到查看器窗口下并铺满 client 区域。宿主被借走时原借用方会收到
// WM_MDV_HOST_LOST。返回 false 表示宿主线程没起来。
bool WebHostBorrow(HWND hViewer, const RECT& client);

// 归还宿主（隐藏并脱离查看器）。只有当前借用方调用才生效；之后宿主按借用历史
// 交给上一个还活着的查看器（它收到 WM_MDV_RECLAIM）。controller 保留，不必重建。
void WebHostUnborrow(HWND hViewer);

// 查看器尺寸变化。非借用方调用会被忽略——否则会把别人的内容缩成自己的尺寸。
bool WebHostSetBounds(HWND hViewer, const RECT& client);

// 当前文档所在目录 → mddoc.localhost 虚拟主机；空串表示取消映射。
bool WebHostSetDocFolder(const std::wstring& dir);

// 关闭 controller（宿主线程保留）。当前只在需要强制重建时使用。
void WebHostReleaseController(void);

// 只给 tools/hosttest 用：在页面里执行一段脚本（滚动验收）。
bool WebHostExecuteScript(HWND hViewer, const std::wstring& script);

// 只给 tools/hosttest 用：发一条 DevTools 方法，用来造渲染进程崩溃（Page.crash）。
bool WebHostCallDevTools(HWND hViewer, const std::wstring& method);

// 打开 WebView2 的打印对话框（ICoreWebView2_16::ShowPrintUI）。页面要先把
// 全文渲染进打印容器；运行时太老拿不到接口时返回 false。
bool WebHostShowPrintUI(HWND hViewer);

// 实际会用的 WebView2 运行时：「自定义 153.0.4234.48」/「系统 153.0.4234.48」；
// 自定义那份建不起来时给出「系统 …（已回退）」；完全没有返回空串。设置界面“关于”用。
std::wstring WebHostRuntimeInfo(void);
