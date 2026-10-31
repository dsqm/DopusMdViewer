// settings_dlg.cpp - 非模态设置窗口（票 09：外观 / 字体 / 渲染 / 高级四页）。
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
#include "log.h"
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
  HWND hApply = nullptr;
  bool dirty = false;
};

HWND g_hwnd = nullptr;

HMODULE ModuleHandle(void);

int ScaleOf(const DialogState* st, int value) {
  return ::MulDiv(value, static_cast<int>(st->dpi ? st->dpi : 96), 96);
}

void ApplyFont(HWND hwnd, HFONT font) {
  if (hwnd && font) ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
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
  return h;
}

void ShowPage(DialogState* st, int page) {
  if (page < 0 || page >= kPages) return;
  st->page = page;
  for (int p = 0; p < kPages; ++p) {
    for (HWND h : st->pageCtrls[p]) ::ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
  }
  ::CheckRadioButton(st->hwnd, kIdTabAppearance, kIdTabAdvanced, kIdTabAppearance + page);
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

  ::SetWindowTextW(st->hPreview, L"中文 English 123 — 字体预览");
  std::wstring note;
  if (st->previewMissing) {
    note = L"未找到：" + face + L"（仍可保存，系统会回退）";
  } else if (inherited && st->staged.font.body.empty()) {
    note = L"正文未设置，跟随界面字体";
  }
  ::SetWindowTextW(st->hPreviewNote, note.c_str());
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
}

bool ApplySettings(DialogState* st) {
  ReadUiIntoStaged(st);
  if (!ConfigSave(st->staged)) {
    ::MessageBoxW(st->hwnd, L"写入 config.json 失败，详见日志。", L"DopusMdViewer",
                  MB_OK | MB_ICONERROR);
    return false;
  }
  ViewerNotifyConfigChanged();
  SetDirty(st, false);
  LogLine(L"SETTINGS: 已应用 skin=\"%s\" size=%d lh=%.2f measure=%d net=%d", st->staged.skin.c_str(),
          st->staged.font.sizePx, st->staged.font.lineHeight, st->staged.font.measureEm,
          st->staged.netImageMode);
  return true;
}

void OpenFolder(HWND owner, const std::wstring& dir) {
  if (dir.empty()) {
    ::MessageBoxW(owner, L"目录路径不可用。", L"DopusMdViewer", MB_OK | MB_ICONERROR);
    return;
  }
  UtilEnsureDir(dir);
  const HINSTANCE r = ::ShellExecuteW(owner, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    ::MessageBoxW(owner, L"打开目录失败。", L"DopusMdViewer", MB_OK | MB_ICONERROR);
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
  const TabDef tabs[] = {{kIdTabAppearance, L"外观"},
                         {kIdTabFont, L"字体"},
                         {kIdTabRender, L"渲染"},
                         {kIdTabAdvanced, L"高级"}};
  for (int i = 0; i < 4; ++i) {
    AddControl(st, -1, L"BUTTON", tabs[i].text,
               BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_TABSTOP | (i == 0 ? WS_GROUP : 0),
               12 + i * 90, 10, 84, 26, tabs[i].id);
  }

  // ---------------- 外观 ----------------
  int y = 54;
  AddControl(st, 0, L"STATIC", L"皮肤", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hSkin = AddControl(st, 0, L"COMBOBOX", L"",
                         CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 100, y,
                         430, 200, kIdSkin);

  y = 92;
  AddControl(st, 0, L"STATIC", L"字号", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hSize = AddControl(st, 0, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 100, y, 70, 24,
                         kIdSize, WS_EX_CLIENTEDGE);
  st->hSizeSpin = AddControl(st, 0, UPDOWN_CLASSW, L"",
                             UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
                             0, 0, 0, 0, kIdSizeSpin);
  ::SendMessageW(st->hSizeSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hSize), 0);
  ::SendMessageW(st->hSizeSpin, UDM_SETRANGE32, 8, 48);
  AddControl(st, 0, L"STATIC", L"px（8–48）", SS_LEFT, 180, y + 3, 180, 20, 0);

  y = 126;
  AddControl(st, 0, L"STATIC", L"行高", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hLh = AddControl(st, 0, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 100, y, 70, 24, kIdLh,
                       WS_EX_CLIENTEDGE);
  st->hLhSpin = AddControl(st, 0, UPDOWN_CLASSW, L"", UDS_ALIGNRIGHT | UDS_ARROWKEYS, 0, 0, 0, 0,
                           kIdLhSpin);
  ::SendMessageW(st->hLhSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hLh), 0);
  ::SendMessageW(st->hLhSpin, UDM_SETRANGE32, 120, 220);
  AddControl(st, 0, L"STATIC", L"倍（1.20–2.20，步进 0.05）", SS_LEFT, 180, y + 3, 260, 20, 0);

  y = 160;
  AddControl(st, 0, L"STATIC", L"行宽", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hMeasure = AddControl(st, 0, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 100, y, 70, 24,
                            kIdMeasure, WS_EX_CLIENTEDGE);
  st->hMeasureSpin = AddControl(st, 0, UPDOWN_CLASSW, L"",
                                UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT |
                                    UDS_NOTHOUSANDS,
                                0, 0, 0, 0, kIdMeasureSpin);
  ::SendMessageW(st->hMeasureSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(st->hMeasure), 0);
  ::SendMessageW(st->hMeasureSpin, UDM_SETRANGE32, 8, 80);
  AddControl(st, 0, L"STATIC", L"em（8–80）", SS_LEFT, 180, y + 3, 120, 20, 0);
  st->hUnlimited = AddControl(st, 0, L"BUTTON", L"不限宽", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP,
                              300, y, 120, 24, kIdUnlimited);
  AddControl(st, 0, L"STATIC", L"行高与字号改变后，预览按当前锚点恢复位置。", SS_LEFT, 16, 210,
             520, 20, 0);

  // ---------------- 字体 ----------------
  y = 48;
  AddControl(st, 1, L"STATIC", L"槽位", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hSlot[0] = AddControl(st, 1, L"BUTTON", L"界面",
                            BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 100, y, 70, 24, kIdSlotUi);
  st->hSlot[1] = AddControl(st, 1, L"BUTTON", L"正文",
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 176, y, 70, 24, kIdSlotBody);
  st->hSlot[2] = AddControl(st, 1, L"BUTTON", L"等宽",
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 252, y, 70, 24, kIdSlotMono);

  y = 82;
  AddControl(st, 1, L"STATIC", L"字体", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hFamily = AddControl(st, 1, L"COMBOBOX", L"",
                           CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_HASSTRINGS | WS_VSCROLL |
                               WS_TABSTOP,
                           100, y, 330, 240, kIdFamily);
  AddControl(st, 1, L"BUTTON", L"添加", BS_PUSHBUTTON | WS_TABSTOP, 436, y, 60, 24, kIdFamilyAdd);

  y = 118;
  AddControl(st, 1, L"STATIC", L"回退链", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hChainList = AddControl(st, 1, L"LISTBOX", L"",
                              LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER |
                                  WS_TABSTOP,
                              100, y, 330, 116, kIdFamilyList);
  AddControl(st, 1, L"BUTTON", L"上移", BS_PUSHBUTTON | WS_TABSTOP, 436, y, 60, 24, kIdChainUp);
  AddControl(st, 1, L"BUTTON", L"下移", BS_PUSHBUTTON | WS_TABSTOP, 436, y + 30, 60, 24,
             kIdChainDown);
  AddControl(st, 1, L"BUTTON", L"删除", BS_PUSHBUTTON | WS_TABSTOP, 436, y + 60, 60, 24,
             kIdChainDel);

  y = 246;
  AddControl(st, 1, L"STATIC", L"预览", SS_LEFT, 16, y + 3, 70, 20, 0);
  st->hPreview = AddControl(st, 1, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 100, y, 430, 26, kIdPreview);
  st->hPreviewNote = AddControl(st, 1, L"STATIC", L"", SS_LEFT, 100, y + 30, 430, 20,
                                kIdPreviewNote);
  AddControl(st, 1, L"BUTTON", L"刷新字体列表", BS_PUSHBUTTON | WS_TABSTOP, 100, y + 58, 110, 24,
             kIdRefreshFonts);

  // ---------------- 渲染 ----------------
  struct RenderDef {
    int id;
    const wchar_t* text;
  };
  const RenderDef renders[] = {{kIdRenderMermaid, L"Mermaid 图表"},
                               {kIdRenderKatex, L"KaTeX 公式"},
                               {kIdRenderHighlight, L"代码语法高亮"},
                               {kIdRenderHtml, L"内联原始 HTML"},
                               {kIdRenderLocal, L"本地相对图片"}};
  y = 48;
  for (int i = 0; i < 5; ++i) {
    st->hRender[i] = AddControl(st, 2, L"BUTTON", renders[i].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                                16, y + i * 26, 300, 24, renders[i].id);
  }

  y = 190;
  AddControl(st, 2, L"STATIC", L"网络图片加载前：", SS_LEFT, 16, y, 300, 20, 0);
  st->hNetDoc = AddControl(st, 2, L"BUTTON", L"每文档询问一次",
                           BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 28, y + 24, 300, 24,
                           kIdNetDoc);
  st->hNetHost = AddControl(st, 2, L"BUTTON", L"每主机询问一次（DO 运行期间记住）",
                            BS_AUTORADIOBUTTON | WS_TABSTOP, 28, y + 50, 360, 24, kIdNetHost);
  st->hNetOff = AddControl(st, 2, L"BUTTON", L"不询问，直接加载", BS_AUTORADIOBUTTON | WS_TABSTOP,
                           28, y + 76, 300, 24, kIdNetOff);
  st->hOutside = AddControl(st, 2, L"BUTTON", L"允许图片引用文档目录外的路径（../ 与绝对路径）",
                            BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 16, y + 108, 480, 24,
                            kIdOutside);

  // ---------------- 高级 ----------------
  std::wstring version = UtilModuleVersion();
  if (version.empty()) version = L"未知";
  std::wstring runtime = WebHostRuntimeVersion();
  if (runtime.empty()) runtime = L"未安装或不可用";

  y = 50;
  AddControl(st, 3, L"STATIC", L"版本", SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", version.c_str(), SS_LEFT, 110, y + 2, 300, 20, kIdVersion);
  y = 78;
  AddControl(st, 3, L"STATIC", L"WebView2", SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", runtime.c_str(), SS_LEFT, 110, y + 2, 420, 20, kIdWebView);
  y = 108;
  AddControl(st, 3, L"STATIC", L"巨型块", SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", L"超过 2000 行或 256 KiB 时按纯文本窗口化", SS_LEFT, 110, y + 2,
             420, 20, kIdGiant);
  y = 136;
  AddControl(st, 3, L"STATIC", L"常驻窗口", SS_LEFT, 16, y + 2, 90, 20, 0);
  AddControl(st, 3, L"STATIC", L"视口上下各 120 块", SS_LEFT, 110, y + 2, 420, 20, kIdResident);

  y = 178;
  AddControl(st, 3, L"BUTTON", L"打开日志目录", BS_PUSHBUTTON | WS_TABSTOP, 16, y, 130, 26,
             kIdOpenLog);
  AddControl(st, 3, L"BUTTON", L"打开配置根目录", BS_PUSHBUTTON | WS_TABSTOP, 154, y, 140, 26,
             kIdOpenConfig);
  AddControl(st, 3, L"BUTTON", L"打开皮肤目录", BS_PUSHBUTTON | WS_TABSTOP, 302, y, 130, 26,
             kIdOpenSkins);

  // 底部按钮
  AddControl(st, -1, L"BUTTON", L"恢复默认", BS_PUSHBUTTON | WS_TABSTOP, 16, 392, 100, 28,
             kIdRestore);
  st->hApply = AddControl(st, -1, L"BUTTON", L"应用",
                          BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 322, 392, 70, 28, kIdApply);
  AddControl(st, -1, L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, 400, 392, 70, 28, kIdOk);
  AddControl(st, -1, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 478, 392, 70, 28, kIdCancel);

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
      if (code == EN_CHANGE) SetDirty(st, true);
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
      return TRUE;
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
        if (hdr->idFrom == kIdSizeSpin || hdr->idFrom == kIdMeasureSpin) {
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

  HWND hwnd = ::CreateWindowExW(0, kClass, L"Markdown 查看器设置", style, x, y, w, h, parent,
                                nullptr, ModuleHandle(), st);
  if (!hwnd) {
    if (st->font) ::DeleteObject(st->font);
    delete st;
    LogLine(L"SETTINGS: 创建窗口失败 err=%lu", ::GetLastError());
    return nullptr;
  }

  g_hwnd = hwnd;
  ::SetWindowTextW(hwnd, L"Markdown 查看器设置");
  ::ShowWindow(hwnd, SW_SHOW);
  ::SetForegroundWindow(hwnd);
  LogLine(L"SETTINGS: 已打开设置窗口 %p（%u dpi）", hwnd, st->dpi);
  return hwnd;
}
