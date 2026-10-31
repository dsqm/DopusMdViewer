// viewer_window.h - Opus 的查看器子窗口。
//
// 这个窗口本身不画任何东西：它把 WebView2 的宿主窗口借进来铺满客户区，
// 负责把 DVPLUGINMSG_* 翻译成「读文件 / 推送正文 / 重新注入皮肤与字体」。
#pragma once

#include <windows.h>

// 注册查看器窗口类（每进程一次）。失败时已写日志，DVP_CreateViewer 返回 NULL。
bool ViewerRegisterClass(HINSTANCE hInstance);

// 建查看器子窗口。所有权归 Opus；插件只创建与布局自己的子窗口。
HWND ViewerCreate(HWND hWndParent, const RECT* lpRc, DWORD dwFlags);

// 当前存活的查看器窗口数（create 递增、WM_NCDESTROY 递减），DVP_Uninit 会记一行。
LONG ViewerLiveCount(void);

// 设置保存后调用：给所有活着的查看器 PostMessage(WM_MDV_CONFIG_CHANGED)，
// 让查看器线程重推皮肤/字体/网络图档位。
void ViewerNotifyConfigChanged(void);

// 设置已更新（无载荷）。
#define WM_MDV_CONFIG_CHANGED (WM_APP + 0x311)
