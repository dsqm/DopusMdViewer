// settings_preview.h - 设置窗口里的字体预览。
//
// 预览要跟页面上看到的一致，就只能用同一个渲染引擎。这里自建一个**独立**的
// WebView2 实例：不与 webhost.h 那个「唯一宿主」共用，否则打开设置会把正在看的
// 预览窗格抢走（宿主被 reparent，原持有者收到 WM_MDV_HOST_LOST 直接白屏）。
//
// 代价是多一个浏览器进程与一份独立的用户数据目录，关掉设置窗口即销毁。

#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct SettingsPreview;

// 在 parent 的客户区 rc 处创建一个预览子窗口。失败返回 nullptr（调用方退回纯文本提示）。
SettingsPreview* SettingsPreviewCreate(HWND parent, const RECT& rc);

// 子窗口句柄：调用方用它参与页签的显示/隐藏。
HWND SettingsPreviewHwnd(SettingsPreview* p);

void SettingsPreviewResize(SettingsPreview* p, const RECT& rc);

// 回退链 + 字号。整条链按顺序拼成 CSS font-family，某个字体缺失时由浏览器自己往下回退，
// 所以预览里能看到真实的回退结果。
void SettingsPreviewSetFont(SettingsPreview* p, const std::vector<std::wstring>& chain, int sizePx);

void SettingsPreviewDestroy(SettingsPreview* p);
