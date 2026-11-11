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

// 在 parent 的客户区 rc 处创建一个预览子窗口。失败返回 nullptr，此时调用方
// 只留下一片空白的预览区，并在下方那句说明里写明预览不可用。
SettingsPreview* SettingsPreviewCreate(HWND parent, const RECT& rc);

// 子窗口句柄：调用方用它参与页签的显示/隐藏。
HWND SettingsPreviewHwnd(SettingsPreview* p);

void SettingsPreviewResize(SettingsPreview* p, const RECT& rc);

// 回退链 + 字号。整条链按顺序拼成 CSS font-family，某个字体缺失时由浏览器自己往下回退，
// 所以预览里能看到真实的回退结果。
void SettingsPreviewSetFont(SettingsPreview* p, const std::vector<std::wstring>& chain, int sizePx);

// 内置示例文字：两行，\n 分隔。配置里的空串即表示用这一份。
const wchar_t* SettingsPreviewDefaultText(void);

// 预览框显示的文字。换行分行，第一行用主色、其余用辅助色；用 textContent 写入，
// 用户文字里的尖括号不会被当成 HTML。页面尚未就绪时先记下，就绪后自动下发。
void SettingsPreviewSetText(SettingsPreview* p, const std::wstring& text);

// 预览页跑在独立 WebView2 里，看不到宿主的深色模式，由设置窗口显式告知。
// 页面尚未导航完成时先记下状态，就绪后自动下发。
void SettingsPreviewSetTheme(SettingsPreview* p, bool dark);

// 删掉预览的用户数据目录并在同一个窗口上重建：先释放 Environment，再删目录，
// 窗口句柄与已下发的字体/配色都不变。userDataDir 必须是 UtilWebView2SettingsDir
// 给出的 ...\WebView2-Settings。返回 false 表示目录仍被占用、没删干净，预览照样重建。
bool SettingsPreviewClearCache(SettingsPreview* p, const std::wstring& userDataDir);

void SettingsPreviewDestroy(SettingsPreview* p);
