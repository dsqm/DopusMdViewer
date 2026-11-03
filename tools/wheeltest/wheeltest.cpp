// wheeltest.cpp - 往指定窗口发真实滚轮输入，用来测「首次滚动」这类只有真输入才
// 会走到的路径（合成 WheelEvent 绕不过 WebView2 的输入管线，测不出代价）。
//
// 用法：wheeltest.exe [窗口类名] [滚动次数] [每次间隔毫秒]
//   默认 DopusMdViewerHostTest / 40 次 / 30ms

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t** argv) {
  const wchar_t* cls = argc > 1 ? argv[1] : L"DopusMdViewerHostTest";
  const int steps = argc > 2 ? _wtoi(argv[2]) : 40;
  const int gap = argc > 3 ? _wtoi(argv[3]) : 30;

  HWND hwnd = ::FindWindowW(cls, nullptr);
  if (!hwnd) {
    wprintf(L"找不到窗口类 %s\n", cls);
    return 2;
  }
  ::ShowWindow(hwnd, SW_RESTORE);
  ::SetForegroundWindow(hwnd);
  ::Sleep(300);

  RECT rc = {};
  ::GetClientRect(hwnd, &rc);
  POINT pt = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
  ::ClientToScreen(hwnd, &pt);
  ::SetCursorPos(pt.x, pt.y);
  ::Sleep(200);

  INPUT in = {};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = MOUSEEVENTF_WHEEL;
  in.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);  // 负数 = 向下
  for (int i = 0; i < steps; i++) {
    ::SendInput(1, &in, sizeof(in));
    ::Sleep(static_cast<DWORD>(gap));
  }
  wprintf(L"已发送 %d 次滚轮，间隔 %d ms\n", steps, gap);
  return 0;
}
