// settings_dlg.cpp - 最小设置窗口：网络图片的确认粒度。
//
// 非模态：CreateWindowEx 之后交给 Opus 的消息泵；不自己跑 GetMessage。
// M6 会把它扩成四页 Tab，这里先把设置入口与 config 写盘跑通。

#include "settings_dlg.h"

#include <windows.h>

#include "config.h"
#include "log.h"

namespace {

const wchar_t kClass[] = L"DopusMdViewerSettings";
const int kIdDoc = 1001;
const int kIdHost = 1002;
const int kIdOff = 1003;

HWND g_hwnd = nullptr;

struct DialogState {
  HFONT font = nullptr;
};

HMODULE ModuleHandle(void);

int Scale(int value, UINT dpi) {
  return ::MulDiv(value, static_cast<int>(dpi), 96);
}

void ApplyFont(HWND hwnd, HFONT font) {
  ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND AddControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                int y, int cx, int cy, int id, HFONT font) {
  HWND h = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
  if (h && font) ApplyFont(h, font);
  return h;
}

void CheckForMode(HWND hwnd) {
  const int mode = ConfigNetImageMode();
  const int id = (mode == 0) ? kIdOff : (mode == 2 ? kIdDoc : kIdHost);
  ::CheckRadioButton(hwnd, kIdDoc, kIdOff, id);
}

bool SaveFromControls(HWND hwnd) {
  int mode = 1;
  if (::IsDlgButtonChecked(hwnd, kIdDoc) == BST_CHECKED) {
    mode = 2;
  } else if (::IsDlgButtonChecked(hwnd, kIdOff) == BST_CHECKED) {
    mode = 0;
  }
  if (ConfigSetNetImageMode(mode)) return true;
  ::MessageBoxW(hwnd, L"写入 config.json 失败，详见日志。", L"DopusMdViewer",
                MB_OK | MB_ICONERROR);
  return false;
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  DialogState* state =
      reinterpret_cast<DialogState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (uMsg) {
    case WM_NCCREATE: {
      const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
      state = static_cast<DialogState*>(cs->lpCreateParams);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      return TRUE;
    }

    case WM_CREATE: {
      if (!state) return -1;
      const UINT dpi = ::GetDpiForWindow(hwnd);
      const int d = dpi ? static_cast<int>(dpi) : 96;

      AddControl(hwnd, L"STATIC", L"网络图片加载前：",
                 SS_LEFT, Scale(20, d), Scale(16, d), Scale(330, d), Scale(20, d), 0,
                 state->font);
      AddControl(hwnd, L"BUTTON", L"每文档询问一次", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP,
                 Scale(32, d), Scale(44, d), Scale(300, d), Scale(22, d), kIdDoc, state->font);
      AddControl(hwnd, L"BUTTON", L"每主机每文档询问一次",
                 BS_AUTORADIOBUTTON | WS_TABSTOP, Scale(32, d), Scale(70, d), Scale(300, d),
                 Scale(22, d), kIdHost, state->font);
      AddControl(hwnd, L"BUTTON", L"不询问，直接加载", BS_AUTORADIOBUTTON | WS_TABSTOP,
                 Scale(32, d), Scale(96, d), Scale(300, d), Scale(22, d), kIdOff, state->font);
      AddControl(hwnd, L"STATIC",
                 L"网络图片使用 https。询问时预览会等待你的选择；\n"
                 L"文档内的其他远程资源仍会被 CSP 挡住。",
                 SS_LEFT, Scale(20, d), Scale(126, d), Scale(330, d), Scale(38, d), 0,
                 state->font);
      AddControl(hwnd, L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP,
                 Scale(200, d), Scale(170, d), Scale(70, d), Scale(26, d), IDOK, state->font);
      AddControl(hwnd, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, Scale(278, d),
                 Scale(170, d), Scale(70, d), Scale(26, d), IDCANCEL, state->font);
      CheckForMode(hwnd);
      return 0;
    }

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDOK:
          if (SaveFromControls(hwnd)) ::DestroyWindow(hwnd);
          return 0;
        case IDCANCEL:
          ::DestroyWindow(hwnd);
          return 0;
        default:
          return 0;
      }

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      g_hwnd = nullptr;
      if (state) {
        if (state->font) ::DeleteObject(state->font);
        delete state;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
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
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
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

  DialogState* state = new (std::nothrow) DialogState();
  if (!state) return nullptr;
  state->font = CreateUiFont();

  const UINT dpi = parent ? ::GetDpiForWindow(parent) : 96;
  const int d = dpi ? static_cast<int>(dpi) : 96;
  RECT rc = {0, 0, Scale(380, d), Scale(214, d)};
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

  HWND hwnd = ::CreateWindowExW(0, kClass, L"DopusMdViewer 设置", style, x, y, w, h, parent,
                                nullptr, ModuleHandle(), state);
  if (!hwnd) {
    if (state->font) ::DeleteObject(state->font);
    delete state;
    LogLine(L"SETTINGS: 创建窗口失败 err=%lu", ::GetLastError());
    return nullptr;
  }

  g_hwnd = hwnd;
  ::ShowWindow(hwnd, SW_SHOW);
  ::SetForegroundWindow(hwnd);
  LogLine(L"SETTINGS: 已打开设置窗口 %p", hwnd);
  return hwnd;
}

