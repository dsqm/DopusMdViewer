// settings_dlg.cpp - 非模态设置窗口（外观 / 字体 / 渲染 / 高级四页）。
//
// 四页共用一个窗口，靠控件的 ShowWindow 切换，不建子对话框；改动只进 staged
// 副本，点“应用/确定”才写 config.json 并通知查看器。

#include "settings_dlg.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "config.h"
#include "localize.h"
#include "log.h"
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
  kIdSlotUi = 3200,
  kIdSlotBody,
  kIdSlotMono,
  kIdFamily,
  kIdFamilyList,
  kIdFamilyAdd,
  kIdChainUp,
  kIdChainDown,
  kIdChainDel,
  kIdPreview,
  kIdPreviewNote,
  kIdRefreshFonts,
  kIdRenderMermaid = 3300,
  kIdRenderKatex,
  kIdRenderHighlight,
  kIdRenderHtml,
  kIdRenderLocal,
  kIdNetDoc,
  kIdNetHost,
  kIdNetOff,
  kIdOutside,
  kIdVersion = 3400,
  kIdWebView,
  kIdGiant,
  kIdResident,
  kIdOpenLog,
  kIdOpenConfig,
  kIdOpenSkins,
  kIdPrewarm,
  kIdIdle,
  kIdIdleSpin,
  kIdConfigPath = 3410,
  kIdConfigHelp,
  kIdRestore = 3500,
  kIdApply,
  kIdOk,
  kIdCancel,
};

constexpr int kPages = 4;

struct DialogState {
  HWND hwnd = nullptr;
  HFONT font = nullptr;
  HFONT previewFont = nullptr;
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

  HWND hSlot[3] = {};
  HWND hFamily = nullptr;
  HWND hChainList = nullptr;
  HWND hPreview = nullptr;
  HWND hPreviewNote = nullptr;

  HWND hRender[5] = {};
  HWND hNetDoc = nullptr;
  HWND hNetHost = nullptr;
  HWND hNetOff = nullptr;
  HWND hOutside = nullptr;
  HWND hPrewarm = nullptr;
  HWND hIdle = nullptr;
  HWND hIdleSpin = nullptr;
  HWND hApply = nullptr;
  bool dirty = false;
};

HWND g_hwnd = nullptr;

HMODULE ModuleHandle(void);

int ScaleByDpi(UINT dpi, int value) {
  return ::MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

int ScaleOf(const DialogState* st, int value) {
  return ScaleByDpi(st->dpi, value);
}

void ApplyFont(HWND hwnd, HFONT font) {
  if (hwnd && font) ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

/* ---------------- 配置文件夹说明 ----------------
   用 DialogBoxParam：可选中、Ctrl+C、右键复制都可用，另给「复制别名」一键复制。
   不自己跑嵌套消息循环，避免和外层配置窗口的消息循环打架。 */

struct HelpDialogData {
  const wchar_t* text = nullptr;
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
      ::SetWindowTextW(dlg, loc::T(loc::Str::ConfigFolder));
      ::SetDlgItemTextW(dlg, IDC_HELP_TEXT, data && data->text ? data->text : L"");
      ::SetDlgItemTextW(dlg, IDC_HELP_COPY, loc::T(loc::Str::CopyAlias));
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
        if (CopyTextToClipboard(dlg, kDataAliasName)) {
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

void ShowConfigHelp(HWND owner, const wchar_t* text, HFONT font) {
  HelpDialogData data;
  data.text = text;
  data.font = font;
  if (::DialogBoxParamW(ModuleHandle(), MAKEINTRESOURCEW(IDD_CONFIG_HELP), owner, HelpDlgProc,
                        reinterpret_cast<LPARAM>(&data)) == -1) {
    ::MessageBoxW(owner, text, loc::T(loc::Str::ConfigFolder), MB_OK | MB_ICONINFORMATION);
  }
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
bool HandleDialogKey(DialogState* st, HWND focus, WPARAM vk);

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
  HWND tab = nullptr;
  for (int i = 0; i < kPages; ++i) {
    HWND t = ::GetDlgItem(st->hwnd, kIdTabAppearance + i);
    if (t && ::IsWindowVisible(t) && ::IsWindowEnabled(t) &&
        ::SendMessageW(t, BM_GETCHECK, 0, 0) == BST_CHECKED) {
      tab = t;
      break;
    }
  }
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
  if (h) SubclassControl(st, h);
  return h;
}

void ShowPage(DialogState* st, int page) {
  if (page < 0 || page >= kPages) return;
  st->page = page;
  for (int p = 0; p < kPages; ++p) {
    for (HWND h : st->pageCtrls[p]) ::ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
  }
  ::CheckRadioButton(st->hwnd, kIdTabAppearance, kIdTabAdvanced, kIdTabAppearance + page);
  LogLine(L"SETTINGS: 切到第 %d 页", page + 1);
  // 切页后焦点若落在刚藏起来的控件上，挪到本页第一个控件
  HWND focus = ::GetFocus();
  if (!focus || !::IsWindowVisible(focus)) FocusPageFirst(st);
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

void UpdatePreview(DialogState* st) {
  if (!st->hPreview) return;
  const FontStack* stack = CurrentStack(st);
  bool inherited = false;
  if (stack->empty() && st->slot == 1) {
    stack = &st->staged.font.ui;
    inherited = true;
  }
  std::wstring face = stack->empty() ? std::wstring(L"Segoe UI") : stack->front();

  LOGFONTW lf = {};
  lf.lfHeight = -::MulDiv(15, static_cast<int>(st->dpi ? st->dpi : 96), 96);
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  ::StringCchCopyW(lf.lfFaceName, LF_FACESIZE, face.c_str());

  HFONT created = ::CreateFontIndirectW(&lf);
  st->previewMissing = false;
  if (created) {
    LOGFONTW got = {};
    if (::GetObjectW(created, sizeof(got), &got) == sizeof(got)) {
      st->previewMissing = !SameFamily(got.lfFaceName, face);
    }
    if (st->previewFont) ::DeleteObject(st->previewFont);
    st->previewFont = created;
    ApplyFont(st->hPreview, created);
  }

  ::SetWindowTextW(st->hPreview, loc::T(loc::Str::PreviewSample));
  if (st->previewMissing) {
    wchar_t note[LF_FACESIZE * 2 + 64] = {};
    ::StringCchPrintfW(note, ARRAYSIZE(note), loc::T(loc::Str::PreviewMissing), face.c_str());
    ::SetWindowTextW(st->hPreviewNote, note);
  } else if (inherited && st->staged.font.body.empty()) {
    ::SetWindowTextW(st->hPreviewNote, loc::T(loc::Str::PreviewInherited));
  } else {
    ::SetWindowTextW(st->hPreviewNote, L"");
  }
  ::InvalidateRect(st->hPreview, nullptr, TRUE);
  ::InvalidateRect(st->hPreviewNote, nullptr, TRUE);
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
    selected = 0;
    st->staged.skin = st->skins[0].id;
  }
  ::SendMessageW(st->hSkin, CB_SETCURSEL, selected, 0);
}

void EnableMeasure(DialogState* st, bool enabled) {
  ::EnableWindow(st->hMeasure, enabled);
  ::EnableWindow(st->hMeasureSpin, enabled);
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

  const int netId =
      st->staged.netImageMode == 0 ? kIdNetOff : (st->staged.netImageMode == 2 ? kIdNetDoc : kIdNetHost);
  ::CheckRadioButton(st->hwnd, kIdNetDoc, kIdNetOff, netId);
  ::CheckDlgButton(st->hwnd, kIdOutside,
                   st->staged.allowOutsideImages ? BST_CHECKED : BST_UNCHECKED);
  ::CheckDlgButton(st->hwnd, kIdPrewarm, st->staged.prewarm ? BST_CHECKED : BST_UNCHECKED);
  ::StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d", st->staged.idleReleaseMinutes);
  ::SetWindowTextW(st->hIdle, buf);
  ::SendMessageW(st->hIdleSpin, UDM_SETPOS32, 0, st->staged.idleReleaseMinutes);
  EnableIdleRelease(st, !st->staged.prewarm);

  ::CheckRadioButton(st->hwnd, kIdSlotUi, kIdSlotMono, kIdSlotUi + st->slot);
  RefreshChainList(st);
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
    if (ok && measure >= 8 && measure <= 80) st->staged.font.measureEm = static_cast<int>(measure);
  }

  st->staged.renderMermaid = ::IsDlgButtonChecked(st->hwnd, kIdRenderMermaid) == BST_CHECKED;
  st->staged.renderKatex = ::IsDlgButtonChecked(st->hwnd, kIdRenderKatex) == BST_CHECKED;
  st->staged.renderHighlight = ::IsDlgButtonChecked(st->hwnd, kIdRenderHighlight) == BST_CHECKED;
  st->staged.renderInlineHtml = ::IsDlgButtonChecked(st->hwnd, kIdRenderHtml) == BST_CHECKED;
  st->staged.renderLocalImages = ::IsDlgButtonChecked(st->hwnd, kIdRenderLocal) == BST_CHECKED;

  if (::IsDlgButtonChecked(st->hwnd, kIdNetDoc) == BST_CHECKED) {
    st->staged.netImageMode = 2;
  } else if (::IsDlgButtonChecked(st->hwnd, kIdNetOff) == BST_CHECKED) {
    st->staged.netImageMode = 0;
  } else {
    st->staged.netImageMode = 1;
  }
  st->staged.allowOutsideImages = ::IsDlgButtonChecked(st->hwnd, kIdOutside) == BST_CHECKED;
  st->staged.prewarm = ::IsDlgButtonChecked(st->hwnd, kIdPrewarm) == BST_CHECKED;
  const UINT idle = ::GetDlgItemInt(st->hwnd, kIdIdle, &ok, FALSE);
  if (ok && idle <= 1440) st->staged.idleReleaseMinutes = static_cast<int>(idle);
}

bool ApplySettings(DialogState* st) {
  ReadUiIntoStaged(st);
  if (!ConfigSave(st->staged)) {
    ::MessageBoxW(st->hwnd, loc::T(loc::Str::SaveFailed), L"DopusMdViewer",
                  MB_OK | MB_ICONERROR);
    return false;
  }
  ViewerNotifyConfigChanged();
  WebHostPrewarm(st->staged.prewarm, st->staged.idleReleaseMinutes);
  SetDirty(st, false);
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

void AddFontToChain(DialogState* st) {
  wchar_t buf[LF_FACESIZE * 2] = {};
  ::GetWindowTextW(st->hFamily, buf, ARRAYSIZE(buf));
  const std::wstring name = Trimmed(buf);
  if (name.empty()) return;
  FontStack* stack = CurrentStack(st);
  for (const std::wstring& existing : *stack) {
    if (SameFamily(existing, name)) return;
  }
  stack->push_back(name);
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
    AddControl(st, -1, L"BUTTON", tabs[i].text,
               BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0),
               12 + i * 90, 10, 84, 26, tabs[i].id);
  }

  // ---------------- 外观 ----------------
  int y = 54;
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::Skin), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hSkin = AddControl(st, 0, L"COMBOBOX", L"",
                         CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 100, y,
                         430, 200, kIdSkin);

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
  ::SendMessageW(st->hMeasureSpin, UDM_SETRANGE32, 8, 80);
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::MeasureUnit), SS_LEFT, 180, y + 3, 120, 20, 0);
  st->hUnlimited = AddControl(st, 0, L"BUTTON", loc::T(loc::Str::Unlimited),
                              BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 300, y, 120, 24,
                              kIdUnlimited);
  AddControl(st, 0, L"STATIC", loc::T(loc::Str::MeasureHint), SS_LEFT, 16, 210, 520, 20, 0);

  // ---------------- 字体 ----------------
  y = 48;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Slot), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hSlot[0] = AddControl(st, 1, L"BUTTON", loc::T(loc::Str::SlotUi),
                            BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 100, y, 70, 24, kIdSlotUi);
  st->hSlot[1] = AddControl(st, 1, L"BUTTON", loc::T(loc::Str::SlotBody),
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 176, y, 70, 24, kIdSlotBody);
  st->hSlot[2] = AddControl(st, 1, L"BUTTON", loc::T(loc::Str::SlotMono),
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 252, y, 70, 24, kIdSlotMono);

  y = 82;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Family), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hFamily = AddControl(st, 1, L"COMBOBOX", L"",
                           CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_HASSTRINGS | WS_VSCROLL |
                               WS_TABSTOP,
                           100, y, 330, 240, kIdFamily);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::FamilyAdd), BS_PUSHBUTTON | WS_TABSTOP, 436, y,
             60, 24, kIdFamilyAdd);

  y = 118;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Chain), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hChainList = AddControl(st, 1, L"LISTBOX", L"",
                              LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER |
                                  WS_TABSTOP,
                              100, y, 330, 116, kIdFamilyList);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::ChainUp), BS_PUSHBUTTON | WS_TABSTOP, 436, y, 60,
             24, kIdChainUp);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::ChainDown), BS_PUSHBUTTON | WS_TABSTOP, 436,
             y + 30, 60, 24, kIdChainDown);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::ChainDel), BS_PUSHBUTTON | WS_TABSTOP, 436,
             y + 60, 60, 24, kIdChainDel);

  y = 246;
  AddControl(st, 1, L"STATIC", loc::T(loc::Str::Preview), SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hPreview = AddControl(st, 1, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 100, y, 430, 26, kIdPreview);
  st->hPreviewNote = AddControl(st, 1, L"STATIC", L"", SS_LEFT, 100, y + 30, 430, 20,
                                kIdPreviewNote);
  AddControl(st, 1, L"BUTTON", loc::T(loc::Str::RefreshFonts), BS_PUSHBUTTON | WS_TABSTOP, 100,
             y + 58, 110, 24, kIdRefreshFonts);

  // ---------------- 渲染 ----------------
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
  for (int i = 0; i < 5; ++i) {
    st->hRender[i] = AddControl(st, 2, L"BUTTON", renders[i].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                                16, y + i * 26, 300, 24, renders[i].id);
  }

  y = 190;
  AddControl(st, 2, L"STATIC", loc::T(loc::Str::NetAskWhen), SS_LEFT, 16, y, 300, 20, 0);
  st->hNetDoc = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::NetPerDoc),
                           BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 28, y + 24, 300, 24,
                           kIdNetDoc);
  st->hNetHost = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::NetPerHost),
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 28, y + 50, 360, 24, kIdNetHost);
  st->hNetOff = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::NetNever),
                           BS_AUTORADIOBUTTON | WS_TABSTOP, 28, y + 76, 300, 24, kIdNetOff);
  st->hOutside = AddControl(st, 2, L"BUTTON", loc::T(loc::Str::OutsideImages),
                            BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 16, y + 108, 480, 24,
                            kIdOutside);

  // ---------------- 高级 ----------------
  std::wstring version = UtilModuleVersion();
  if (version.empty()) version = loc::T(loc::Str::Unknown);
  std::wstring runtime = WebHostRuntimeInfo();
  if (runtime.empty()) runtime = loc::T(loc::Str::RuntimeMissing);

  y = 50;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::Version), SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", version.c_str(), SS_LEFT, 110, y + 2, 300, 20, kIdVersion);
  y = 78;
  AddControl(st, 3, L"STATIC", L"WebView2", SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", runtime.c_str(), SS_LEFT, 110, y + 2, 420, 20, kIdWebView);
  y = 108;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::GiantBlock), SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::GiantBlockNote), SS_LEFT, 110, y + 2, 420, 20,
             kIdGiant);
  y = 136;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::ResidentWindow), SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::ResidentNote), SS_LEFT, 110, y + 2, 420, 20,
             kIdResident);

  y = 166;
  st->hPrewarm = AddControl(st, 3, L"BUTTON", loc::T(loc::Str::Prewarm),
                            BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 480, 24, kIdPrewarm);

  y = 196;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::IdleRelease), SS_LEFT, 16, y + 3, 120, 20, 0);
  st->hIdle = AddControl(st, 3, L"EDIT", L"",
                         ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, 140, y, 70, 24, kIdIdle,
                         WS_EX_CLIENTEDGE);
  st->hIdleSpin = AddControl(st, 3, UPDOWN_CLASSW, L"",
                             UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
                             0, 0, 0, 0, kIdIdleSpin);
  ::SendMessageW(st->hIdleSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hIdle), 0);
  ::SendMessageW(st->hIdleSpin, UDM_SETRANGE32, 0, 1440);
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::IdleUnit), SS_LEFT, 220, y + 3, 220, 20, 0);

  y = 234;
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::OpenLog), BS_PUSHBUTTON | WS_TABSTOP, 16, y, 130,
             26, kIdOpenLog);
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::OpenConfig), BS_PUSHBUTTON | WS_TABSTOP, 154, y,
             140, 26, kIdOpenConfig);
  AddControl(st, 3, L"BUTTON", loc::T(loc::Str::OpenSkins), BS_PUSHBUTTON | WS_TABSTOP, 302, y,
             130, 26, kIdOpenSkins);

  y = 266;
  AddControl(st, 3, L"STATIC", loc::T(loc::Str::ConfigFolder), SS_LEFT, 16, y, 200, 20, 0);
  std::wstring dataDir = UtilDataDir();
  if (dataDir.empty()) dataDir = loc::T(loc::Str::NoFolder);
  HWND hPath = AddControl(st, 3, L"EDIT", dataDir.c_str(),
                          WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 16, y + 22,
                          480, 40, kIdConfigPath, WS_EX_CLIENTEDGE);
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

  // 底部按钮
  AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Restore), BS_PUSHBUTTON | WS_TABSTOP, 16, 392,
             100, 28, kIdRestore);
  st->hApply = AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Apply),
                          BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 322, 392, 70, 28, kIdApply);
  AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Ok), BS_DEFPUSHBUTTON | WS_TABSTOP, 400,
             392, 70, 28, kIdOk);
  AddControl(st, -1, L"BUTTON", loc::T(loc::Str::Cancel), BS_PUSHBUTTON | WS_TABSTOP, 478, 392,
             70, 28, kIdCancel);

  ShowPage(st, 0);
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
      st->staged = ConfigDefaults();
      LoadUiFromStaged(st);
      SetDirty(st, true);
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

    // 这三个编辑框也可能被上面三个上下微调改写（不一定发 EN_CHANGE）
    case kIdSize:
    case kIdLh:
    case kIdMeasure:
    case kIdIdle:
      if (code == EN_CHANGE) SetDirty(st, true);
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
        SetDirty(st, true);
      }
      return;

    case kIdSlotUi:
    case kIdSlotBody:
    case kIdSlotMono:
      st->slot = id - kIdSlotUi;
      RefreshChainList(st);
      return;

    case kIdFamilyAdd:
      AddFontToChain(st);
      SetDirty(st, true);
      return;

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

    case kIdRefreshFonts:
      LoadFamilies(st);
      return;

    // 渲染开关、网络图档位、目录外图片：点一下就算未保存改动
    case kIdRenderMermaid:
    case kIdRenderKatex:
    case kIdRenderHighlight:
    case kIdRenderHtml:
    case kIdRenderLocal:
    case kIdNetDoc:
    case kIdNetHost:
    case kIdNetOff:
    case kIdOutside:
      if (code == BN_CLICKED) SetDirty(st, true);
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

    case WM_CTLCOLORSTATIC: {
      HDC hdc = reinterpret_cast<HDC>(wParam);
      HWND control = reinterpret_cast<HWND>(lParam);
      ::SetBkMode(hdc, TRANSPARENT);
      if (st && control == st->hPreviewNote && st->previewMissing) {
        ::SetTextColor(hdc, RGB(192, 0, 0));
      }
      return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      g_hwnd = nullptr;
      if (st) {
        if (st->previewFont) ::DeleteObject(st->previewFont);
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

HMODULE ModuleHandle(void) {
  HMODULE mod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&SettingsProc), &mod)) {
    mod = ::GetModuleHandleW(nullptr);
  }
  return mod;
}

HFONT CreateUiFont(void) {
  NONCLIENTMETRICSW ncm = {};
  ncm.cbSize = sizeof(ncm);
  if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    return ::CreateFontIndirectW(&ncm.lfMessageFont);
  }
  return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
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
  st->font = CreateUiFont();
  st->staged = ConfigSnapshot();

  const UINT dpi = parent ? ::GetDpiForWindow(parent) : 96;
  st->dpi = dpi ? dpi : 96;
  RECT rc = {0, 0, ScaleOf(st, 560), ScaleOf(st, 430)};
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
  ::AdjustWindowRectEx(&rc, style, FALSE, 0);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;

  int x = 200;
  int y = 200;
  if (parent) {
    RECT pr;
    if (::GetWindowRect(parent, &pr)) {
      x = pr.left + ((pr.right - pr.left) - w) / 2;
      y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    }
  }

  HWND hwnd = ::CreateWindowExW(0, kClass, loc::T(loc::Str::SettingsTitle), style, x, y, w, h,
                                parent, nullptr, ModuleHandle(), st);
  if (!hwnd) {
    if (st->font) ::DeleteObject(st->font);
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
