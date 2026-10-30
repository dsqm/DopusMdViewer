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

enum {
  MDV_EVT_READY = 1,    // 页面首帧就绪（text 为空）
  MDV_EVT_FAILED = 2,   // 建不起来（text = 原因）
  MDV_EVT_MESSAGE = 3,  // 页面发来的消息（text = 完整帧，如 "event|rendered|12.3"）
};

struct MDV_EVENT {
  int kind;
  wchar_t* text;
};

// 建宿主线程并按需创建 controller。幂等：已就绪或正在创建时立即返回。
bool WebHostEnsure(void);

// 页面是否已发过 ready（未就绪时 Post 会失败，调用方要先缓存内容）。
bool WebHostIsReady(void);

// 发一条控制消息；页面未就绪或 controller 不在时返回 false。
bool WebHostPost(const std::wstring& text);

// 把宿主窗口挂到查看器窗口下并铺满 client 区域。事件之后都发给 hViewer。
void WebHostBorrow(HWND hViewer, const RECT& client);

// 归还宿主（隐藏并脱离查看器）。controller 保留，下次借用不必重建。
void WebHostUnborrow(void);

// 查看器尺寸变化。
void WebHostSetBounds(const RECT& client);

// 当前文档所在目录 → mddoc.local 虚拟主机；空串表示取消映射。
bool WebHostSetDocFolder(const std::wstring& dir);

// 关闭 controller（宿主线程保留）。当前只在需要强制重建时使用。
void WebHostReleaseController(void);
