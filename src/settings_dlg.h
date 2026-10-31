// settings_dlg.h - Opus 插件设置入口（M6 的四页设置之前先放网络图片这一项）。

#pragma once

#include <windows.h>

// 创建或激活非模态设置窗口；返回窗口句柄（失败为 nullptr）。
HWND SettingsShow(HWND parent);
