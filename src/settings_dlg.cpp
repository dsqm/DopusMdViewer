// settings_dlg.cpp - 非模态设置窗口（外观 / 字体 / 渲染 / 高级四页）。
//
// 四页共用一个窗口，靠控件的 ShowWindow 切换，不建子对话框；改动只进 staged
// 副本，点“应用/确定”才写 config.json 并通知查看器。

#include "settings_dlg.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <strsafe.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "config.h"
#include "localize.h"
#include "log.h"
#include "settings_preview.h"
#include "resource_ids.h"
#include "skin.h"
#include "util.h"
#include "viewer_window.h"
#include "webhost.h"

namespace {

const wchar_t kClass[] = L"DopusMdViewerSettings";

enum {
  kIdTabAppearance = 3001,
  kIdTabFont,
  kIdTabRender,
  kIdTabAdvanced,
  kIdSkin = 3100,
  kIdSize,
  kIdSizeSpin,
  kIdLh,
  kIdLhSpin,
  kIdMeasure,
  kIdMeasureSpin,
  kIdUnlimited,
  kIdMeasureHint,
  kIdSlotUi = 3200,
  kIdSlotBody,
  kIdSlotMono,
  kIdFamily,
  kIdFamilyList,
  kIdFamilyAdd,
  kIdChainUp,
  kIdChainDown,
  kIdChainDel,
  kIdPreviewNote,
  kIdRenderMermaid = 3300,
  kIdRenderKatex,
  kIdRenderHighlight,
  kIdRenderHtml,
  kIdRenderLocal,
  kIdNetDoc,
  kIdNetHost,
  kIdNetOff,
  kIdOutsideParent,
  kIdOutsideAbsolute,
  kIdCopyCode = 3310,
  kIdCopyTable,
  kIdCopyFormat,
  kIdTableWrap,
  kIdFindBtn,
  kIdOutlineBtn,
  kIdLineNumbers,
  kIdCodeWsOn,
  kIdCodeWsSelected,
  kIdCodeWsBoundary,
  kIdCodeWsAll,
  kIdIndentedBlocks,
  kIdCodeGroup,
  kIdVersion = 3400,
  kIdWebView,
  kIdOpenLog,
  kIdOpenConfig,
  kIdOpenSkins,
  kIdPrewarm,
  kIdLinkOpenMode,
  kIdViewerExtsBtn,
  kIdIdle,
  kIdIdleSpin,
  kIdConfigPath = 3410,
  kIdConfigHelp,
  kIdEnableLog,
  kIdWebViewDir = 3420,
  kIdWebViewBrowse,
  kIdWebViewHelp,
  kIdWebViewOpen,
  kIdWebViewDefault,
  kIdWebViewClear,
  kIdCopyVersion = 3490,
  kIdPrewarmHelp,
  kIdRestore = 3500,
  kIdResetPage,
  kIdApply,
  kIdOk,
  kIdCancel,
};

struct FontPanel;

constexpr int kPages = 4;
// 「复制版本信息」按下后按钮文字短暂变成「已复制」，靠这个计时器改回来。
constexpr UINT_PTR kCopyHintTimerId = 1;
// 字体预览区的未缩放矩形：创建与 DPI 重排共用一份。
constexpr int kPreviewX = 100;
constexpr int kPreviewY = 332;
constexpr int kPreviewW = 430;
constexpr int kPreviewH = 64;
// 客户区目标尺寸（未缩放单位）与窗口样式：算窗口大小、按工作区裁剪都要用。
constexpr int kClientW = 560;
constexpr int kClientH = 558;
constexpr DWORD kDialogStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;

struct DialogState {
  HWND hwnd = nullptr;
  HFONT font = nullptr;
  HFONT fontBold = nullptr;   // 页签当前页用，见 DrawTab
  UINT dpi = 96;
  Config staged;
  std::vector<SkinEntry> skins;
  std::vector<std::wstring> families;
  int page = 0;
  int slot = 0;  // 0 界面 / 1 正文 / 2 等宽
  bool previewMissing = false;
  std::vector<HWND> pageCtrls[kPages];
  std::vector<std::pair<HWND, WNDPROC>> subs;  // 装过键盘 subclass 的控件

  HWND hSkin = nullptr;
  HWND hSize = nullptr;
  HWND hSizeSpin = nullptr;
  HWND hLh = nullptr;
  HWND hLhSpin = nullptr;
  HWND hMeasure = nullptr;
  HWND hMeasureSpin = nullptr;
  HWND hUnlimited = nullptr;
  HWND hMeasureHint = nullptr;
  HWND hCopyVersion = nullptr;

  HWND hSlot[3] = {};
  HWND hFamily = nullptr;
  HWND hChainList = nullptr;
  FontPanel* fontPanel = nullptr;   // 「添加字体…」弹出的选择面板，见 FontPanel
  SettingsPreview* preview = nullptr;  // 字体预览用的独立 WebView2 实例
  HWND hPreview = nullptr;
  HWND hPreviewNote = nullptr;

  HWND hRender[5] = {};
  HWND hCopyCode = nullptr;
  HWND hCopyTable = nullptr;
  HWND hCopyFormat = nullptr;
  HWND hFindBtn = nullptr;
  HWND hOutlineBtn = nullptr;
  HWND hLineNumbers = nullptr;
  HWND hCodeWsOn = nullptr;
  HWND hCodeWsSelected = nullptr;
  HWND hCodeWsBoundary = nullptr;
  HWND hCodeWsAll = nullptr;
  HWND hIndentedBlocks = nullptr;
  HWND hNetDoc = nullptr;
  HWND hNetHost = nullptr;
  HWND hNetOff = nullptr;
  HWND hOutsideParent = nullptr;
  HWND hOutsideAbsolute = nullptr;
  HWND hPrewarm = nullptr;
  HWND hLinkMode = nullptr;
  HWND hViewerExts = nullptr;
  HWND hIdle = nullptr;
  HWND hIdleSpin = nullptr;
  HWND hWebViewDir = nullptr;
  HWND hWebViewInfo = nullptr;
  HWND hApply = nullptr;
  bool dirty = false;
  // 每个控件的**未缩放**坐标：DPI 变化时按新比例重算，比按当前比例反推更准。
  struct CtrlGeom {
    HWND hwnd;
    int x, y, cx, cy;
  };
  std::vector<CtrlGeom> layout;
};

HWND g_hwnd = nullptr;

HMODULE ModuleHandle(void);

int ScaleByDpi(UINT dpi, int value) {
  return ::MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

int ScaleOf(const DialogState* st, int value) {
  return ScaleByDpi(st->dpi, value);
}

// 设备像素 → 未缩放坐标（AddControl 内部会再乘回 DPI 比例）。
int UnscaleOf(const DialogState* st, int px) {
  const UINT dpi = st->dpi ? st->dpi : 96;
  return ::MulDiv(px, 96, static_cast<int>(dpi));
}

// 用当前 UI 字体量一行文字的宽度（设备像素）。用来把「?」紧贴在描述文字后面。
int TextWidth(DialogState* st, const wchar_t* text) {
  if (!text || !*text) return 0;
  HDC dc = ::GetDC(st->hwnd);
  if (!dc) return 0;
  HGDIOBJ old = ::SelectObject(dc, st->font);
  SIZE sz = {};
  ::GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
  ::SelectObject(dc, old);
  ::ReleaseDC(st->hwnd, dc);
  return sz.cx;
}

void ApplyFont(HWND hwnd, HFONT font) {
  if (hwnd && font) ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

/* ---------------- 配置文件夹说明 ----------------
   用 DialogBoxParam：可选中、Ctrl+C、右键复制都可用，另给「复制别名」一键复制。
   不自己跑嵌套消息循环，避免和外层配置窗口的消息循环打架。 */

struct HelpDialogData {
  const wchar_t* title = nullptr;
  const wchar_t* text = nullptr;
  const wchar_t* copyText = nullptr;
  const wchar_t* copyLabel = nullptr;
  HFONT font = nullptr;
};

bool CopyTextToClipboard(HWND owner, const wchar_t* text) {
  if (!text || !::OpenClipboard(owner)) return false;
  bool ok = false;
  if (::EmptyClipboard()) {
    const size_t cch = std::wcslen(text) + 1;
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, cch * sizeof(wchar_t));
    if (mem) {
      void* dst = ::GlobalLock(mem);
      if (dst) {
        std::wmemcpy(static_cast<wchar_t*>(dst), text, cch);
        ::GlobalUnlock(mem);
        if (::SetClipboardData(CF_UNICODETEXT, mem)) {
          ok = true;
        } else {
          ::GlobalFree(mem);
        }
      } else {
        ::GlobalFree(mem);
      }
    }
  }
  ::CloseClipboard();
  return ok;
}

INT_PTR CALLBACK HelpDlgProc(HWND dlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_INITDIALOG: {
      const HelpDialogData* data = reinterpret_cast<const HelpDialogData*>(lParam);
      ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
      ::SetWindowTextW(dlg, data && data->title ? data->title : L"");
      ::SetDlgItemTextW(dlg, IDC_HELP_TEXT, data && data->text ? data->text : L"");
      ::SetDlgItemTextW(dlg, IDC_HELP_COPY,
                        data && data->copyLabel ? data->copyLabel : loc::T(loc::Str::CopyAlias));
      ::SetDlgItemTextW(dlg, IDOK, loc::T(loc::Str::Ok));
      if (data && data->font) {
        HWND controls[] = {::GetDlgItem(dlg, IDC_HELP_TEXT), ::GetDlgItem(dlg, IDC_HELP_COPY),
                           ::GetDlgItem(dlg, IDOK)};
        for (HWND h : controls) {
          if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(data->font), TRUE);
        }
      }
      HWND edit = ::GetDlgItem(dlg, IDC_HELP_TEXT);
      if (edit) ::SetFocus(edit);
      return TRUE;
    }

    case WM_COMMAND:
      if (LOWORD(wParam) == IDC_HELP_COPY && HIWORD(wParam) == BN_CLICKED) {
        const HelpDialogData* data =
            reinterpret_cast<const HelpDialogData*>(::GetWindowLongPtrW(dlg, GWLP_USERDATA));
        const wchar_t* copy = data && data->copyText && *data->copyText
                                  ? data->copyText
                                  : (data && data->text ? data->text : L"");
        if (CopyTextToClipboard(dlg, copy)) {
          ::SetDlgItemTextW(dlg, IDC_HELP_COPY, loc::T(loc::Str::Copied));
          ::SetTimer(dlg, 1, 1200, nullptr);
        }
        return TRUE;
      }
      if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
        ::EndDialog(dlg, 0);
        return TRUE;
      }
      return FALSE;

    case WM_TIMER:
      if (wParam == 1) {
        ::KillTimer(dlg, 1);
        ::SetDlgItemTextW(dlg, IDC_HELP_COPY, loc::T(loc::Str::CopyAlias));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      ::EndDialog(dlg, 0);
      return TRUE;

    default:
      return FALSE;
  }
}

void ShowTextHelp(HWND owner, const wchar_t* title, const wchar_t* text,
                  const wchar_t* copyText, const wchar_t* copyLabel, HFONT font) {
  HelpDialogData data;
  data.title = title;
  data.text = text;
  data.copyText = copyText;
  data.copyLabel = copyLabel;
  data.font = font;
  if (::DialogBoxParamW(ModuleHandle(), MAKEINTRESOURCEW(IDD_CONFIG_HELP), owner, HelpDlgProc,
                        reinterpret_cast<LPARAM>(&data)) == -1) {
    ::MessageBoxW(owner, text, title ? title : L"", MB_OK | MB_ICONINFORMATION);
  }
}

void ShowConfigHelp(HWND owner, const wchar_t* text, HFONT font) {
  ShowTextHelp(owner, loc::T(loc::Str::ConfigFolder), text, kDataAliasName,
               loc::T(loc::Str::CopyAlias), font);
}

/* ---------------- 键盘导航 ----------------
   这是非模态的自建窗口，跑在宿主的消息循环里，拿不到对话框那套 IsDialogMessage
   行为（Tab 换焦点、Enter 走默认按钮、Esc 走取消）。所以自己实现：Tab 的消息先到
   焦点的子控件，因此给每个子控件装一个 subclass，在它之前把这些键截下来。
   Tab 顺序取子窗口的创建顺序（GW_CHILD），见 TabStops。 */

LRESULT CALLBACK ControlProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void MoveFocus(DialogState* st, int dir);
void FocusFirst(DialogState* st);
void ShowPage(DialogState* st, int page);
bool ApplySettings(DialogState* st);
void RefreshWebViewCacheInfo(DialogState* st);
void UpdateWebViewUsbState(DialogState* st);
std::wstring& ActiveWebViewBaseRef(Config* cfg);
RECT WorkAreaFor(HWND wnd);
UINT FitDpiToWorkArea(UINT dpi, const RECT& work);
void RelayoutForDpi(DialogState* st, UINT dpi);
void WindowSizeForDpi(UINT dpi, int* w, int* h);
void AddFontNameToChain(DialogState* st, const std::wstring& name);
void SetDirty(DialogState* st, bool dirty);
bool FamilyInstalled(DialogState* st, const std::wstring& name);
bool ValidateWebViewDir(const std::wstring& raw, std::wstring* error);
std::wstring ReadText(HWND hwnd);
bool HandleDialogKey(DialogState* st, HWND focus, WPARAM vk);
bool EditViewerExts(HWND owner, std::wstring* exts, HFONT font);

/* ---------------- 扩展名名单对话框 ----------------
   名单有几十项，主设置页放不下多行框，单独开一个模态对话框。初值是当前名单；
   为空时展开内置名单，用户看到的就是全部生效项，直接在上面增删。 */

struct ViewerExtsDialogData {
  std::wstring text;
  HFONT font;
};

INT_PTR CALLBACK ViewerExtsDlgProc(HWND dlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_INITDIALOG: {
      auto* data = reinterpret_cast<ViewerExtsDialogData*>(lParam);
      ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
      ::SetWindowTextW(dlg, loc::T(loc::Str::ViewerExtsTitle));
      ::SetDlgItemTextW(dlg, IDC_EXTS_HINT, loc::T(loc::Str::ViewerExtsHint));
      ::SetDlgItemTextW(dlg, IDC_EXTS_TEXT, data ? data->text.c_str() : L"");
      ::SetDlgItemTextW(dlg, IDC_EXTS_DEFAULT, loc::T(loc::Str::Restore));
      ::SetDlgItemTextW(dlg, IDOK, loc::T(loc::Str::Ok));
      ::SetDlgItemTextW(dlg, IDCANCEL, loc::T(loc::Str::Cancel));
      if (data && data->font) {
        HWND controls[] = {::GetDlgItem(dlg, IDC_EXTS_HINT), ::GetDlgItem(dlg, IDC_EXTS_TEXT),
                           ::GetDlgItem(dlg, IDC_EXTS_DEFAULT), ::GetDlgItem(dlg, IDOK),
                           ::GetDlgItem(dlg, IDCANCEL)};
        for (HWND h : controls) {
          if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(data->font), TRUE);
        }
      }
      HWND edit = ::GetDlgItem(dlg, IDC_EXTS_TEXT);
      if (edit) ::SetFocus(edit);
      return FALSE;  // 焦点已经自己给
    }

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDC_EXTS_DEFAULT:
          if (HIWORD(wParam) == BN_CLICKED) {
            ::SetDlgItemTextW(dlg, IDC_EXTS_TEXT, UtilOpusViewerDefaultExts().c_str());
            return TRUE;
          }
          return FALSE;
        case IDOK: {
          auto* data = reinterpret_cast<ViewerExtsDialogData*>(
              ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
          if (data) data->text = ReadText(::GetDlgItem(dlg, IDC_EXTS_TEXT));
          ::EndDialog(dlg, 1);
          return TRUE;
        }
        case IDCANCEL:
          ::EndDialog(dlg, 0);
          return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      ::EndDialog(dlg, 0);
      return TRUE;

    default:
      return FALSE;
  }
}

bool EditViewerExts(HWND owner, std::wstring* exts, HFONT font) {
  if (!exts) return false;
  ViewerExtsDialogData data;
  data.text = exts->empty() ? UtilOpusViewerDefaultExts() : *exts;
  data.font = font;
  const INT_PTR result = ::DialogBoxParamW(ModuleHandle(), MAKEINTRESOURCEW(IDD_VIEWER_EXTS),
                                           owner, ViewerExtsDlgProc,
                                           reinterpret_cast<LPARAM>(&data));
  if (result == -1) {
    // 资源缺失时 DialogBoxParam 只返回 -1，不弹任何东西；静默失败最难查
    LogLine(L"SETTINGS: 扩展名名单对话框打不开 err=%lu", ::GetLastError());
    ::MessageBoxW(owner, loc::T(loc::Str::ViewerExtsOpenFailed), L"DopusMdViewer",
                  MB_OK | MB_ICONERROR);
    return false;
  }
  if (result != 1) return false;
  const std::wstring text = UtilTrim(data.text);
  // 与内置名单一致就存空串：以后内置名单更新，没改过的用户自动跟着变。
  *exts = (text == UtilOpusViewerDefaultExts()) ? std::wstring() : text;
  return true;
}

DialogState* StateOfParent(HWND hwnd) {
  HWND parent = ::GetParent(hwnd);
  if (!parent) return nullptr;
  return reinterpret_cast<DialogState*>(::GetWindowLongPtrW(parent, GWLP_USERDATA));
}

WNDPROC OriginalProc(DialogState* st, HWND hwnd) {
  if (!st) return nullptr;
  for (const std::pair<HWND, WNDPROC>& s : st->subs) {
    if (s.first == hwnd) return s.second;
  }
  return nullptr;
}

void SubclassControl(DialogState* st, HWND hwnd) {
  if (!st || !hwnd) return;
  if (OriginalProc(st, hwnd)) return;
  WNDPROC old = reinterpret_cast<WNDPROC>(
      ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ControlProc)));
  if (!old || old == &ControlProc) return;
  st->subs.push_back(std::make_pair(hwnd, old));
}

std::vector<HWND> TabStops(DialogState* st) {
  std::vector<HWND> out;
  if (!st || !st->hwnd) return out;

  // 页签组单独处理：四个单选按钮是一组，Windows 会在 BM_SETCHECK 之后搬动甚至清掉
  // 同组按钮的 WS_TABSTOP，靠样式位枚举会整组漏掉（实测第一个 Tab 直接落到皮肤下拉，
  // 于是 ←/→ 换页在键盘上永远用不上）。这里按「当前选中的那个」占一个站位，放最前。
  HWND tab = ::GetDlgItem(st->hwnd, kIdTabAppearance + st->page);
  if (tab && (!::IsWindowVisible(tab) || !::IsWindowEnabled(tab))) tab = nullptr;
  if (!tab) tab = ::GetDlgItem(st->hwnd, kIdTabAppearance);
  if (tab) out.push_back(tab);

  // GW_CHILD 的顺序就是创建顺序（这些子窗口不重叠、谁也不改 z 序），
  // 正好是 Tab 该走的顺序：页签 → 当页控件 → 底部按钮。
  for (HWND h = ::GetWindow(st->hwnd, GW_CHILD); h; h = ::GetWindow(h, GW_HWNDNEXT)) {
    const int id = ::GetDlgCtrlID(h);
    if (id >= kIdTabAppearance && id <= kIdTabAdvanced) continue;  // 页签已在最前
    if (!(::GetWindowLongPtrW(h, GWL_STYLE) & WS_TABSTOP)) continue;
    if (!::IsWindowVisible(h) || !::IsWindowEnabled(h)) continue;
    out.push_back(h);
  }
  return out;
}

// 键盘导航时把「显示焦点提示」的 UI 状态打开。这不是对话框，没有对话框管理器替我们
// 切换这个状态：鼠标点过任何控件之后 UISF_HIDEFOCUS 一直停在“隐藏”，于是 Tab 过去
// 看不见虚线焦点框（方向键换页时因为整页重绘才偶然看得见）。
void ShowFocusCues(DialogState* st, HWND target) {
  if (!st) return;
  const WPARAM clear = MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS);
  ::SendMessageW(st->hwnd, WM_CHANGEUISTATE, clear, 0);
  if (target) {
    ::SendMessageW(target, WM_CHANGEUISTATE, clear, 0);
    ::InvalidateRect(target, nullptr, TRUE);
  }
}

// 焦点移到下一个/上一个 tab stop；顺带把编辑框内容全选，省得用户先按 Ctrl+A。
void MoveFocus(DialogState* st, int dir) {
  std::vector<HWND> stops = TabStops(st);
  if (stops.empty()) return;
  HWND cur = ::GetFocus();
  const int n = static_cast<int>(stops.size());
  int idx = -1;
  for (int i = 0; i < n; ++i) {
    if (stops[static_cast<size_t>(i)] == cur) idx = i;
  }
  int next = idx < 0 ? (dir > 0 ? 0 : n - 1) : ((idx + dir) % n + n) % n;
  HWND target = stops[static_cast<size_t>(next)];
  ::SetFocus(target);
  ShowFocusCues(st, target);
  wchar_t cls[32] = {};
  ::GetClassNameW(target, cls, ARRAYSIZE(cls));
  if (::lstrcmpiW(cls, L"Edit") == 0) ::SendMessageW(target, EM_SETSEL, 0, -1);
  LogLine(L"SETTINGS: 焦点 → 控件 %d（%s）", ::GetDlgCtrlID(target), cls);
}

void FocusFirst(DialogState* st) {
  std::vector<HWND> stops = TabStops(st);
  if (stops.empty()) return;
  ::SetFocus(stops.front());
  ShowFocusCues(st, stops.front());
  if (::GetFocus() != stops.front()) LogLine(L"SETTINGS: 第一个可聚焦控件没拿到焦点");
}

// 切页后把焦点落到本页第一个控件上：跳过顶部那排页签，否则换页后焦点还停在页签上，
// 用户按空格会又点一次页签。本页没有可聚焦控件时才退回整窗第一个。
void FocusPageFirst(DialogState* st) {
  std::vector<HWND> stops = TabStops(st);
  for (HWND h : stops) {
    const int id = ::GetDlgCtrlID(h);
    if (id >= kIdTabAppearance && id <= kIdTabAdvanced) continue;
    ::SetFocus(h);
    ShowFocusCues(st, h);
    return;
  }
  FocusFirst(st);
}

bool IsPageTab(HWND hwnd) {
  const int id = ::GetDlgCtrlID(hwnd);
  return id >= kIdTabAppearance && id <= kIdTabAdvanced;
}

bool ComboDropped(DialogState* st) {
  if (!st) return false;
  const HWND focus = ::GetFocus();
  wchar_t cls[32] = {};
  if (!focus || !::GetClassNameW(focus, cls, ARRAYSIZE(cls))) return false;
  if (::lstrcmpiW(cls, L"ComboBox") != 0) return false;
  return ::SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0) != 0;
}

// 对话框键：子控件上按下时由 ControlProc 送来，焦点在窗口本身时由 SettingsProc 送来。
// 返回 true 表示已处理，调用方不再把消息往下传。
bool HandleDialogKey(DialogState* st, HWND hwnd, WPARAM vk) {
  if (!st) return false;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  switch (vk) {
    case VK_TAB:
      MoveFocus(st, shift ? -1 : 1);
      return true;
    case VK_ESCAPE:
      // 下拉展开时先把下拉收起来，别顺手关窗
      if (ComboDropped(st)) return false;
      ::DestroyWindow(st->hwnd);
      return true;
    case VK_RETURN:
      // 下拉展开时 Enter 是“选中当前项”，其余一律等同“确定”。
      if (ComboDropped(st)) return false;
      if (ApplySettings(st)) ::DestroyWindow(st->hwnd);
      return true;
    case VK_LEFT:
    case VK_RIGHT:
      if (!IsPageTab(hwnd)) return false;
      {
        const int page = (vk == VK_LEFT) ? st->page - 1 : st->page + 1;
        if (page < 0 || page >= kPages) return true;  // 到头了就别wrap
        ShowPage(st, page);
        HWND tab = ::GetDlgItem(st->hwnd, kIdTabAppearance + page);
        ::SetFocus(tab);
        ShowFocusCues(st, tab);
      }
      return true;
    default:
      return false;
  }
}

LRESULT CALLBACK ControlProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  DialogState* st = StateOfParent(hwnd);
  WNDPROC orig = OriginalProc(st, hwnd);
  if (!orig) return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

  if (uMsg == WM_KEYDOWN && HandleDialogKey(st, hwnd, wParam)) return 0;
  // 让下拉/编辑框把 Tab 与方向键交出来（有对话框管理器时才有意义，留着无害）
  if (uMsg == WM_GETDLGCODE) {
    return ::CallWindowProcW(orig, hwnd, uMsg, wParam, lParam) | DLGC_WANTTAB;
  }
  return ::CallWindowProcW(orig, hwnd, uMsg, wParam, lParam);
}

HWND AddControl(DialogState* st, int page, const wchar_t* cls, const wchar_t* text,
                DWORD style, int x, int y, int cx, int cy, int id, DWORD exStyle = 0) {
  HWND h = ::CreateWindowExW(
      exStyle, cls, text,
      WS_CHILD | (page < 0 || page == st->page ? WS_VISIBLE : 0) | style, ScaleOf(st, x),
      ScaleOf(st, y),
      ScaleOf(st, cx), ScaleOf(st, cy), st->hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ModuleHandle(), nullptr);
  ApplyFont(h, st->font);
  if (h && page >= 0) st->pageCtrls[page].push_back(h);
  if (h) {
    st->layout.push_back({h, x, y, cx, cy});
    SubclassControl(st, h);
  }
  return h;
}

// 页签自绘：当前页白底 + 加粗 + 底部强调色条，其余页浅灰底 + 灰边框。
// 互斥状态由系统维护（BM_GETCHECK），这里只负责画。
void DrawTab(DialogState* st, const DRAWITEMSTRUCT* dis) {
  // BS_OWNERDRAW 的按钮不响应 BM_SETCHECK，当前页只能看 st->page
  const bool on = dis->CtlID == static_cast<UINT>(kIdTabAppearance + st->page);
  HDC dc = dis->hDC;
  RECT rc = dis->rcItem;

  HBRUSH bg = ::CreateSolidBrush(on ? RGB(255, 255, 255) : RGB(240, 240, 240));
  ::FillRect(dc, &rc, bg);
  ::DeleteObject(bg);

  HPEN pen = ::CreatePen(PS_SOLID, 1, on ? RGB(0x8a, 0x8a, 0x8a) : RGB(0xb8, 0xb8, 0xb8));
  HGDIOBJ oldPen = ::SelectObject(dc, pen);
  HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
  ::SelectObject(dc, oldBrush);
  ::SelectObject(dc, oldPen);
  ::DeleteObject(pen);

  if (on) {
    RECT bar = rc;
    bar.top = bar.bottom - ScaleOf(st, 2);
    ::InflateRect(&bar, -1, 0);
    HBRUSH accent = ::CreateSolidBrush(RGB(0x0a, 0x6e, 0xd1));
    ::FillRect(dc, &bar, accent);
    ::DeleteObject(accent);
  }

  wchar_t text[64] = {};
  ::GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, on ? RGB(0x1a, 0x1a, 0x1a) : RGB(0x4a, 0x4a, 0x4a));
  HGDIOBJ oldFont = ::SelectObject(dc, (on && st->fontBold) ? st->fontBold : st->font);
  ::DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc, oldFont);

  if (dis->itemState & ODS_FOCUS) {
    RECT fr = rc;
    ::InflateRect(&fr, -ScaleOf(st, 3), -ScaleOf(st, 3));
    ::DrawFocusRect(dc, &fr);
  }
}

// 回退链自绘：系统里没有的字体画成灰色，免得以为它正在生效。
void DrawChainItem(DialogState* st, const DRAWITEMSTRUCT* dis) {
  if (dis->itemID == static_cast<UINT>(-1)) return;
  const bool sel = (dis->itemState & ODS_SELECTED) != 0;
  RECT rc = dis->rcItem;
  ::FillRect(dis->hDC, &rc, ::GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));

  // 条目文本可能来自 config.json 或面板的「使用「%s」」，长字体名远超 LF_FACESIZE；
  // LB_GETTEXT 不做边界检查，必须按实际长度分配。
  const LRESULT textLen = ::SendMessageW(dis->hwndItem, LB_GETTEXTLEN, dis->itemID, 0);
  if (textLen <= 0) return;
  std::vector<wchar_t> text(static_cast<size_t>(textLen) + 1, L'\0');
  ::SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID,
                 reinterpret_cast<LPARAM>(text.data()));
  const std::wstring family(text.data());
  const bool installed = FamilyInstalled(st, family);
  ::SetBkMode(dis->hDC, TRANSPARENT);
  COLORREF color = ::GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
  if (!installed) color = sel ? RGB(0xd8, 0xd8, 0xd8) : RGB(0x9a, 0x9a, 0x9a);
  ::SetTextColor(dis->hDC, color);
  rc.left += ScaleOf(st, 6);
  HGDIOBJ old = ::SelectObject(dis->hDC, st->font);
  ::DrawTextW(dis->hDC, text.data(), -1, &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  ::SelectObject(dis->hDC, old);
}

void ShowPage(DialogState* st, int page) {
  if (page < 0 || page >= kPages) return;
  st->page = page;
  for (int p = 0; p < kPages; ++p) {
    for (HWND h : st->pageCtrls[p]) ::ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
  }
  ::CheckRadioButton(st->hwnd, kIdTabAppearance, kIdTabAdvanced, kIdTabAppearance + page);
  for (int i = 0; i < kPages; ++i) {
    HWND tab = ::GetDlgItem(st->hwnd, kIdTabAppearance + i);
    if (tab) ::InvalidateRect(tab, nullptr, TRUE);
  }
  LogLine(L"SETTINGS: 切到第 %d 页", page + 1);
  // 切页后焦点若落在刚藏起来的控件上，挪到本页第一个控件
  HWND focus = ::GetFocus();
  if (!focus || !::IsWindowVisible(focus)) FocusPageFirst(st);
  if (page == 3) {
    UpdateWebViewUsbState(st);
    RefreshWebViewCacheInfo(st);
  }
}

std::wstring Trimmed(const wchar_t* text) {
  std::wstring s = text ? text : L"";
  size_t a = 0;
  while (a < s.size() && (s[a] == L' ' || s[a] == L'\t')) ++a;
  size_t b = s.size();
  while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) --b;
  return s.substr(a, b - a);
}

FontStack* CurrentStack(DialogState* st) {
  if (st->slot == 1) return &st->staged.font.body;
  if (st->slot == 2) return &st->staged.font.mono;
  return &st->staged.font.ui;
}

bool SameFamily(const std::wstring& a, const std::wstring& b) {
  return ::CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), -1, b.c_str(),
                          -1) == CSTR_EQUAL;
}

void SetLhText(DialogState* st, double lh) {
  wchar_t buf[32];
  ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%.2f", lh);
  ::SetWindowTextW(st->hLh, buf);
}

double ReadLhText(DialogState* st, double fallback) {
  wchar_t buf[32] = {};
  ::GetWindowTextW(st->hLh, buf, ARRAYSIZE(buf));
  wchar_t* end = nullptr;
  const double v = ::wcstod(buf, &end);
  if (end == buf || !(v >= 1.2 && v <= 2.2)) return fallback;
  return v;
}

// CSS 通用族：不是系统字体，但浏览器认识，不能当成「没装」标灰。
bool IsGenericFamily(const std::wstring& name) {
  static const wchar_t* const kGenerics[] = {
      L"system-ui",      L"sans-serif",  L"serif",        L"monospace",  L"cursive",
      L"fantasy",        L"ui-sans-serif", L"ui-serif",   L"ui-monospace", L"ui-rounded",
      L"-apple-system",  L"blinkmacsystemfont", L"emoji",  L"math",       L"fangsong",
  };
  for (const wchar_t* g : kGenerics) {
    if (::CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, name.c_str(), -1, g, -1) ==
        CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

// 字体名是否在枚举到的系统字体里。预览本身由浏览器渲染，这里只用来决定提示文案与灰显。
bool FamilyInstalled(DialogState* st, const std::wstring& name) {
  if (IsGenericFamily(name)) return true;
  for (const std::wstring& f : st->families) {
    if (SameFamily(f, name)) return true;
  }
  return false;
}

void UpdatePreview(DialogState* st) {
  const FontStack* stack = CurrentStack(st);
  bool inherited = false;
  if (stack->empty() && st->slot == 1) {
    stack = &st->staged.font.ui;
    inherited = true;
  }

  // 独立 WebView2 没建起来（运行时缺失、初始化失败）：预览区只会是一块空白，
  // 至少把原因写在下面那句说明里，否则看着就像字体设置没生效。
  if (!st->preview) {
    st->previewMissing = false;
    if (st->hPreviewNote) {
      ::SetWindowTextW(st->hPreviewNote, loc::T(loc::Str::PreviewUnavailable));
      ::InvalidateRect(st->hPreviewNote, nullptr, TRUE);
    }
    return;
  }

  // 整条回退链交给浏览器：第一项缺失时它会自己按顺序往下找，预览里看到的就是最终效果。
  SettingsPreviewSetFont(st->preview, *stack, st->staged.font.sizePx);

  st->previewMissing = !stack->empty() && !FamilyInstalled(st, stack->front());
  if (st->previewMissing) {
    wchar_t note[LF_FACESIZE * 2 + 64] = {};
    ::StringCchPrintfW(note, ARRAYSIZE(note), loc::T(loc::Str::PreviewMissing),
                       stack->front().c_str());
    ::SetWindowTextW(st->hPreviewNote, note);
  } else if (inherited && st->staged.font.body.empty()) {
    ::SetWindowTextW(st->hPreviewNote, loc::T(loc::Str::PreviewInherited));
  } else {
    ::SetWindowTextW(st->hPreviewNote, L"");
  }
  if (st->hPreviewNote) ::InvalidateRect(st->hPreviewNote, nullptr, TRUE);
}

void RefreshChainList(DialogState* st) {
  if (!st->hChainList) return;
  const FontStack* stack = CurrentStack(st);
  ::SendMessageW(st->hChainList, LB_RESETCONTENT, 0, 0);
  for (const std::wstring& name : *stack) {
    ::SendMessageW(st->hChainList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
  }
  if (!stack->empty()) ::SendMessageW(st->hChainList, LB_SETCURSEL, 0, 0);
  if (st->hFamily) ::SetWindowTextW(st->hFamily, stack->empty() ? L"" : stack->front().c_str());
  UpdatePreview(st);
}

int CALLBACK EnumFontProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
  auto* st = reinterpret_cast<DialogState*>(lp);
  if (lf->lfFaceName[0] != L'@') st->families.push_back(lf->lfFaceName);
  return 1;
}

void LoadFamilies(DialogState* st) {
  st->families.clear();
  HDC hdc = ::GetDC(st->hwnd);
  if (!hdc) return;
  LOGFONTW lf = {};
  lf.lfCharSet = DEFAULT_CHARSET;
  ::EnumFontFamiliesExW(hdc, &lf, EnumFontProc, reinterpret_cast<LPARAM>(st), 0);
  ::ReleaseDC(st->hwnd, hdc);

  std::sort(st->families.begin(), st->families.end(),
            [](const std::wstring& a, const std::wstring& b) {
              return ::CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), -1,
                                      b.c_str(), -1) == CSTR_LESS_THAN;
            });
  st->families.erase(
      std::unique(st->families.begin(), st->families.end(),
                  [](const std::wstring& a, const std::wstring& b) { return SameFamily(a, b); }),
      st->families.end());

  if (!st->hFamily) return;
  wchar_t current[LF_FACESIZE] = {};
  ::GetWindowTextW(st->hFamily, current, ARRAYSIZE(current));
  ::SendMessageW(st->hFamily, CB_RESETCONTENT, 0, 0);
  for (const std::wstring& name : st->families) {
    ::SendMessageW(st->hFamily, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
  }
  ::SetWindowTextW(st->hFamily, current);
}


/* ---------------- 字体选择面板 ----------------
   点「添加字体…」在按钮下方弹出：上面筛选框，中间字体列表（每项用该字体自己的字形画），
   底部 刷新 / 确定 / 取消。无命中时列表末行是「使用输入的字体名」，保留添加未枚举名字的能力。
   非阻塞：不跑嵌套消息循环，确认后直接往回退链里加，焦点离开即关闭。 */

struct FontPanel {
  HWND hwnd = nullptr;
  HWND edit = nullptr;
  HWND list = nullptr;
  DialogState* st = nullptr;
  std::vector<std::wstring> items;   // 当前筛选结果（不含末行伪项）
  std::wstring useInput;             // 非空时末行是「使用这个名字」
};

const wchar_t kFontPanelClass[] = L"DopusMdViewerFontPanel";
constexpr int kPanelFilterId = 1;
constexpr int kPanelListId = 2;
constexpr int kPanelRefreshId = 3;
constexpr int kPanelOkId = 4;
constexpr int kPanelCancelId = 5;

FontPanel* PanelOf(HWND hwnd) {
  return reinterpret_cast<FontPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

std::wstring Lowered(const std::wstring& in) {
  std::wstring out = in;
  if (!out.empty()) ::CharLowerBuffW(&out[0], static_cast<DWORD>(out.size()));
  return out;
}

// 按筛选框内容重填列表；没有完全匹配且输入非空时，末尾加一行「使用这个名字」。
void PanelFillList(FontPanel* p) {
  if (!p || !p->list || !p->st) return;
  wchar_t raw[128] = {};
  ::GetWindowTextW(p->edit, raw, ARRAYSIZE(raw));
  const std::wstring filter = Lowered(Trimmed(raw));

  p->items.clear();
  bool exact = false;
  for (const std::wstring& name : p->st->families) {
    if (!filter.empty() && Lowered(name).find(filter) == std::wstring::npos) continue;
    p->items.push_back(name);
    if (Lowered(name) == filter) exact = true;
  }
  p->useInput = (!filter.empty() && !exact) ? Trimmed(raw) : std::wstring();

  ::SendMessageW(p->list, WM_SETREDRAW, FALSE, 0);
  ::SendMessageW(p->list, LB_RESETCONTENT, 0, 0);
  for (const std::wstring& name : p->items) {
    ::SendMessageW(p->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
  }
  if (!p->useInput.empty()) {
    std::wstring row = loc::T(loc::Str::FontUseInput);
    const size_t at = row.find(L"%s");
    if (at != std::wstring::npos) row.replace(at, 2, p->useInput);
    ::SendMessageW(p->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
  }
  ::SendMessageW(p->list, WM_SETREDRAW, TRUE, 0);
  ::InvalidateRect(p->list, nullptr, TRUE);
  if (::SendMessageW(p->list, LB_GETCOUNT, 0, 0) > 0) {
    ::SendMessageW(p->list, LB_SETCURSEL, 0, 0);
  }
}

void PanelAccept(FontPanel* p) {
  if (!p || !p->st) return;
  const int sel = static_cast<int>(::SendMessageW(p->list, LB_GETCURSEL, 0, 0));
  std::wstring name;
  if (sel >= 0 && sel < static_cast<int>(p->items.size())) {
    name = p->items[static_cast<size_t>(sel)];
  } else if (sel >= 0 && !p->useInput.empty()) {
    name = p->useInput;
  }
  if (name.empty()) return;
  AddFontNameToChain(p->st, name);
  SetDirty(p->st, true);
  ::DestroyWindow(p->hwnd);
}

void PanelMoveSel(FontPanel* p, int delta) {
  const int count = static_cast<int>(::SendMessageW(p->list, LB_GETCOUNT, 0, 0));
  if (count <= 0) return;
  int sel = static_cast<int>(::SendMessageW(p->list, LB_GETCURSEL, 0, 0));
  sel = (sel < 0) ? 0 : sel + delta;
  if (sel < 0) sel = 0;
  if (sel >= count) sel = count - 1;
  ::SendMessageW(p->list, LB_SETCURSEL, sel, 0);
}

// 列表项用该字体自己的字形画，一眼分清宋体/黑体/等宽。
void PanelDrawItem(FontPanel* p, const DRAWITEMSTRUCT* dis) {
  if (dis->itemID == static_cast<UINT>(-1)) return;
  RECT rc = dis->rcItem;
  const bool sel = (dis->itemState & ODS_SELECTED) != 0;
  ::FillRect(dis->hDC, &rc, ::GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
  if (dis->itemID == static_cast<UINT>(-1)) return;

  wchar_t text[256] = {};
  ::SendMessageW(p->list, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
  ::SetBkMode(dis->hDC, TRANSPARENT);
  ::SetTextColor(dis->hDC, ::GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
  rc.left += ScaleOf(p->st, 8);

  const bool pseudo = !p->useInput.empty() && dis->itemID == p->items.size();
  HFONT font = nullptr;
  if (!pseudo) {
    LOGFONTW lf = {};
    lf.lfHeight = -::MulDiv(15, static_cast<int>(p->st->dpi ? p->st->dpi : 96), 96);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    ::StringCchCopyW(lf.lfFaceName, LF_FACESIZE, text);
    font = ::CreateFontIndirectW(&lf);
  }
  HGDIOBJ old = ::SelectObject(dis->hDC, font ? font : p->st->font);
  ::DrawTextW(dis->hDC, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                          DT_END_ELLIPSIS);
  ::SelectObject(dis->hDC, old);
  if (font) ::DeleteObject(font);
}

LRESULT CALLBACK PanelControlProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  FontPanel* p = PanelOf(::GetParent(hwnd));
  WNDPROC orig = p ? reinterpret_cast<WNDPROC>(
                         ::GetPropW(hwnd, L"mdv-panel-orig")) : nullptr;
  if (!orig) return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  if (uMsg == WM_KEYDOWN && p) {
    switch (wParam) {
      case VK_ESCAPE:
        ::DestroyWindow(p->hwnd);
        return 0;
      case VK_RETURN:
        PanelAccept(p);
        return 0;
      case VK_DOWN:
        PanelMoveSel(p, 1);
        return 0;
      case VK_UP:
        PanelMoveSel(p, -1);
        return 0;
      default:
        break;
    }
  }
  return ::CallWindowProcW(orig, hwnd, uMsg, wParam, lParam);
}

void PanelSubclass(HWND hwnd) {
  WNDPROC old = reinterpret_cast<WNDPROC>(
      ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PanelControlProc)));
  if (old && old != &PanelControlProc) {
    ::SetPropW(hwnd, L"mdv-panel-orig", reinterpret_cast<HANDLE>(old));
  }
}

LRESULT CALLBACK FontPanelProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  FontPanel* p = PanelOf(hwnd);
  switch (uMsg) {
    case WM_NCCREATE: {
      const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
      p = static_cast<FontPanel*>(cs->lpCreateParams);
      if (p) {
        p->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    case WM_CREATE: {
      if (!p || !p->st) return -1;
      const UINT dpi = p->st->dpi;
      auto scale = [dpi](int v) { return ::MulDiv(v, static_cast<int>(dpi), 96); };
      p->edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, scale(8),
                                  scale(8), scale(300), scale(26), hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelFilterId)),
                                  ModuleHandle(), nullptr);
      ::SendMessageW(p->edit, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(loc::T(loc::Str::FontFilterCue)));
      p->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                                      LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                  scale(8), scale(40), scale(300), scale(210), hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelListId)),
                                  ModuleHandle(), nullptr);
      ::SendMessageW(p->list, LB_SETITEMHEIGHT, 0, scale(26));
      ::CreateWindowExW(0, L"BUTTON", loc::T(loc::Str::RefreshFonts),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, scale(8), scale(258),
                        scale(96), scale(26), hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelRefreshId)),
                        ModuleHandle(), nullptr);
      ::CreateWindowExW(0, L"BUTTON", loc::T(loc::Str::Ok),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, scale(216),
                        scale(258), scale(52), scale(26), hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelOkId)), ModuleHandle(),
                        nullptr);
      ::CreateWindowExW(0, L"BUTTON", loc::T(loc::Str::Cancel),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, scale(272),
                        scale(258), scale(50), scale(26), hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelCancelId)),
                        ModuleHandle(), nullptr);
      // 五个子控件统一套 UI 字体：漏掉按钮的话，它们会用系统默认字体，
      // 在高 DPI 下既没跟着放大，笔画也糊，和设置窗口里其它按钮对不上。
      const int kPanelIds[] = {kPanelFilterId, kPanelListId, kPanelRefreshId, kPanelOkId,
                               kPanelCancelId};
      for (int id : kPanelIds) {
        HWND child = ::GetDlgItem(hwnd, id);
        if (child) ApplyFont(child, p->st->font);
      }
      PanelSubclass(p->edit);
      PanelSubclass(p->list);
      PanelFillList(p);
      if (p->edit) ::SetFocus(p->edit);
      return 0;
    }

    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);
      if (!p) return 0;
      if (id == kPanelFilterId && code == EN_CHANGE) {
        PanelFillList(p);
        return 0;
      }
      if (id == kPanelListId && code == LBN_DBLCLK) {
        PanelAccept(p);
        return 0;
      }
      if (id == kPanelRefreshId) {
        LoadFamilies(p->st);
        PanelFillList(p);
        return 0;
      }
      if (id == kPanelOkId) {
        PanelAccept(p);
        return 0;
      }
      if (id == kPanelCancelId) {
        ::DestroyWindow(hwnd);
        return 0;
      }
      return 0;
    }

    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
      if (p && dis && dis->CtlID == static_cast<UINT>(kPanelListId)) {
        PanelDrawItem(p, dis);
        return TRUE;
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    // 点面板外面（或切到别的窗口）就收起来，跟菜单一样
    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE) {
        ::DestroyWindow(hwnd);
        return 0;
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    // 无边框 popup 没有非客户区（WM_NCPAINT 不会来），边框画在客户区最外一圈：
    // 子控件都从 8px 起排，这 1px 不会被盖住。
    case WM_ERASEBKGND: {
      RECT rc = {};
      ::GetClientRect(hwnd, &rc);
      ::FillRect(reinterpret_cast<HDC>(wParam), &rc, ::GetSysColorBrush(COLOR_WINDOW));
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      RECT rc = {};
      ::GetClientRect(hwnd, &rc);
      // 底色比页面（白）深一档，靠颜色区分层次，不再铺阴影：
      // 细边框外面再套一圈渐变会看成两道边。
      HBRUSH bg = ::CreateSolidBrush(RGB(0xec, 0xec, 0xec));
      ::FillRect(dc, &rc, bg);
      ::DeleteObject(bg);
      HBRUSH frame = ::CreateSolidBrush(RGB(0xbc, 0xbc, 0xbc));
      ::FrameRect(dc, &rc, frame);
      ::DeleteObject(frame);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      if (p) {
        for (HWND child : {p->edit, p->list}) {
          if (child) ::RemovePropW(child, L"mdv-panel-orig");
        }
        if (p->st && p->st->fontPanel == p) p->st->fontPanel = nullptr;
        delete p;
      }
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

bool RegisterFontPanelClass() {
  static bool done = false;
  if (done) return true;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  // CS_DROPSHADOW：弹出面板本身只有 1px 边框，压在浅色页面上边界不清
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = &FontPanelProc;
  wc.hInstance = ModuleHandle();
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kFontPanelClass;
  done = ::RegisterClassExW(&wc) != 0;
  return done;
}

// 在「添加字体…」按钮正下方弹出。按钮窗口用调用方传入的矩形定位。
void OpenFontPanel(DialogState* st, const RECT& anchor) {
  if (!st || !st->hwnd) return;
  if (st->fontPanel) {
    ::SetForegroundWindow(st->fontPanel->hwnd);
    return;
  }
  if (!RegisterFontPanelClass()) return;

  FontPanel* p = new (std::nothrow) FontPanel();
  if (!p) return;
  p->st = st;
  const UINT dpi = st->dpi ? st->dpi : 96;
  auto scale = [dpi](int v) { return ::MulDiv(v, static_cast<int>(dpi), 96); };

  // 边框自己画（见 WM_NCPAINT）：WS_BORDER 用的是系统窗口边框色，浅到几乎看不见。
  // 不加 WS_EX_TOOLWINDOW——工具窗口连 CS_DROPSHADOW 也不画。
  HWND h = ::CreateWindowExW(0, kFontPanelClass, L"", WS_POPUP, anchor.left,
                             anchor.bottom + scale(2), scale(330), scale(300), st->hwnd, nullptr,
                             ModuleHandle(), p);
  if (!h) {
    delete p;
    return;
  }
  st->fontPanel = p;
  ::ShowWindow(h, SW_SHOW);
  ::SetForegroundWindow(h);
  if (p->edit) {
    // 创建时设置过的提示文字有时不显示，等窗口真的可见了再设一次
    ::SendMessageW(p->edit, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(loc::T(loc::Str::FontFilterCue)));
  }
}

void RefreshSkins(DialogState* st) {
  if (!st->hSkin) return;
  SkinList(&st->skins);
  ::SendMessageW(st->hSkin, CB_RESETCONTENT, 0, 0);
  int selected = -1;
  for (size_t i = 0; i < st->skins.size(); ++i) {
    ::SendMessageW(st->hSkin, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(st->skins[i].label.c_str()));
    if (st->skins[i].id == st->staged.skin) selected = static_cast<int>(i);
  }
  if (st->skins.empty()) {
    // 内置目录被删也不让下拉框空着：保留当前 id 作唯一项
    ::SendMessageW(st->hSkin, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(st->staged.skin.c_str()));
    selected = 0;
  } else if (selected < 0) {
    // 配置里的皮肤文件没了（删了或改名）：把当前 id 补成一项显示出来，别悄悄
    // 换成列表第一项——那样打开设置点「确定」就白改了配置。
    ::SendMessageW(st->hSkin, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(st->staged.skin.c_str()));
    selected = static_cast<int>(st->skins.size());
  }
  ::SendMessageW(st->hSkin, CB_SETCURSEL, selected, 0);
}

// 「正文宽度」下面那句说明：勾了不限就讲不限，否则讲当前这条提示。
void UpdateMeasureHint(DialogState* st) {
  if (!st->hMeasureHint) return;
  const bool unlimited = ::IsDlgButtonChecked(st->hwnd, kIdUnlimited) == BST_CHECKED;
  ::SetWindowTextW(st->hMeasureHint,
                   unlimited ? loc::T(loc::Str::MeasureHintUnlimited)
                             : loc::T(loc::Str::MeasureHint));
}

void EnableMeasure(DialogState* st, bool enabled) {
  ::EnableWindow(st->hMeasure, enabled);
  ::EnableWindow(st->hMeasureSpin, enabled);
}

// 表格复制按钮关掉时，复制格式没有意义，跟着置灰。
void EnableCopyFormat(DialogState* st, bool enabled) {
  if (st->hCopyFormat) ::EnableWindow(st->hCopyFormat, enabled ? TRUE : FALSE);
}

// 空白符整个关掉时，范围那组单选按钮没有意义，置灰免得让人以为它还生效。
void EnableCodeWsRange(DialogState* st, bool enabled) {
  if (st->hCodeWsSelected) ::EnableWindow(st->hCodeWsSelected, enabled ? TRUE : FALSE);
  if (st->hCodeWsBoundary) ::EnableWindow(st->hCodeWsBoundary, enabled ? TRUE : FALSE);
  if (st->hCodeWsAll) ::EnableWindow(st->hCodeWsAll, enabled ? TRUE : FALSE);
}

// 本窗口是自建窗口，不是对话框：BS_AUTORADIOBUTTON 的互斥由对话框管理器负责，
// 这里没有那一层，点一个只会勾上它自己，同组旧按钮仍留着勾。读回来时按
// 「仅选中 > 全部 > 边界」取优先级，于是回到之前点过的档位就再也点不动了。
// 组内 id 连续，点下时按区间补一次互斥。
void CheckRadioExclusive(DialogState* st, int id, int firstId, int lastId) {
  if (::IsDlgButtonChecked(st->hwnd, id) != BST_CHECKED) return;
  ::CheckRadioButton(st->hwnd, firstId, lastId, id);
}

// “应用”只在有未保存改动时可用；应用成功后立刻变灰。
void SetDirty(DialogState* st, bool dirty) {
  st->dirty = dirty;
  if (st->hApply) ::EnableWindow(st->hApply, dirty ? TRUE : FALSE);
}

void EnableIdleRelease(DialogState* st, bool enabled) {
  if (st->hIdle) ::EnableWindow(st->hIdle, enabled ? TRUE : FALSE);
  if (st->hIdleSpin) ::EnableWindow(st->hIdleSpin, enabled ? TRUE : FALSE);
}

void LoadUiFromStaged(DialogState* st) {
  RefreshSkins(st);

  wchar_t buf[32];
  ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d", st->staged.font.sizePx);
  ::SetWindowTextW(st->hSize, buf);
  ::SendMessageW(st->hSizeSpin, UDM_SETPOS32, 0, st->staged.font.sizePx);
  SetLhText(st, st->staged.font.lineHeight);
  ::SendMessageW(st->hLhSpin, UDM_SETPOS32, 0,
                 static_cast<int>(st->staged.font.lineHeight * 100.0 + 0.5));

  const bool unlimited = st->staged.font.measureEm == 0;
  ::CheckDlgButton(st->hwnd, kIdUnlimited, unlimited ? BST_CHECKED : BST_UNCHECKED);
  const int measure = unlimited ? 40 : st->staged.font.measureEm;
  ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d", measure);
  ::SetWindowTextW(st->hMeasure, buf);
  ::SendMessageW(st->hMeasureSpin, UDM_SETPOS32, 0, measure);
  EnableMeasure(st, !unlimited);
  UpdateMeasureHint(st);

  ::CheckDlgButton(st->hwnd, kIdRenderMermaid,
                   st->staged.renderMermaid ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdRenderKatex,
                   st->staged.renderKatex ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdRenderHighlight,
                   st->staged.renderHighlight ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdRenderHtml,
                   st->staged.renderInlineHtml ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdRenderLocal,
                   st->staged.renderLocalImages ? BST_CHECKED : BST_UNCHECKED);

  ::CheckDlgButton(st->hwnd, kIdCopyCode,
                   st->staged.copyCodeBlock ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdCopyTable, st->staged.copyTable ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdTableWrap,
                   st->staged.renderTableWrap ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdFindBtn,
                   st->staged.showFindButton ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdOutlineBtn,
                   st->staged.showOutlineButton ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdLineNumbers,
                   st->staged.codeLineNumbers ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdCodeWsOn,
                   st->staged.codeWsOn ? BST_CHECKED : BST_UNCHECKED);
  const int wsRangeId = st->staged.codeWsRange == 1   ? kIdCodeWsSelected
                        : st->staged.codeWsRange == 3 ? kIdCodeWsAll
                                                      : kIdCodeWsBoundary;
  ::CheckRadioButton(st->hwnd, kIdCodeWsSelected, kIdCodeWsAll, wsRangeId);
  ::CheckDlgButton(st->hwnd, kIdIndentedBlocks,
                   st->staged.codeIndentedBlocks ? BST_CHECKED : BST_UNCHECKED);
  EnableCodeWsRange(st, st->staged.codeWsOn);
  if (st->hCopyFormat) {
    ::SendMessageW(st->hCopyFormat, CB_SETCURSEL, st->staged.tableCopyMode == 1 ? 1 : 0, 0);
  }
  EnableCopyFormat(st, st->staged.copyTable);

  const int netId =
      st->staged.netImageMode == 0 ? kIdNetOff : (st->staged.netImageMode == 2 ? kIdNetDoc : kIdNetHost);
  ::CheckRadioButton(st->hwnd, kIdNetDoc, kIdNetOff, netId);
  ::CheckDlgButton(st->hwnd, kIdOutsideParent,
                   st->staged.allowParentImages ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdOutsideAbsolute,
                   st->staged.allowAbsoluteImages ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdEnableLog, st->staged.enableLog ? BST_CHECKED : BST_UNCHECKED);
  if (st->hLinkMode) {
    ::SendMessageW(st->hLinkMode, CB_SETCURSEL, st->staged.openLinksInOpusViewer ? 0 : 1, 0);
  }
  ::EnableWindow(st->hViewerExts, st->staged.openLinksInOpusViewer ? TRUE : FALSE);
  ::CheckDlgButton(st->hwnd, kIdPrewarm, st->staged.prewarm ? BST_CHECKED : BST_UNCHECKED);
  ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d", st->staged.idleReleaseMinutes);
  ::SetWindowTextW(st->hIdle, buf);
  ::SendMessageW(st->hIdleSpin, UDM_SETPOS32, 0, st->staged.idleReleaseMinutes);
  EnableIdleRelease(st, !st->staged.prewarm);

  ::CheckRadioButton(st->hwnd, kIdSlotUi, kIdSlotMono, kIdSlotUi + st->slot);
  RefreshChainList(st);

  ::SetWindowTextW(st->hWebViewDir, ConfigWebViewBaseDir(st->staged).c_str());
  UpdateWebViewUsbState(st);  // U 盘模式下要把这行改写成固定落点
  if (st->page == 3) RefreshWebViewCacheInfo(st);
}

void ReadUiIntoStaged(DialogState* st) {
  const int sel = static_cast<int>(::SendMessageW(st->hSkin, CB_GETCURSEL, 0, 0));
  if (sel >= 0 && sel < static_cast<int>(st->skins.size())) st->staged.skin = st->skins[sel].id;

  BOOL ok = FALSE;
  const UINT size = ::GetDlgItemInt(st->hwnd, kIdSize, &ok, FALSE);
  if (ok && size >= 8 && size <= 48) st->staged.font.sizePx = static_cast<int>(size);
  st->staged.font.lineHeight = ReadLhText(st, st->staged.font.lineHeight);

  if (::IsDlgButtonChecked(st->hwnd, kIdUnlimited) == BST_CHECKED) {
    st->staged.font.measureEm = 0;
  } else {
    const UINT measure = ::GetDlgItemInt(st->hwnd, kIdMeasure, &ok, FALSE);
    if (ok && measure >= 8 && measure <= 200) st->staged.font.measureEm = static_cast<int>(measure);
  }

  st->staged.renderMermaid = ::IsDlgButtonChecked(st->hwnd, kIdRenderMermaid) == BST_CHECKED;
  st->staged.renderKatex = ::IsDlgButtonChecked(st->hwnd, kIdRenderKatex) == BST_CHECKED;
  st->staged.renderHighlight = ::IsDlgButtonChecked(st->hwnd, kIdRenderHighlight) == BST_CHECKED;
  st->staged.renderInlineHtml = ::IsDlgButtonChecked(st->hwnd, kIdRenderHtml) == BST_CHECKED;
  st->staged.renderLocalImages = ::IsDlgButtonChecked(st->hwnd, kIdRenderLocal) == BST_CHECKED;
  st->staged.copyCodeBlock = ::IsDlgButtonChecked(st->hwnd, kIdCopyCode) == BST_CHECKED;
  st->staged.copyTable = ::IsDlgButtonChecked(st->hwnd, kIdCopyTable) == BST_CHECKED;
  st->staged.renderTableWrap = ::IsDlgButtonChecked(st->hwnd, kIdTableWrap) == BST_CHECKED;
  st->staged.showFindButton = ::IsDlgButtonChecked(st->hwnd, kIdFindBtn) == BST_CHECKED;
  st->staged.showOutlineButton = ::IsDlgButtonChecked(st->hwnd, kIdOutlineBtn) == BST_CHECKED;
  st->staged.codeLineNumbers = ::IsDlgButtonChecked(st->hwnd, kIdLineNumbers) == BST_CHECKED;
  st->staged.codeWsOn = ::IsDlgButtonChecked(st->hwnd, kIdCodeWsOn) == BST_CHECKED;
  const bool wsSelChecked = ::IsDlgButtonChecked(st->hwnd, kIdCodeWsSelected) == BST_CHECKED;
  const bool wsAllChecked = ::IsDlgButtonChecked(st->hwnd, kIdCodeWsAll) == BST_CHECKED;
  st->staged.codeWsRange = wsSelChecked ? 1 : (wsAllChecked ? 3 : 2);
  st->staged.codeIndentedBlocks =
      ::IsDlgButtonChecked(st->hwnd, kIdIndentedBlocks) == BST_CHECKED;
  if (st->hCopyFormat) {
    const int format = static_cast<int>(::SendMessageW(st->hCopyFormat, CB_GETCURSEL, 0, 0));
    st->staged.tableCopyMode = (format == 1) ? 1 : 0;
  }

  if (::IsDlgButtonChecked(st->hwnd, kIdNetDoc) == BST_CHECKED) {
    st->staged.netImageMode = 2;
  } else if (::IsDlgButtonChecked(st->hwnd, kIdNetOff) == BST_CHECKED) {
    st->staged.netImageMode = 0;
  } else {
    st->staged.netImageMode = 1;
  }
  st->staged.allowParentImages =
      ::IsDlgButtonChecked(st->hwnd, kIdOutsideParent) == BST_CHECKED;
  st->staged.allowAbsoluteImages =
      ::IsDlgButtonChecked(st->hwnd, kIdOutsideAbsolute) == BST_CHECKED;
  st->staged.enableLog = ::IsDlgButtonChecked(st->hwnd, kIdEnableLog) == BST_CHECKED;
  if (st->hLinkMode) {
    const int linkSel = static_cast<int>(::SendMessageW(st->hLinkMode, CB_GETCURSEL, 0, 0));
    st->staged.openLinksInOpusViewer = (linkSel != 1);  // 0 = Opus 查看器，1 = 系统默认程序
  }
  ActiveWebViewBaseRef(&st->staged) = UtilTrim(ReadText(st->hWebViewDir));
  st->staged.prewarm = ::IsDlgButtonChecked(st->hwnd, kIdPrewarm) == BST_CHECKED;
  const UINT idle = ::GetDlgItemInt(st->hwnd, kIdIdle, &ok, FALSE);
  if (ok && idle <= 1440) st->staged.idleReleaseMinutes = static_cast<int>(idle);
}

// 保存失败的弹窗：主行 + 路径 + 错误码与系统消息 + 一句针对性的建议。
// 日志关着时这是用户唯一能拿到的线索，所以一次给全，不写"详见日志"。
void ShowSaveFailed(HWND owner, const ConfigSaveFailure& fail) {
  const wchar_t* advice = loc::T(loc::Str::SaveAdviceOther);
  switch (fail.code) {
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
      advice = loc::T(loc::Str::SaveAdviceMissing);
      break;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
      advice = loc::T(loc::Str::SaveAdviceDenied);
      break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      advice = loc::T(loc::Str::SaveAdviceBusy);
      break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      advice = loc::T(loc::Str::SaveAdviceDisk);
      break;
    default:
      break;
  }

  std::wstring text = loc::T(loc::Str::SaveFailed);
  text += L"\r\n\r\n";

  if (fail.path.empty()) {
    text += loc::T(loc::Str::SaveFailedNoPath);
  } else {
    wchar_t line[MAX_PATH + 32];
    ::StringCchPrintfW(line, ARRAYSIZE(line), loc::T(loc::Str::SaveFailedPath), fail.path.c_str());
    text += line;
  }

  if (fail.code != 0) {
    const std::wstring sys = UtilWin32ErrorText(fail.code);
    wchar_t line[512];
    ::StringCchPrintfW(line, ARRAYSIZE(line), loc::T(loc::Str::SaveFailedError), fail.code,
                       sys.empty() ? loc::T(loc::Str::Unknown) : sys.c_str());
    text += L"\r\n";
    text += line;
  }

  text += L"\r\n\r\n";
  text += advice;

  ::MessageBoxW(owner, text.c_str(), L"DopusMdViewer", MB_OK | MB_ICONERROR);
}

bool ApplySettings(DialogState* st) {
  ReadUiIntoStaged(st);
  const Config saved = ConfigSnapshot();
  if (ActiveWebViewBaseRef(&st->staged) != ConfigWebViewBaseDir(saved)) {
    std::wstring error;
    if (!ValidateWebViewDir(ActiveWebViewBaseRef(&st->staged), &error)) {
      ::MessageBoxW(st->hwnd, error.c_str(), loc::T(loc::Str::WebViewCache),
                    MB_OK | MB_ICONERROR);
      return false;
    }
  }
  ConfigSaveFailure fail{};
  if (!ConfigSave(st->staged, &fail)) {
    ShowSaveFailed(st->hwnd, fail);
    return false;
  }
  ViewerNotifyConfigChanged();
  WebHostPrewarm(st->staged.prewarm, st->staged.idleReleaseMinutes);
  SetDirty(st, false);
  if (st->page == 3) RefreshWebViewCacheInfo(st);
  LogLine(L"SETTINGS: 已应用 skin=\"%s\" size=%d lh=%.2f measure=%d net=%d", st->staged.skin.c_str(),
          st->staged.font.sizePx, st->staged.font.lineHeight, st->staged.font.measureEm,
          st->staged.netImageMode);
  return true;
}

void OpenFolder(HWND owner, const std::wstring& dir) {
  if (dir.empty()) {
    ::MessageBoxW(owner, loc::T(loc::Str::NoFolder), L"DopusMdViewer", MB_OK | MB_ICONERROR);
    return;
  }
  UtilEnsureDir(dir);
  const HINSTANCE r = ::ShellExecuteW(owner, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    ::MessageBoxW(owner, loc::T(loc::Str::OpenFolderFailed), L"DopusMdViewer",
                  MB_OK | MB_ICONERROR);
  }
}

std::wstring ReadText(HWND hwnd) {
  if (!hwnd) return std::wstring();
  const int len = ::GetWindowTextLengthW(hwnd);
  if (len <= 0) return std::wstring();
  std::wstring text(static_cast<size_t>(len) + 1, L'\0');
  const int got = ::GetWindowTextW(hwnd, &text[0], len + 1);
  if (got <= 0) return std::wstring();
  text.resize(static_cast<size_t>(got));
  return text;
}

std::wstring WebViewBaseFromUi(DialogState* st) {
  return UtilTrim(ReadText(st->hWebViewDir));
}

// 「缓存位置」这一行改的是当前模式对应的那个键：U 盘版与普通版各存一份，
// 同一台机器插拔 U 盘时互不覆盖。读用 ConfigWebViewBaseDir，写用这个。
std::wstring& ActiveWebViewBaseRef(Config* cfg) {
  return UtilIsUsbMode() ? cfg->webviewBaseDirUsb : cfg->webviewBaseDir;
}

bool ResolveWebViewDir(const std::wstring& raw, std::wstring* actual) {
  // U 盘模式下 UtilWebView2BaseDir 会忽略配置值，直接给 %TEMP%\DopusMdViewer，
  // 所以这里不用单独判模式：界面置灰只是防误操作，真正的口径在 util 里。
  const std::wstring base = UtilWebView2BaseDir(UtilTrim(raw));
  if (base.empty()) return false;
  *actual = UtilJoin(base, L"WebView2");
  return UtilIsWebView2UserDataDir(*actual);
}

bool ValidateWebViewDir(const std::wstring& raw, std::wstring* error) {
  std::wstring actual;
  if (!ResolveWebViewDir(raw, &actual) || !UtilEnsureDir(actual)) {
    if (error) *error = loc::T(loc::Str::WebViewPathInvalid);
    return false;
  }

  wchar_t name[64];
  ::StringCchPrintfW(name, ARRAYSIZE(name), L".mdv-write-%llu",
                     static_cast<unsigned long long>(::GetTickCount64()));
  const std::wstring probe = UtilJoin(actual, name);
  HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = loc::T(loc::Str::WebViewPathInvalid);
    return false;
  }
  ::CloseHandle(h);
  return true;
}

void RefreshWebViewCacheInfo(DialogState* st) {
  if (!st->hWebViewInfo) return;

  const std::wstring raw = WebViewBaseFromUi(st);
  std::wstring actual;
  if (!ResolveWebViewDir(raw, &actual)) {
    ::SetWindowTextW(st->hWebViewInfo, loc::T(loc::Str::WebViewPathInvalid));
    return;
  }

  // 缓存其实是两份：查看器的 WebView2 与设置页字体预览的 WebView2-Settings。
  uint64_t bytes = UtilDirSize(actual);
  const std::wstring settings = UtilWebView2SettingsDir(raw);
  if (!settings.empty() && _wcsicmp(settings.c_str(), actual.c_str()) != 0) {
    bytes += UtilDirSize(settings);
  }

  const std::wstring size = UtilFormatSize(bytes);
  // 这一行只有一个行高，写长了会被裁掉：路径在输入框里，这里只说大小与缘由，
  // 并且把大小放最前面——真被截断时也不会把数字切没。
  const std::wstring source = UtilIsUsbMode() ? UtilWebView2BaseSource(raw) : std::wstring();
  wchar_t text[512];
  if (source == L"temp" || source == L"data" || source == L"local") {
    loc::Str fmt = loc::Str::WebViewUsbTemp;
    if (source == L"data") {
      fmt = loc::Str::WebViewUsbData;
    } else if (source == L"local") {
      fmt = loc::Str::WebViewUsbLocal;
    }
    ::StringCchPrintfW(text, ARRAYSIZE(text), loc::T(fmt), size.c_str());
  } else {
    // 非 U 盘，或 U 盘模式下手填了位置：和普通模式同一句
    const std::wstring limit = UtilFormatSize(kWebViewCacheAutoCleanBytes);
    ::StringCchPrintfW(text, ARRAYSIZE(text), loc::T(loc::Str::WebViewCacheInfo), size.c_str(),
                       limit.c_str());
  }
  ::SetWindowTextW(st->hWebViewInfo, text);
}

// U 盘模式下位置照样能手填；留空时走自动选路（%TEMP% → 数据目录 → 本机），
// 这里把「自动会落到哪」写进输入框的提示条，免得用户以为留空是没设置。
void UpdateWebViewUsbState(DialogState* st) {
  if (!st->hWebViewDir) return;
  if (!UtilIsUsbMode()) return;

  const std::wstring autoBase = UtilWebView2BaseDir(L"");
  if (!autoBase.empty()) {
    ::SendMessageW(st->hWebViewDir, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(autoBase.c_str()));
  }
}

void BrowseWebViewDir(DialogState* st) {
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninit = SUCCEEDED(init);

  IFileOpenDialog* dialog = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
  if (SUCCEEDED(hr) && dialog) {
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
      dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }

    std::wstring base = UtilNormalizeLocalDir(WebViewBaseFromUi(st));
    if (base.empty()) base = UtilNormalizeLocalDir(UtilDefaultWebView2BaseDir());
    if (!base.empty() && UtilDirExists(base)) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(::SHCreateItemFromParsingName(base.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
          item) {
        dialog->SetFolder(item);
        item->Release();
      }
    }

    if (SUCCEEDED(dialog->Show(st->hwnd))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)) && item) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
          ::SetWindowTextW(st->hWebViewDir, path);
          ::CoTaskMemFree(path);
          SetDirty(st, true);
          RefreshWebViewCacheInfo(st);
        }
        item->Release();
      }
    }
    dialog->Release();
  } else {
    ::MessageBoxW(st->hwnd, loc::T(loc::Str::WebViewPathInvalid), loc::T(loc::Str::WebViewCache),
                  MB_OK | MB_ICONERROR);
  }

  if (uninit) ::CoUninitialize();
}

void OpenWebViewDir(DialogState* st) {
  // 打开父目录：缓存实际是 WebView2 与 WebView2-Settings 两个子目录，只打开其中一个
  // 会让人以为另一个不存在。
  const std::wstring base = UtilWebView2BaseDir(WebViewBaseFromUi(st));
  if (base.empty()) {
    ::MessageBoxW(st->hwnd, loc::T(loc::Str::WebViewPathInvalid), loc::T(loc::Str::WebViewCache),
                  MB_OK | MB_ICONERROR);
    return;
  }
  UtilEnsureDir(base);
  OpenFolder(st->hwnd, base);
  RefreshWebViewCacheInfo(st);
}

void ClearWebViewCache(DialogState* st) {
  std::wstring actual;
  if (!ResolveWebViewDir(WebViewBaseFromUi(st), &actual)) {
    ::MessageBoxW(st->hwnd, loc::T(loc::Str::WebViewPathInvalid), loc::T(loc::Str::WebViewCache),
                  MB_OK | MB_ICONERROR);
    return;
  }
  if (::MessageBoxW(st->hwnd, loc::T(loc::Str::WebViewClearConfirm),
                    loc::T(loc::Str::WebViewCache),
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  HCURSOR old = ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
  const bool ok = WebHostClearUserData(actual);
  // 设置页字体预览那份缓存是独立目录，别漏。
  const std::wstring settings = UtilWebView2SettingsDir(WebViewBaseFromUi(st));
  if (!settings.empty() && _wcsicmp(settings.c_str(), actual.c_str()) != 0) {
    WebHostClearUserData(settings);
  }
  // 旧版把缓存放在数据根下；按钮顺手把那个目录也清掉。
  const std::wstring legacy = UtilJoin(UtilDataDir(), L"WebView2");
  if (!legacy.empty() && UtilIsWebView2UserDataDir(legacy) &&
      _wcsicmp(legacy.c_str(), actual.c_str()) != 0) {
    WebHostClearUserData(legacy);
  }
  ::SetCursor(old);
  if (!ok) {
    const wchar_t* message = ViewerLiveCount() > 0 ? loc::T(loc::Str::WebViewClearCloseFirst)
                                                   : loc::T(loc::Str::WebViewClearFailed);
    ::MessageBoxW(st->hwnd, message, loc::T(loc::Str::WebViewCache), MB_OK | MB_ICONERROR);
  }
  RefreshWebViewCacheInfo(st);
}

void ShowWebViewHelp(DialogState* st) {
  const std::wstring defaultBase = UtilNormalizeLocalDir(UtilDefaultWebView2BaseDir());
  const std::wstring defaultDir =
      defaultBase.empty() ? std::wstring() : UtilJoin(defaultBase, L"WebView2");

  std::wstring actual;
  const bool valid = ResolveWebViewDir(WebViewBaseFromUi(st), &actual);
  const std::wstring shown = valid ? actual : std::wstring(L"—");
  const std::wstring defaultShown = defaultDir.empty() ? std::wstring(L"—") : defaultDir;
  const std::wstring limit = UtilFormatSize(kWebViewCacheAutoCleanBytes);

  wchar_t text[2048];
  ::StringCchPrintfW(text, ARRAYSIZE(text), loc::T(loc::Str::WebViewDirHelp),
                     defaultShown.c_str(), shown.c_str(), limit.c_str());

  // U 盘模式下先解释选路规则，再说具体位置。
  std::wstring body = text;
  if (UtilIsUsbMode()) {
    body = std::wstring(loc::T(loc::Str::WebViewUsbHelp)) + L"\r\n\r\n" + body;
  }
  const std::wstring copy = valid ? actual : std::wstring();
  ShowTextHelp(st->hwnd, loc::T(loc::Str::WebViewCache), body.c_str(), copy.c_str(),
               loc::T(loc::Str::CopyPath), st->font);
}

// 把一个字体名加进当前槽位的回退链；重名（SameFamily）直接忽略。
void AddFontNameToChain(DialogState* st, const std::wstring& name) {
  const std::wstring trimmed = Trimmed(name.c_str());
  if (trimmed.empty()) return;
  FontStack* stack = CurrentStack(st);
  for (const std::wstring& existing : *stack) {
    if (SameFamily(existing, trimmed)) return;
  }
  stack->push_back(trimmed);
  RefreshChainList(st);
  ::SendMessageW(st->hChainList, LB_SETCURSEL, static_cast<WPARAM>(stack->size() - 1), 0);
}

void RemoveFromChain(DialogState* st) {
  FontStack* stack = CurrentStack(st);
  const int sel = static_cast<int>(::SendMessageW(st->hChainList, LB_GETCURSEL, 0, 0));
  if (sel < 0 || sel >= static_cast<int>(stack->size())) return;
  stack->erase(stack->begin() + sel);
  RefreshChainList(st);
}

void MoveInChain(DialogState* st, int delta) {
  FontStack* stack = CurrentStack(st);
  const int sel = static_cast<int>(::SendMessageW(st->hChainList, LB_GETCURSEL, 0, 0));
  const int target = sel + delta;
  if (sel < 0 || target < 0 || sel >= static_cast<int>(stack->size()) ||
      target >= static_cast<int>(stack->size())) {
    return;
  }
  std::swap((*stack)[sel], (*stack)[target]);
  RefreshChainList(st);
  ::SendMessageW(st->hChainList, LB_SETCURSEL, target, 0);
  UpdatePreview(st);
}

void BuildUi(DialogState* st) {
  // 顶部页签
  struct TabDef {
    int id;
    const wchar_t* text;
  };
  const TabDef tabs[] = {{kIdTabAppearance, loc::T(loc::Str::TabAppearance)},
                         {kIdTabFont, loc::T(loc::Str::TabFont)},
                         {kIdTabRender, loc::T(loc::Str::TabRender)},
                         {kIdTabAdvanced, loc::T(loc::Str::TabAdvanced)}};
  for (int i = 0; i < 4; ++i) {
    // 四个页签是一个单选组：只给组里第一个 WS_TABSTOP，整组占一个 Tab 站位
    //（Windows 对同组单选按钮也会把其余的清掉，这里写明白就不靠它）。
    // 换页用左右方向键，也照旧能点。
    // 自绘：单选只负责互斥状态，外观由 DrawTab 画（白底加粗 + 强调色下划线），
    // BS_PUSHLIKE 那套内阴影在浅色背景上几乎看不出当前页。
    AddControl(st, -1, L"BUTTON", tabs[i].text,
               BS_AUTORADIOBUTTON | BS_OWNERDRAW | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0),
               12 + i * 90, 10, 84, 26, tabs[i].id);
  }

  // ---------------- 外观 ----------------
  int y = 54;
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::Skin), SS_LEFT, 16, y + 3, 70, 20, 0);
  // 下拉框收窄到 280：右侧留给「打开皮肤目录」，找皮肤文件不用再跑去高级页
  st->hSkin = AddControl(st, 0, L"COMBOBOX", L"",
                         CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 100, y,
                         280, 200, kIdSkin);
  AddControl(st, 0, L"BUTTON", loc::T(loc::Str::OpenSkins), BS_PUSHBUTTON | WS_TABSTOP, 390, y,
             114, 24, kIdOpenSkins);

  y = 92;
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::FontSize), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hSize = AddControl(st, 0, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 100, y, 70, 24,
                         kIdSize, WS_EX_CLIENTEDGE);
  st->hSizeSpin = AddControl(st, 0, UPDOWN_CLASSW, L"",
                             UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
                             0, 0, 0, 0, kIdSizeSpin);
  ::SendMessageW(st->hSizeSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hSize), 0);
  ::SendMessageW(st->hSizeSpin, UDM_SETRANGE32, 8, 48);
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::FontSizeUnit), SS_LEFT, 180, y + 3, 180, 20, 0);

  y = 126;
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::LineHeight), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hLh = AddControl(st, 0, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 100, y, 70, 24, kIdLh,
                       WS_EX_CLIENTEDGE);
  st->hLhSpin = AddControl(st, 0, UPDOWN_CLASSW, L"", UDS_ALIGNRIGHT | UDS_ARROWKEYS, 0, 0, 0, 0,
                           kIdLhSpin);
  ::SendMessageW(st->hLhSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hLh), 0);
  ::SendMessageW(st->hLhSpin, UDM_SETRANGE32, 120, 220);
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::LineHeightUnit), SS_LEFT, 180, y + 3, 260, 20, 0);

  y = 160;
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::Measure), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hMeasure = AddControl(st, 0, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 100, y, 70, 24,
                            kIdMeasure, WS_EX_CLIENTEDGE);
  st->hMeasureSpin = AddControl(st, 0, UPDOWN_CLASSW, L"",
                                UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT |
                                    UDS_NOTHOUSANDS,
                                0, 0, 0, 0, kIdMeasureSpin);
  ::SendMessageW(st->hMeasureSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hMeasure), 0);
  ::SendMessageW(st->hMeasureSpin, UDM_SETRANGE32, 8, 200);
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::MeasureUnit), SS_LEFT, 180, y + 3, 120, 20, 0);
  st->hUnlimited = AddControl(st, 0, L"BUTTON", loc::T(loc::Str::Unlimited),
                              BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 300, y, 120, 24,
                              kIdUnlimited);
  st->hMeasureHint = AddControl(st, 0, L"STATIC", loc::T(loc::Str::MeasureHint), SS_LEFT, 16, 210,
                                520, 20, kIdMeasureHint);

  // ---------------- 字体 ----------------
  y = 48;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Slot), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hSlot[0] = AddControl(st, 1, L"BUTTON", loc::T(loc::Str::SlotUi),
                            BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 100, y, 70, 24, kIdSlotUi);
  st->hSlot[1] = AddControl(st, 1, L"BUTTON", loc::T(loc::Str::SlotBody),
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 176, y, 70, 24, kIdSlotBody);
  st->hSlot[2] = AddControl(st, 1, L"BUTTON", loc::T(loc::Str::SlotMono),
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 252, y, 70, 24, kIdSlotMono);

  // 字体：不再挂一个可编辑下拉框——要先打字再展开才看得到列表，用起来别扭。
  // 现在只有一个按钮，点开是带筛选的列表（面板里也能刷新字体列表）。
  y = 82;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Family), SS_LEFT, 16, y + 3, 70, 20, 0);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::FontAdd), BS_PUSHBUTTON | WS_TABSTOP, 100, y,
             140, 26, kIdFamilyAdd);

  y = 120;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Chain), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hChainList = AddControl(st, 1, L"LISTBOX", L"",
                              LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER |
                                  WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                              100, y, 330, 200, kIdFamilyList);
  if (st->hChainList) {
    ::SendMessageW(st->hChainList, LB_SETITEMHEIGHT, 0, ScaleOf(st, 22));
  }
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::ChainUp), BS_PUSHBUTTON | WS_TABSTOP, 436, y, 60,
             24, kIdChainUp);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::ChainDown), BS_PUSHBUTTON | WS_TABSTOP, 436,
             y + 30, 60, 24, kIdChainDown);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::ChainDel), BS_PUSHBUTTON | WS_TABSTOP, 436,
             y + 60, 60, 24, kIdChainDel);

  y = 332;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Preview), SS_LEFT, 16, y + 3, 70, 20, 0);
  {
    RECT prc = {ScaleOf(st, kPreviewX), ScaleOf(st, kPreviewY), ScaleOf(st, kPreviewX + kPreviewW),
                ScaleOf(st, kPreviewY + kPreviewH)};
    st->preview = SettingsPreviewCreate(st->hwnd, prc);
    st->hPreview = SettingsPreviewHwnd(st->preview);
    if (st->hPreview) {
      st->pageCtrls[1].push_back(st->hPreview);
      // 预览是独立子窗口，不走 AddControl，但要参与 DPI 重排。
      st->layout.push_back({st->hPreview, kPreviewX, kPreviewY, kPreviewW, kPreviewH});
    }
  }
  st->hPreviewNote = AddControl(st, 1, L"STATIC", L"", SS_LEFT, 100, y + 70, 430, 20,
                                kIdPreviewNote);

  // ---------------- 渲染 ----------------
  // 按「渲染什么 / 界面多出什么 / 代码块 / 外部图片」分四组纵排。原来左右两列没有
  // 任何标题，网络图片那组又插在左列下方，看着像第 6 个开关。
  struct RenderDef {
    int id;
    const wchar_t* text;
  };
  const RenderDef renders[] = {{kIdRenderMermaid, loc::T(loc::Str::RenderMermaid)},
                               {kIdRenderKatex, loc::T(loc::Str::RenderKatex)},
                               {kIdRenderHighlight, loc::T(loc::Str::RenderHighlight)},
                               {kIdRenderHtml, loc::T(loc::Str::RenderHtml)},
                               {kIdRenderLocal, loc::T(loc::Str::RenderLocal)}};

  y = 48;
  AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderGroupContent), BS_GROUPBOX, 16, y, 524,
             120, 0);
  // 三个短开关横排，长的（外部图片路径）单独占一行
  st->hRender[0] = AddControl(st, 2, L"BUTTON", renders[0].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                              28, y + 16, 170, 24, renders[0].id);
  st->hRender[1] = AddControl(st, 2, L"BUTTON", renders[1].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                              208, y + 16, 170, 24, renders[1].id);
  st->hRender[2] = AddControl(st, 2, L"BUTTON", renders[2].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                              388, y + 16, 150, 24, renders[2].id);
  st->hRender[3] = AddControl(st, 2, L"BUTTON", renders[3].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                              28, y + 40, 170, 24, renders[3].id);
  st->hRender[4] = AddControl(st, 2, L"BUTTON", renders[4].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                              208, y + 40, 170, 24, renders[4].id);
  // 这句解释的是上一行「渲染原始 HTML」，所以紧贴它放。原先在组末最后一行，
  // 紧挨着的是「允许图片引用文档目录外的路径」，读起来像是在解释那条。
  AddControl(st, 2, L"STATIC", loc::T(loc::Str::RenderHtmlHint), SS_LEFT, 28, y + 64, 500, 18,
             0);
  // 图片的路径策略与「本地图片」是同一件事，放在同一组里。
  // 目录外分两类，各自一个开关：../ 上级目录常见于兄弟目录引用，绝对路径更少见也更危险。
  st->hOutsideParent = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::AllowParentImages),
                                  BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 28, y + 88, 240, 24,
                                  kIdOutsideParent);
  st->hOutsideAbsolute = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::AllowAbsoluteImages),
                                    BS_AUTOCHECKBOX | WS_TABSTOP, 280, y + 88, 224, 24,
                                    kIdOutsideAbsolute);

  // 表格复制按钮与复制格式挪去「表格」组后，这里只剩三个短开关，排成一行
  y = 176;
  AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderGroupUi), BS_GROUPBOX, 16, y, 524, 48, 0);
  st->hCopyCode = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderCopyCode),
                             BS_AUTOCHECKBOX | WS_TABSTOP, 28, y + 16, 140, 24, kIdCopyCode);
  st->hFindBtn = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderFindButton),
                            BS_AUTOCHECKBOX | WS_TABSTOP, 176, y + 16, 140, 24, kIdFindBtn);
  st->hOutlineBtn = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderOutlineButton),
                               BS_AUTOCHECKBOX | WS_TABSTOP, 324, y + 16, 140, 24,
                               kIdOutlineBtn);

  y = 232;
  AddControl(st, 2, L"BUTTON", loc::T(loc::Str::CodeBlockGroup), BS_GROUPBOX, 16, y, 524, 76,
             kIdCodeGroup);
  st->hLineNumbers = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderLineNumbers),
                                BS_AUTOCHECKBOX | WS_TABSTOP, 28, y + 16, 140, 24,
                                kIdLineNumbers);
  st->hCodeWsOn = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderCodeWhitespace),
                             BS_AUTOCHECKBOX | WS_TABSTOP, 176, y + 16, 130, 24, kIdCodeWsOn);
  // 范围三档按常显多少递增，创建顺序即 tab 顺序
  st->hCodeWsSelected = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::CodeWsSelected),
                                   BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 312, y + 16, 76,
                                   24, kIdCodeWsSelected);
  st->hCodeWsBoundary = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::CodeWsBoundary),
                                   BS_AUTORADIOBUTTON | WS_TABSTOP, 396, y + 16, 66, 24,
                                   kIdCodeWsBoundary);
  st->hCodeWsAll = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::CodeWsAll),
                              BS_AUTORADIOBUTTON | WS_TABSTOP, 470, y + 16, 60, 24, kIdCodeWsAll);
  st->hIndentedBlocks = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderIndentedBlocks),
                                   BS_AUTOCHECKBOX | WS_TABSTOP, 28, y + 44, 400, 24,
                                   kIdIndentedBlocks);

  // 表格：复制按钮与复制格式从「界面控件」迁进来，另加折行模式开关。
  // 按创建顺序进 Tab 序列：代码块 → 表格 → 外部图片。
  y = 316;
  AddControl(st, 2, L"BUTTON", loc::T(loc::Str::TableGroup), BS_GROUPBOX, 16, y, 524, 76, 0);
  st->hCopyTable = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderCopyTable),
                              BS_AUTOCHECKBOX | WS_TABSTOP, 28, y + 16, 140, 24, kIdCopyTable);
  AddControl(st, 2, L"STATIC", loc::T(loc::Str::RenderCopyFormat), SS_LEFT, 176, y + 19, 80, 20,
             0);
  st->hCopyFormat = AddControl(st, 2, L"COMBOBOX", L"",
                               CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 258,
                               y + 16, 130, 120, kIdCopyFormat);
  if (st->hCopyFormat) {
    ::SendMessageW(st->hCopyFormat, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(loc::T(loc::Str::CopyFormatTsv)));
    ::SendMessageW(st->hCopyFormat, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(loc::T(loc::Str::CopyFormatMarkdown)));
    ::SendMessageW(st->hCopyFormat, CB_SETCURSEL, 0, 0);
  }
  AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderTableAutoWrap),
             BS_AUTOCHECKBOX | WS_TABSTOP, 28, y + 44, 400, 24, kIdTableWrap);

  y = 400;
  AddControl(st, 2, L"BUTTON", loc::T(loc::Str::RenderGroupImages), BS_GROUPBOX, 16, y, 524, 104,
             0);
  AddControl(st, 2, L"STATIC", loc::T(loc::Str::NetAskWhen), SS_LEFT, 28, y + 14, 300, 20, 0);
  // 两个档位的说明写全了，一行放不下两个；按实测文字宽度定宽，免得 150% 缩放下被截断
  {
    const wchar_t* docLabel = loc::T(loc::Str::NetPerDoc);
    const wchar_t* hostLabel = loc::T(loc::Str::NetPerHost);
    const int docW = 22 + UnscaleOf(st, TextWidth(st, docLabel)) + 6;
    const int hostW = 22 + UnscaleOf(st, TextWidth(st, hostLabel)) + 6;
    st->hNetDoc = AddControl(st, 2, L"BUTTON", docLabel,
                             BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 28, y + 34, docW, 24,
                             kIdNetDoc);
    st->hNetHost = AddControl(st, 2, L"BUTTON", hostLabel,
                              BS_AUTORADIOBUTTON | WS_TABSTOP, 28, y + 56, hostW, 24,
                              kIdNetHost);
  }
  st->hNetOff = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::NetNever),
                           BS_AUTORADIOBUTTON | WS_TABSTOP, 28, y + 78, 200, 24, kIdNetOff);

  // ---------------- 高级 ----------------
  std::wstring version = UtilModuleVersion();
  if (version.empty()) version = loc::T(loc::Str::Unknown);
  std::wstring runtime = WebHostRuntimeInfo();
  if (runtime.empty()) runtime = loc::T(loc::Str::RuntimeMissing);

  y = 50;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::Version), SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", version.c_str(), SS_LEFT, 110, y + 2, 280, 20, kIdVersion);
  st->hCopyVersion = AddControl(st, 3, L"BUTTON", loc::T(loc::Str::CopyVersionInfo),
                                BS_PUSHBUTTON | WS_TABSTOP, 400, y - 2, 124, 26,
                                kIdCopyVersion);

  y = 78;
  AddControl(st, 3, L"STATIC", L"WebView2", SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", runtime.c_str(), SS_LEFT, 110, y + 2, 280, 20, kIdWebView);

  // 排查问题先看日志，所以日志与它的目录放最上面；再往下才是引擎生命周期
  // （预热、空闲释放）。
  y = 110;
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::LogEnabled), BS_AUTOCHECKBOX | WS_TABSTOP,
             16, y, 300, 24, kIdEnableLog);

  // 日志目录紧贴「记录日志」：开关和它的落点不该隔着三个控件。
  y = 140;
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::OpenLog), BS_PUSHBUTTON | WS_TABSTOP, 16, y,
             130, 26, kIdOpenLog);

  y = 178;
  {
    // 问号紧跟描述。复选框的宽度必须收在文字后面——给宽了它就会盖住右侧的问号
    // （两个都是子窗口，重叠时谁最后重绘谁在上）。
    const wchar_t* prewarmLabel = loc::T(loc::Str::Prewarm);
    const int boxW = 22 + UnscaleOf(st, TextWidth(st, prewarmLabel)) + 6;
    st->hPrewarm = AddControl(st, 3, L"BUTTON", prewarmLabel, BS_AUTOCHECKBOX | WS_TABSTOP, 16,
                              y, boxW, 24, kIdPrewarm);
    AddControl(st, 3, L"BUTTON", L"?", BS_PUSHBUTTON | WS_TABSTOP, 16 + boxW + 4, y, 24, 24,
               kIdPrewarmHelp);
  }

  y = 212;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::IdleRelease), SS_LEFT, 16, y + 3, 120, 20, 0);
  st->hIdle = AddControl(st, 3, L"EDIT", L"",
                         ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, 140, y, 70, 24, kIdIdle,
                         WS_EX_CLIENTEDGE);
  st->hIdleSpin = AddControl(st, 3, UPDOWN_CLASSW, L"",
                             UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
                             0, 0, 0, 0, kIdIdleSpin);
  ::SendMessageW(st->hIdleSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hIdle), 0);
  ::SendMessageW(st->hIdleSpin, UDM_SETRANGE32, 0, 1440);
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::IdleUnit), SS_LEFT, 220, y + 3, 260, 20, 0);

  // 文档内链接：这是内容行为，和上面的引擎开关、下面的路径各成一组。
  // 名单有几十项，页面上放不下多行框，用一个按钮开对话框编辑；选了系统默认程序时置灰。
  y = 252;
  {
    // 下拉框而不是勾选框：勾选框只说明「开」是什么，「关」会变成什么得靠猜。
    const wchar_t* label = loc::T(loc::Str::LinkOpenMode);
    const int labelW = UnscaleOf(st, TextWidth(st, label)) + 4;
    AddControl(st, 3, L"STATIC", label, SS_LEFT, 16, y + 4, labelW, 20, 0);
    const int comboX = 16 + labelW + 6;
    st->hLinkMode = AddControl(st, 3, L"COMBOBOX", L"",
                               CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                               comboX, y, 150, 200, kIdLinkOpenMode);
    if (st->hLinkMode) {
      ::SendMessageW(st->hLinkMode, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(loc::T(loc::Str::LinkOpenOpus)));
      ::SendMessageW(st->hLinkMode, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(loc::T(loc::Str::LinkOpenSystem)));
    }
    st->hViewerExts = AddControl(st, 3, L"BUTTON", loc::T(loc::Str::ViewerExts),
                                 BS_PUSHBUTTON | WS_TABSTOP, comboX + 158, y, 120, 24,
                                 kIdViewerExtsBtn);
  }

  y = 288;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::ConfigFolder), SS_LEFT, 16, y, 200, 20, 0);
  std::wstring dataDir = UtilDataDir();
  if (dataDir.empty()) dataDir = loc::T(loc::Str::NoFolder);
  HWND hPath = AddControl(st, 3, L"EDIT", dataDir.c_str(),
                          WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 16, y + 22,
                          480, 40, kIdConfigPath, WS_EX_CLIENTEDGE);
  int pathH = ScaleOf(st, 40);
  if (hPath) {
    HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hPath, WM_GETFONT, 0, 0));
    if (!font) font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    int lineH = ScaleOf(st, 16);
    HDC hdc = ::GetDC(hPath);
    TEXTMETRICW tm = {};
    if (hdc) {
      HGDIOBJ old = ::SelectObject(hdc, font);
      if (::GetTextMetricsW(hdc, &tm)) lineH = tm.tmHeight + tm.tmExternalLeading;
      ::SelectObject(hdc, old);
      ::ReleaseDC(hPath, hdc);
    }
    const int lines = static_cast<int>(::SendMessageW(hPath, EM_GETLINECOUNT, 0, 0));
    const int visible = lines < 1 ? 1 : (lines > 3 ? 3 : lines);
    const int editH = visible * lineH + ScaleOf(st, 8);
    pathH = editH;
    ::SetWindowPos(hPath, nullptr, 0, 0, ScaleOf(st, 480), editH,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (lines > visible) {
      ::SetWindowLongPtrW(hPath, GWL_STYLE, ::GetWindowLongPtrW(hPath, GWL_STYLE) | WS_VSCROLL);
      ::SetWindowPos(hPath, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
  }
  AddControl(st, 3, L"BUTTON", L"?", BS_PUSHBUTTON | WS_TABSTOP, 504, y + 20, 26, 24,
             kIdConfigHelp);

  // 「打开配置目录」跟着路径走：原来和日志、皮肤并排，离它要打开的东西隔着一整个页面
  const int openConfigY = y + 22 + UnscaleOf(st, pathH) + 8;
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::OpenConfig), BS_PUSHBUTTON | WS_TABSTOP, 16,
             openConfigY, 104, 26, kIdOpenConfig);

  y = openConfigY + 40;
  const int cacheLabelW = UnscaleOf(st, TextWidth(st, loc::T(loc::Str::WebViewCache))) + 10;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::WebViewCache), SS_LEFT, 16, y + 3, cacheLabelW,
             20, 0);
  st->hWebViewDir = AddControl(st, 3, L"EDIT", L"",
                               ES_AUTOHSCROLL | WS_TABSTOP, 16 + cacheLabelW, y,
                               410 - (16 + cacheLabelW), 24, kIdWebViewDir,
                               WS_EX_CLIENTEDGE);
  if (st->hWebViewDir) {
    ::SendMessageW(st->hWebViewDir, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"%LOCALAPPDATA%\\DopusMdViewer"));
  }
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::WebViewBrowse), BS_PUSHBUTTON | WS_TABSTOP, 418,
             y, 70, 24, kIdWebViewBrowse);
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::WebViewOpen), BS_PUSHBUTTON | WS_TABSTOP, 114,
             y + 34, 110, 26, kIdWebViewOpen);
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::WebViewDefault), BS_PUSHBUTTON | WS_TABSTOP, 232,
             y + 34, 110, 26, kIdWebViewDefault);
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::WebViewClear), BS_PUSHBUTTON | WS_TABSTOP, 350,
             y + 34, 100, 26, kIdWebViewClear);
  // 提示行与左侧标签对齐并占满整行：从按钮下面起（x=114）只有 434 单位，
  // 168 dpi 下约 760px，稍长一点的文案就会换行被单行控件裁掉。
  st->hWebViewInfo = AddControl(st, 3, L"STATIC", L"", SS_LEFT, 16, y + 66, 532, 20, 0);

  // 底部按钮：两个重置在左（本页在前、全部在后），右侧是 应用/确定/取消
  AddControl(st, -1, L"BUTTON", loc::T(loc::Str::ResetPage), BS_PUSHBUTTON | WS_TABSTOP, 16, 522,
             88, 28, kIdResetPage);
  AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Restore), BS_PUSHBUTTON | WS_TABSTOP, 112, 522,
             88, 28, kIdRestore);
  st->hApply = AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Apply),
                          BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 322, 522, 70, 28, kIdApply);
  AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Ok), BS_DEFPUSHBUTTON | WS_TABSTOP, 400,
             522, 70, 28, kIdOk);
  AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Cancel), BS_PUSHBUTTON | WS_TABSTOP, 478, 522,
             70, 28, kIdCancel);

  ShowPage(st, 0);
}

// 只重置当前页签对应的设置项。改的是暂存值，点「应用 / 确定」才落盘。
void ResetPageSettings(DialogState* st, int page) {
  // 先把界面上的现值收回 staged：LoadUiFromStaged 会重写**全部**页的控件，
  // 不先收回就会把其它页尚未应用的改动一起回滚掉。
  ReadUiIntoStaged(st);

  const Config def = ConfigDefaults();
  Config& c = st->staged;
  switch (page) {
    case 0:
      c.skin = def.skin;
      c.font.sizePx = def.font.sizePx;
      c.font.lineHeight = def.font.lineHeight;
      c.font.measureEm = def.font.measureEm;
      break;
    case 1:
      c.font.ui = def.font.ui;
      c.font.body = def.font.body;
      c.font.mono = def.font.mono;
      break;
    case 2:
      c.renderMermaid = def.renderMermaid;
      c.renderKatex = def.renderKatex;
      c.renderHighlight = def.renderHighlight;
      c.renderInlineHtml = def.renderInlineHtml;
      c.renderLocalImages = def.renderLocalImages;
      c.renderTableWrap = def.renderTableWrap;
      c.copyCodeBlock = def.copyCodeBlock;
      c.copyTable = def.copyTable;
      c.tableCopyMode = def.tableCopyMode;
      c.showFindButton = def.showFindButton;
      c.showOutlineButton = def.showOutlineButton;
      c.codeLineNumbers = def.codeLineNumbers;
      c.codeWsOn = def.codeWsOn;
      c.codeWsRange = def.codeWsRange;
      c.codeIndentedBlocks = def.codeIndentedBlocks;
      c.netImageMode = def.netImageMode;
      c.allowParentImages = def.allowParentImages;
      c.allowAbsoluteImages = def.allowAbsoluteImages;
      break;
    case 3:
      c.enableLog = def.enableLog;
      c.openLinksInOpusViewer = def.openLinksInOpusViewer;
      c.viewerExts = def.viewerExts;
      c.prewarm = def.prewarm;
      c.idleReleaseMinutes = def.idleReleaseMinutes;
      c.webviewBaseDir = def.webviewBaseDir;
      c.webviewBaseDirUsb = def.webviewBaseDirUsb;
      break;
    default:
      return;
  }
  LoadUiFromStaged(st);
  SetDirty(st, true);
  LogLine(L"SETTINGS: 重置第 %d 页", page + 1);
}

void OnCommand(DialogState* st, int id, int code) {
  switch (id) {
    case kIdTabAppearance:
    case kIdTabFont:
    case kIdTabRender:
    case kIdTabAdvanced:
      ShowPage(st, id - kIdTabAppearance);
      return;

    case kIdRestore:
      // 影响所有页签，问一句再动；默认焦点落在「否」上，回车不会误触。
      if (::MessageBoxW(st->hwnd, loc::T(loc::Str::ResetAllText),
                        loc::T(loc::Str::ResetAllTitle),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
      }
      st->staged = ConfigDefaults();
      LoadUiFromStaged(st);
      SetDirty(st, true);
      return;

    case kIdResetPage:
      ResetPageSettings(st, st->page);
      return;

    case kIdCopyVersion: {
      std::wstring text = L"DopusMdViewer ";
      text += UtilModuleVersion();
      text += L"\r\nWebView2 ";
      text += WebHostRuntimeInfo();
      if (CopyTextToClipboard(st->hwnd, text.c_str()) && st->hCopyVersion) {
        ::SetWindowTextW(st->hCopyVersion, loc::T(loc::Str::Copied));
        ::SetTimer(st->hwnd, kCopyHintTimerId, 1200, nullptr);
      }
      return;
    }

    case kIdPrewarmHelp:
      ShowTextHelp(st->hwnd, loc::T(loc::Str::Prewarm), loc::T(loc::Str::PrewarmHelp), nullptr,
                   loc::T(loc::Str::Copy), st->font);
      return;

    case kIdApply:
      ApplySettings(st);
      return;

    case kIdOk:
      if (ApplySettings(st)) ::DestroyWindow(st->hwnd);
      return;

    case kIdCancel:
      ::DestroyWindow(st->hwnd);
      return;

    case kIdSkin:
      if (code == CBN_SELCHANGE) {
        const int sel = static_cast<int>(::SendMessageW(st->hSkin, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(st->skins.size())) {
          st->staged.skin = st->skins[sel].id;
        }
        SetDirty(st, true);
      }
      return;

    // 这四个编辑框也可能被各自的上下微调改写（不一定发 EN_CHANGE）
    case kIdSize:
    case kIdLh:
    case kIdMeasure:
    case kIdIdle:
      if (code == EN_CHANGE) SetDirty(st, true);
      return;

    case kIdLinkOpenMode:
      if (code == CBN_SELCHANGE) {
        const int sel = static_cast<int>(::SendMessageW(st->hLinkMode, CB_GETCURSEL, 0, 0));
        ::EnableWindow(st->hViewerExts, sel != 1);
        SetDirty(st, true);
      }
      return;

    case kIdViewerExtsBtn:
      if (code == BN_CLICKED) {
        std::wstring exts = st->staged.viewerExts;
        if (EditViewerExts(st->hwnd, &exts, st->font)) {
          st->staged.viewerExts = exts;
          SetDirty(st, true);
        }
      }
      return;

    case kIdPrewarm:
      if (code == BN_CLICKED) {
        EnableIdleRelease(st, ::IsDlgButtonChecked(st->hwnd, kIdPrewarm) == BST_UNCHECKED);
        SetDirty(st, true);
      }
      return;

    case kIdUnlimited:
      if (code == BN_CLICKED) {
        EnableMeasure(st, ::IsDlgButtonChecked(st->hwnd, kIdUnlimited) != BST_CHECKED);
        UpdateMeasureHint(st);
        SetDirty(st, true);
      }
      return;

    case kIdSlotUi:
    case kIdSlotBody:
    case kIdSlotMono:
      if (code == BN_CLICKED) CheckRadioExclusive(st, id, kIdSlotUi, kIdSlotMono);
      st->slot = id - kIdSlotUi;
      RefreshChainList(st);
      return;

    case kIdFamilyAdd: {
      HWND btn = ::GetDlgItem(st->hwnd, kIdFamilyAdd);
      RECT rc = {};
      if (btn) ::GetWindowRect(btn, &rc);
      OpenFontPanel(st, rc);
      return;
    }

    case kIdChainDel:
      RemoveFromChain(st);
      SetDirty(st, true);
      return;

    case kIdChainUp:
      MoveInChain(st, -1);
      SetDirty(st, true);
      return;

    case kIdChainDown:
      MoveInChain(st, 1);
      SetDirty(st, true);
      return;

    // 渲染开关、目录外图片：点一下就算未保存改动
    case kIdRenderMermaid:
    case kIdRenderKatex:
    case kIdRenderHighlight:
    case kIdRenderHtml:
    case kIdRenderLocal:
    case kIdCopyCode:
    case kIdTableWrap:
    case kIdFindBtn:
    case kIdOutlineBtn:
    case kIdLineNumbers:
    case kIdIndentedBlocks:
    case kIdOutsideParent:
    case kIdOutsideAbsolute:
    case kIdEnableLog:
      if (code == BN_CLICKED) SetDirty(st, true);
      return;

    case kIdCodeWsSelected:
    case kIdCodeWsBoundary:
    case kIdCodeWsAll:
      if (code == BN_CLICKED) {
        CheckRadioExclusive(st, id, kIdCodeWsSelected, kIdCodeWsAll);
        SetDirty(st, true);
      }
      return;

    case kIdNetDoc:
    case kIdNetHost:
    case kIdNetOff:
      if (code == BN_CLICKED) {
        CheckRadioExclusive(st, id, kIdNetDoc, kIdNetOff);
        SetDirty(st, true);
      }
      return;

    case kIdCopyTable:
      if (code == BN_CLICKED) {
        EnableCopyFormat(st, ::IsDlgButtonChecked(st->hwnd, kIdCopyTable) == BST_CHECKED);
        SetDirty(st, true);
      }
      return;

    case kIdCodeWsOn:
      if (code == BN_CLICKED) {
        EnableCodeWsRange(st, ::IsDlgButtonChecked(st->hwnd, kIdCodeWsOn) == BST_CHECKED);
        SetDirty(st, true);
      }
      return;

    case kIdCopyFormat:
      if (code == CBN_SELCHANGE) SetDirty(st, true);
      return;

    case kIdOpenLog: {
      std::wstring dir = UtilDataDir();
      if (!dir.empty()) dir = UtilJoin(dir, L"logs");
      OpenFolder(st->hwnd, dir);
      return;
    }
    case kIdOpenConfig:
      OpenFolder(st->hwnd, UtilDataDir());
      return;
    case kIdOpenSkins:
      OpenFolder(st->hwnd, SkinUserDir());
      return;
    case kIdConfigHelp: {
      const std::wstring dir = UtilDataDir();
      wchar_t msg[1024];
      ::StringCchPrintfW(msg, ARRAYSIZE(msg), loc::T(loc::Str::ConfigFolderHelp), kDataAliasName,
                         dir.empty() ? loc::T(loc::Str::NoFolder) : dir.c_str());
      ShowConfigHelp(st->hwnd, msg, st->font);
      return;
    }

    case kIdWebViewDir:
      if (code == EN_CHANGE) {
        SetDirty(st, true);
      } else if (code == EN_KILLFOCUS) {
        RefreshWebViewCacheInfo(st);
      }
      return;
    case kIdWebViewBrowse:
      BrowseWebViewDir(st);
      return;
    case kIdWebViewHelp:
      ShowWebViewHelp(st);
      return;
    case kIdWebViewOpen:
      OpenWebViewDir(st);
      return;
    case kIdWebViewDefault:
      ::SetWindowTextW(st->hWebViewDir, L"");
      SetDirty(st, true);
      RefreshWebViewCacheInfo(st);
      return;
    case kIdWebViewClear:
      ClearWebViewCache(st);
      return;

    default:
      return;
  }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  DialogState* st = reinterpret_cast<DialogState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (uMsg) {
    case WM_NCCREATE: {
      const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
      st = static_cast<DialogState*>(cs->lpCreateParams);
      if (st) {
        st->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      }
      // 标题是 DefWindowProc 在 WM_NCCREATE 里存下的：不转给它，CreateWindowExW 的
      // lpszName 就丢了（窗口标题恒为空）。
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    case WM_CREATE:
      if (!st) return -1;
      BuildUi(st);
      LoadFamilies(st);
      LoadUiFromStaged(st);
      SetDirty(st, false);
      return 0;

    case WM_COMMAND:
      if (st) OnCommand(st, LOWORD(wParam), HIWORD(wParam));
      return 0;

    case WM_KEYDOWN:
      // 焦点落在窗口本身（还没 Tab 进任何控件）时，键消息只会到这里
      if (st && HandleDialogKey(st, hwnd, wParam)) return 0;
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    case WM_NOTIFY: {
      const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lParam);
      if (st && hdr && hdr->code == UDN_DELTAPOS) {
        if (hdr->idFrom == kIdLhSpin) {
          const NMUPDOWN* ud = reinterpret_cast<const NMUPDOWN*>(lParam);
          const double cur = ReadLhText(st, st->staged.font.lineHeight);
          int hundredths = static_cast<int>(cur * 100.0 + 0.5) + (ud->iDelta > 0 ? 5 : -5);
          if (hundredths < 120) hundredths = 120;
          if (hundredths > 220) hundredths = 220;
          st->staged.font.lineHeight = hundredths / 100.0;
          SetLhText(st, st->staged.font.lineHeight);
          ::SendMessageW(st->hLhSpin, UDM_SETPOS32, 0, hundredths);
          SetDirty(st, true);
          return 1;  // 位置已自己维护
        }
        if (hdr->idFrom == kIdSizeSpin || hdr->idFrom == kIdMeasureSpin ||
            hdr->idFrom == kIdIdleSpin) {
          SetDirty(st, true);
        }
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    case WM_ERASEBKGND: {
      // 显式刷白：不依赖类背景刷子，静态文本再返回同一个刷子就不会出现灰底板。
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      ::FillRect(reinterpret_cast<HDC>(wParam), &rc, ::GetSysColorBrush(COLOR_WINDOW));
      return 1;
    }

    case WM_TIMER:
      if (wParam == kCopyHintTimerId) {
        ::KillTimer(hwnd, kCopyHintTimerId);
        if (st && st->hCopyVersion) {
          ::SetWindowTextW(st->hCopyVersion, loc::T(loc::Str::CopyVersionInfo));
        }
        return 0;
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
      if (st && dis && dis->CtlID >= kIdTabAppearance && dis->CtlID <= kIdTabAdvanced) {
        DrawTab(st, dis);
        return TRUE;
      }
      if (st && dis && dis->CtlID == static_cast<UINT>(kIdFamilyList)) {
        DrawChainItem(st, dis);
        return TRUE;
      }
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    case WM_CTLCOLORSTATIC: {
      HDC hdc = reinterpret_cast<HDC>(wParam);
      HWND control = reinterpret_cast<HWND>(lParam);
      ::SetBkMode(hdc, TRANSPARENT);
      if (st && control == st->hPreviewNote && st->previewMissing) {
        ::SetTextColor(hdc, RGB(192, 0, 0));
      }
      return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
    }

    // 拖到另一块缩放不同的显示器：按新 DPI 重排，并把整窗压回工作区内。
    case WM_DPICHANGED:
      if (st) {
        const UINT newDpi = HIWORD(wParam) ? HIWORD(wParam) : LOWORD(wParam);
        const RECT work = WorkAreaFor(hwnd);
        RelayoutForDpi(st, FitDpiToWorkArea(newDpi, work));

        int w = 0;
        int h = 0;
        WindowSizeForDpi(st->dpi, &w, &h);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        int x = suggested ? suggested->left : work.left;
        int y = suggested ? suggested->top : work.top;
        if (x + w > work.right) x = work.right - w;
        if (y + h > work.bottom) y = work.bottom - h;
        if (x < work.left) x = work.left;
        if (y < work.top) y = work.top;
        ::SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      g_hwnd = nullptr;
      if (st) {
        if (st->preview) SettingsPreviewDestroy(st->preview);
        if (st->fontBold) ::DeleteObject(st->fontBold);
        if (st->font) ::DeleteObject(st->font);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete st;
      }
      return 0;

    case WM_NCDESTROY:
      // Opus 的 DVP_Configure 宿主靠这个 WM_QUIT 结束配置流程；
      // SDK 的 RAR / CSV 示例都在 WM_NCDESTROY 里发。
      LogLine(L"SETTINGS: 窗口已销毁，向 Opus 配置循环发 WM_QUIT");
      ::PostQuitMessage(0);
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);

    default:
      return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

bool RegisterWindowClass(void) {
  static bool registered = false;
  if (registered) return true;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SettingsProc;
  wc.hInstance = ModuleHandle();
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClass;

  if (::RegisterClassExW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    registered = true;
    return true;
  }
  LogLine(L"SETTINGS: RegisterClassExW 失败 err=%lu", ::GetLastError());
  return false;
}

// 不取引用计数（UNCHANGED_REFCOUNT）：这个函数每建一次窗口都会调，带引用计数
// 就会一轮轮加，DLL 永远卸不掉。
HMODULE ModuleHandle(void) {
  HMODULE mod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&SettingsProc), &mod)) {
    mod = ::GetModuleHandleW(nullptr);
  }
  return mod;
}

HFONT CreateUiFontForDpi(UINT dpi, bool bold = false) {
  NONCLIENTMETRICSW ncm = {};
  ncm.cbSize = sizeof(ncm);
  // SystemParametersInfoForDpi 才给「目标显示器」的度量；老的 SPI 只给主屏的，
  // 把窗口拖到另一块缩放不同的屏上时字会偏小。
  BOOL ok = FALSE;
  if (const auto spiForDpi = reinterpret_cast<BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT)>(
          ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"))) {
    ok = spiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi ? dpi : 96);
  }
  if (!ok) ok = ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
  if (ok) {
    if (bold) ncm.lfMessageFont.lfWeight = FW_BOLD;
    return ::CreateFontIndirectW(&ncm.lfMessageFont);
  }
  if (bold) {
    LOGFONTW lf = {};
    HFONT base = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    if (base && ::GetObjectW(base, sizeof(lf), &lf) == sizeof(lf)) {
      lf.lfWeight = FW_BOLD;
      return ::CreateFontIndirectW(&lf);
    }
  }
  return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

// 按 dpi 算整窗尺寸（含标题栏边框）。
void WindowSizeForDpi(UINT dpi, int* w, int* h) {
  RECT rc = {0, 0, ScaleByDpi(dpi, kClientW), ScaleByDpi(dpi, kClientH)};
  ::AdjustWindowRectEx(&rc, kDialogStyle, FALSE, 0);
  *w = rc.right - rc.left;
  *h = rc.bottom - rc.top;
}

// 窗口所在显示器的工作区（去掉任务栏）。
RECT WorkAreaFor(HWND wnd) {
  const HMONITOR mon = ::MonitorFromWindow(wnd ? wnd : ::GetForegroundWindow(),
                                            MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  if (::GetMonitorInfoW(mon, &mi)) return mi.rcWork;
  RECT fallback = {0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
  return fallback;
}

// 窗口放不进工作区就把有效 DPI 往下压（最低 96 = 100%）：宁可字小一点，
// 也不能让底部的「确定/取消」跑到屏幕外——那等于设置窗口没法用。
UINT FitDpiToWorkArea(UINT dpi, const RECT& work) {
  const int aw = work.right - work.left;
  const int ah = work.bottom - work.top;
  UINT d = dpi ? dpi : 96;
  for (;;) {
    int w = 0;
    int h = 0;
    WindowSizeForDpi(d, &w, &h);
    if ((w <= aw && h <= ah) || d <= 96) return d;
    const UINT next = static_cast<UINT>(d * 9 / 10);
    d = next < 96 ? 96 : next;
  }
}

// DPI 变了：按新比例重排所有控件、换字体。坐标用的是创建时记下的未缩放值，
// 反复拖屏也不会累积误差。
void RelayoutForDpi(DialogState* st, UINT dpi) {
  if (!st || !st->hwnd || !dpi || dpi == st->dpi) return;

  st->dpi = dpi;
  HFONT oldFont = st->font;
  HFONT oldBold = st->fontBold;
  st->font = CreateUiFontForDpi(dpi, false);
  st->fontBold = CreateUiFontForDpi(dpi, true);

  for (const DialogState::CtrlGeom& g : st->layout) {
    if (!g.hwnd || !::IsWindow(g.hwnd)) continue;
    if (st->preview && g.hwnd == st->hPreview) continue;  // 预览单独走 SettingsPreviewResize
    ::SetWindowPos(g.hwnd, nullptr, ScaleOf(st, g.x), ScaleOf(st, g.y), ScaleOf(st, g.cx),
                   ScaleOf(st, g.cy), SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyFont(g.hwnd, st->font);
  }
  if (st->preview) {
    RECT prc = {ScaleOf(st, kPreviewX), ScaleOf(st, kPreviewY), ScaleOf(st, kPreviewX + kPreviewW),
                ScaleOf(st, kPreviewY + kPreviewH)};
    SettingsPreviewResize(st->preview, prc);
  }
  if (st->hChainList) ::SendMessageW(st->hChainList, LB_SETITEMHEIGHT, 0, ScaleOf(st, 22));

  if (oldFont) ::DeleteObject(oldFont);
  if (oldBold) ::DeleteObject(oldBold);
  ::InvalidateRect(st->hwnd, nullptr, TRUE);
}

}  // namespace

HWND SettingsShow(HWND parent) {
  if (g_hwnd && ::IsWindow(g_hwnd)) {
    ::SetForegroundWindow(g_hwnd);
    return g_hwnd;
  }
  if (!RegisterWindowClass()) return nullptr;

  INITCOMMONCONTROLSEX icc = {};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_WIN95_CLASSES;
  ::InitCommonControlsEx(&icc);

  DialogState* st = new (std::nothrow) DialogState();
  if (!st) return nullptr;
  st->staged = ConfigSnapshot();

  // 先定 DPI 再建字体：字体要按最终使用的 DPI 取度量。窗口放不进工作区时
  // FitDpiToWorkArea 会把有效 DPI 压下来，字号跟着变小，整窗仍然完整可见。
  const UINT wantDpi = parent ? ::GetDpiForWindow(parent) : 96;
  const RECT work = WorkAreaFor(parent);
  st->dpi = FitDpiToWorkArea(wantDpi ? wantDpi : 96, work);
  st->font = CreateUiFontForDpi(st->dpi, false);
  st->fontBold = CreateUiFontForDpi(st->dpi, true);

  int w = 0;
  int h = 0;
  WindowSizeForDpi(st->dpi, &w, &h);

  // 居中到工作区（父窗口可能压在屏幕边缘，按父窗口居中会算出屏幕外的坐标）
  int x = work.left + ((work.right - work.left) - w) / 2;
  int y = work.top + ((work.bottom - work.top) - h) / 2;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;

  HWND hwnd = ::CreateWindowExW(0, kClass, loc::T(loc::Str::SettingsTitle), kDialogStyle, x, y, w,
                                h, parent, nullptr, ModuleHandle(), st);
  if (!hwnd) {
    if (st->font) ::DeleteObject(st->font);
    if (st->fontBold) ::DeleteObject(st->fontBold);
    delete st;
    LogLine(L"SETTINGS: 创建窗口失败 err=%lu", ::GetLastError());
    return nullptr;
  }

  g_hwnd = hwnd;
  ::ShowWindow(hwnd, SW_SHOW);
  ::SetForegroundWindow(hwnd);
  FocusFirst(st);
  LogLine(L"SETTINGS: 已打开设置窗口 %p（%u dpi）", hwnd, st->dpi);
  return hwnd;
}
