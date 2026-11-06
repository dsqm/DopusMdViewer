// settings_dlg.h - DOpus 插件设置入口（非模态四页：外观 / 字体 / 渲染 / 高级）。

#pragma once

#include <windows.h>

// 创建或激活非模态设置窗口；返回窗口句柄（失败为 nullptr）。
HWND SettingsShow(HWND parent);
