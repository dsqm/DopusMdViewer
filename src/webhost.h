// webhost.h - 常驻隐藏宿主窗口与 WebView2 controller 的生命周期（票 10）。
//
// 为什么要有这一层：Opus 每换一个选中文件就销毁并重建查看器窗口，而 controller
// 一旦父窗口销毁就永久失效（0x8007139F）。所以 controller 永远挂在插件自持的
// 隐藏宿主下，Opus 的查看器窗口只是通过 SetParent 借用它。
//
// 所有回调都在创建它的线程（Opus 的 UI 线程）上触发。

#pragma once

#include <windows.h>

#include <string>

struct WebHostSink {
  void* ctx;
  void (*onReady)(void* ctx);                                            // shell.html 首帧就绪
  void (*onEvent)(void* ctx, const std::wstring& type,
                  const std::wstring& rest);                             // 页面来的消息
  void (*onFailed)(void* ctx, const wchar_t* reason);                    // 建不起来
};

// 建宿主窗口并按需异步创建 controller。幂等：已就绪或正在创建时直接返回 true。
bool WebHostEnsure(const WebHostSink& sink);

// 换当前借用方的回调（查看器窗口销毁重建时用）。
void WebHostSetSink(const WebHostSink& sink);

// 页面是否已发过 ready（没就绪时 Post 会失败，调用方要先缓存内容）。
bool WebHostIsReady(void);

// 发一条控制消息；页面未就绪或 controller 不在时返回 false。
bool WebHostPost(const std::wstring& text);

// 把宿主窗口挂到查看器窗口下并铺满 client 区域。
void WebHostBorrow(HWND hViewer, const RECT& client);

// 归还宿主（隐藏并脱离查看器）。最后一个借用方消失后开始空闲计时。
void WebHostUnborrow(void);

// 查看器尺寸变化。
void WebHostSetBounds(const RECT& client);

// 当前文档所在目录 → mddoc.local 虚拟主机；空串表示取消映射。
bool WebHostSetDocFolder(const std::wstring& dir);

// 关闭 controller（宿主窗口保留）。空闲超时会自动调用。
void WebHostReleaseController(void);
