// skinedit.cpp — 皮肤编辑器外壳（便携，单窗口 WebView2）。
//
// 只做六件事，别的一概不管（表单与预览页是网页那一层的事）：
//   1. 程序目录可写校验：不可写就报一句并退出，便携的前提是能把 WebView2 缓存与
//      skinedit.ini 都写在程序旁边；不可写时静默降级会变成"改了存不上"。
//   2. 把 assets\ 映射成虚拟主机 https://skinedit.localhost/ ，页面只从这一台取资源。
//   3. 用程序目录下的 WebView2\ 当用户数据目录（不放 APPDATA）。
//   4. 把解析好的资产目录与皮肤目录推给页面（postMessage），页面自己决定怎么用。
//   5. 按页面请求做字节 I/O 与本机对话框：枚举皮肤、读、写、stat、打开/另存为、
//      系统取色器。CSS 的编码与解析一律留在页面侧，宿主不碰文本内容。
//   6. 记一份日志：从启动到首帧的耗时、映射结果、页面发回来的消息。
//
// 命令行：skinedit.exe [/web <资产目录>] [/skins <皮肤目录>]
//   /web   不给就用 <exe 目录>\assets
//   /skins 不给就用插件的数据目录（%APPDATA%\GPSoftware\Directory Opus\User Data\DopusMdViewer\skins）
//
// 窗口类用本模块的 hInstance 注册，退出时注销：类一旦留在系统里，下次运行会拿到
// 一个指向旧代码的 lpfnWndProc。

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>

#include <objbase.h>
#include <eventtoken.h>
#include <wrl.h>
#include <WebView2.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kClassName[] = L"DvSkinEditMain";
const wchar_t kHost[] = L"skinedit.localhost";
const wchar_t kStartPage[] = L"https://skinedit.localhost/shell.html";
const wchar_t kTitle[] = L"皮肤编辑器";

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_web;
bool g_ready = false;

std::wstring g_exeDir;
std::wstring g_webDir;
std::wstring g_skinsDir;
std::wstring g_logPath;
LARGE_INTEGER g_tickFreq = {};
LARGE_INTEGER g_tickStart = {};

/* ---------------- 小工具 ---------------- */

bool FileExists(const std::wstring& path) {
  const DWORD attr = ::GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
  const DWORD attr = ::GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Join(const std::wstring& a, const wchar_t* b) {
  if (a.empty()) return b;
  std::wstring out = a;
  if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
  out += b;
  return out;
}

std::wstring ExeDir() {
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
  if (n == 0 || n >= ARRAYSIZE(buf)) return std::wstring();
  std::wstring path(buf, n);
  const size_t cut = path.find_last_of(L"\\/");
  return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

std::string ToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr,
                                      0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}

double MsSinceStart() {
  if (!g_tickFreq.QuadPart) return 0.0;
  LARGE_INTEGER now = {};
  ::QueryPerformanceCounter(&now);
  return (now.QuadPart - g_tickStart.QuadPart) * 1000.0 / g_tickFreq.QuadPart;
}

/* 日志只追加。程序目录写不进去时退到 %TEMP%——"目录不可写"恰恰是最需要看日志的
   时候，日志也跟着写不进去就等于没有。 */
void Log(const wchar_t* fmt, ...) {
  if (g_logPath.empty()) return;
  wchar_t line[2048] = {};
  va_list ap;
  va_start(ap, fmt);
  ::_vsnwprintf_s(line, _TRUNCATE, fmt, ap);
  va_end(ap);

  wchar_t stamp[32] = {};
  SYSTEMTIME st = {};
  ::GetLocalTime(&st);
  ::_snwprintf_s(stamp, _TRUNCATE, L"%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds);

  const std::string utf8 = ToUtf8(std::wstring(stamp) + line + L"\r\n");
  for (int attempt = 0; attempt < 2; ++attempt) {
    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
      ::CloseHandle(h);
      return;
    }
    if (attempt == 0) {
      wchar_t tmp[MAX_PATH] = {};
      const DWORD n = ::GetTempPathW(ARRAYSIZE(tmp), tmp);
      if (n == 0 || n >= ARRAYSIZE(tmp)) return;
      g_logPath = Join(tmp, L"skinedit.log");
    }
  }
}

/* 程序目录可写吗：建一个临时文件再删。只删自己建的那个文件名。 */
bool ExeDirWritable(std::wstring* why) {
  const std::wstring probe = Join(g_exeDir, L"~skinedit-write-test.tmp");
  HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (why) *why = L"无法在程序目录创建文件（错误 " + std::to_wstring(::GetLastError()) + L"）";
    return false;
  }
  ::CloseHandle(h);
  if (!::DeleteFileW(probe.c_str())) {
    if (why) *why = L"能创建但不能删除文件（错误 " + std::to_wstring(::GetLastError()) + L"）";
    return false;
  }
  return true;
}

std::wstring DefaultSkinsDir() {
  wchar_t buf[MAX_PATH] = {};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
    return std::wstring();
  }
  std::wstring dir = std::wstring(buf) + L"\\GPSoftware\\Directory Opus\\User Data\\DopusMdViewer";
  return Join(dir, L"skins");
}

std::wstring JsonStr(const std::wstring& s) {
  std::wstring o = L"\"";
  for (const wchar_t c : s) {
    switch (c) {
      case L'"': o += L"\\\""; break;
      case L'\\': o += L"\\\\"; break;
      case L'\n': o += L"\\n"; break;
      case L'\r': o += L"\\r"; break;
      case L'\t': o += L"\\t"; break;
      default:
        if (c < 0x20) {
          wchar_t buf[8] = {};
          ::_snwprintf_s(buf, _TRUNCATE, L"\\u%04X", static_cast<unsigned>(c));
          o += buf;
        } else {
          o += c;
        }
    }
  }
  o += L"\"";
  return o;
}

/* ---------------- 宿主桥：字节 I/O 与本机对话框 ----------------
   页面发 {op, id, …}，路径与文件字节一律 base64(UTF-8)：这样请求里不会出现引号、
   反斜杠与控制字符，宿主按固定格式取值即可，不必塞一个通用 JSON 解析器。
   反向仍用 PostWebMessageAsJson + JsonStr，交给浏览器原生解析。
   mtime 是 FILETIME 量级的 64 位整数，只能当字符串传：JS Number 过 2^53 会丢精度。 */

std::wstring FromUtf8(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Decode(const std::wstring& in) {
  int acc = 0, bits = 0;
  std::string out;
  for (const wchar_t c : in) {
    if (c == L'=' || c > 127) break;
    const char* p = std::strchr(kB64, static_cast<char>(c));
    if (!p) continue;
    acc = (acc << 6) | static_cast<int>(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

std::wstring Base64Encode(const std::string& in) {
  static const wchar_t tbl[] =
      L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::wstring out;
  size_t i = 0;
  while (i + 2 < in.size()) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8) |
                       static_cast<unsigned char>(in[i + 2]);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
    i += 3;
  }
  if (i < in.size()) {
    const unsigned v = static_cast<unsigned char>(in[i]) << 16 |
                       (i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) << 8 : 0);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : L'=';
    out += L'=';
  }
  return out;
}

/* 只取「值里没有转义字符」的字符串字段：base64 与枚举都满足这个前提。 */
std::wstring Field(const std::wstring& json, const wchar_t* key) {
  const std::wstring pat = std::wstring(L"\"") + key + L"\":\"";
  const size_t a = json.find(pat);
  if (a == std::wstring::npos) return std::wstring();
  const size_t b = a + pat.size();
  const size_t e = json.find(L'"', b);
  return e == std::wstring::npos ? std::wstring() : json.substr(b, e - b);
}

long long FieldNum(const std::wstring& json, const wchar_t* key) {
  const std::wstring pat = std::wstring(L"\"") + key + L"\":";
  const size_t a = json.find(pat);
  if (a == std::wstring::npos) return 0;
  size_t b = a + pat.size(), e = b;
  while (e < json.size() && ((json[e] >= L'0' && json[e] <= L'9') || json[e] == L'-')) ++e;
  return ::_wcstoi64(json.substr(b, e - b).c_str(), nullptr, 10);
}

struct FileStat {
  bool exists = false;
  long long size = 0;
  std::wstring mtime = L"0";
};

FileStat StatOf(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA d = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return FileStat();
  ULARGE_INTEGER size = {}, time = {};
  size.LowPart = d.nFileSizeLow;
  size.HighPart = d.nFileSizeHigh;
  time.LowPart = d.ftLastWriteTime.dwLowDateTime;
  time.HighPart = d.ftLastWriteTime.dwHighDateTime;
  FileStat s;
  s.exists = true;
  s.size = static_cast<long long>(size.QuadPart);
  s.mtime = std::to_wstring(time.QuadPart);
  return s;
}

constexpr DWORD kMaxSkinBytes = 1024 * 1024;

bool ReadBytes(const std::wstring& path, std::string* out, std::wstring* why) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *why = L"打不开文件（错误 " + std::to_wstring(::GetLastError()) + L"）";
    return false;
  }
  const FileStat st = StatOf(path);
  if (!st.exists || st.size > kMaxSkinBytes) {
    ::CloseHandle(h);
    *why = L"文件超过 1 MB，按皮肤契约拒绝";
    return false;
  }
  out->resize(static_cast<size_t>(st.size));
  DWORD got = 0;
  const bool ok = st.size == 0 ||
                  (::ReadFile(h, out->data(), static_cast<DWORD>(st.size), &got, nullptr) &&
                   got == st.size);
  ::CloseHandle(h);
  if (!ok) *why = L"读取失败（错误 " + std::to_wstring(::GetLastError()) + L"）";
  return ok;
}

bool WriteBytes(const std::wstring& path, const std::string& bytes, std::wstring* why) {
  // 上级目录可能还不存在（首次另存为到用户皮肤目录时）。
  const size_t cut = path.find_last_of(L"\\/");
  if (cut != std::wstring::npos) {
    const std::wstring dir = path.substr(0, cut);
    if (!DirExists(dir)) ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  }
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *why = L"无法写入（错误 " + std::to_wstring(::GetLastError()) + L"）";
    return false;
  }
  DWORD put = 0;
  const bool ok = bytes.empty() ||
                  (::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) &&
                   put == bytes.size());
  ::CloseHandle(h);
  if (!ok) *why = L"写入失败（错误 " + std::to_wstring(::GetLastError()) + L"）";
  return ok;
}

/* 内置模板随程序分发，不能被编辑器改写：保存到 assets\skins\ 一律拒绝。 */
bool IsBuiltinPath(const std::wstring& path) {
  const std::wstring root = Join(g_webDir, L"skins") + L"\\";
  if (path.size() < root.size()) return false;
  return ::_wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0;
}

std::wstring PickFile(bool save, const std::wstring& defaultName, bool* cancel) {
  *cancel = false;
  ComPtr<IFileDialog> dlg;
  if (FAILED(::CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(dlg.GetAddressOf())))) {
    *cancel = true;
    return std::wstring();
  }
  COMDLG_FILTERSPEC spec = {L"皮肤文件 (*.css)", L"*.css"};
  dlg->SetFileTypes(1, &spec);
  dlg->SetFileTypeIndex(1);
  dlg->SetDefaultExtension(L"css");
  if (!g_skinsDir.empty() && DirExists(g_skinsDir)) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(::SHCreateItemFromParsingName(g_skinsDir.c_str(), nullptr,
                                                IID_PPV_ARGS(folder.GetAddressOf())))) {
      dlg->SetFolder(folder.Get());
    }
  }
  if (save && !defaultName.empty()) dlg->SetFileName(defaultName.c_str());
  if (FAILED(dlg->Show(g_hwnd))) {
    *cancel = true;
    return std::wstring();
  }
  ComPtr<IShellItem> item;
  if (FAILED(dlg->GetResult(item.GetAddressOf()))) {
    *cancel = true;
    return std::wstring();
  }
  LPWSTR path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
    *cancel = true;
    return std::wstring();
  }
  std::wstring out(path);
  ::CoTaskMemFree(path);
  return out;
}

void Push(const std::wstring& json) {
  if (!g_ready || !g_web) return;
  g_web->PostWebMessageAsJson(json.c_str());
}

void HandleMessage(const std::wstring& json) {
  const std::wstring op = Field(json, L"op");
  const long long id = FieldNum(json, L"id");
  const std::wstring head = L"{\"id\":" + std::to_wstring(id) + L",";
  const auto error = [&](const wchar_t* code, const std::wstring& why) {
    Push(L"{\"type\":\"error\",\"id\":" + std::to_wstring(id) + L",\"op\":" + JsonStr(op) +
         L",\"code\":" + JsonStr(code) + L",\"message\":" + JsonStr(why) + L"}");
  };
  const std::wstring path = FromUtf8(Base64Decode(Field(json, L"path")));

  if (op == L"list") {
    std::wstring items = L"[";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = ::FindFirstFileW(Join(g_skinsDir, L"*.css").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring full = Join(g_skinsDir, fd.cFileName);
        const FileStat st = StatOf(full);
        ULARGE_INTEGER sz = {};
        sz.QuadPart = static_cast<ULONGLONG>(st.size);
        items += L"{\"name\":" + JsonStr(fd.cFileName) + L",\"path\":" + JsonStr(full) +
                 L",\"size\":" + std::to_wstring(sz.QuadPart) + L",\"mtime\":" +
                 JsonStr(st.mtime) + L"},";
      } while (::FindNextFileW(h, &fd));
      ::FindClose(h);
    }
    if (items.back() == L',') items.pop_back();
    items += L"]";
    Push(head + L"\"type\":\"dir\",\"dir\":" + JsonStr(g_skinsDir) + L",\"items\":" + items + L"}");
    return;
  }

  if (op == L"stat") {
    const FileStat st = StatOf(path);
    Push(head + L"\"type\":\"stat\",\"path\":" + JsonStr(path) + L",\"exists\":" +
         (st.exists ? L"true" : L"false") + L",\"size\":" + std::to_wstring(st.size) +
         L",\"mtime\":" + JsonStr(st.mtime) + L"}");
    return;
  }

  if (op == L"read") {
    std::string bytes;
    std::wstring why;
    if (!ReadBytes(path, &bytes, &why)) return error(L"READ_FAILED", why);
    const FileStat st = StatOf(path);
    Push(head + L"\"type\":\"file\",\"path\":" + JsonStr(path) + L",\"bytes\":" +
         JsonStr(Base64Encode(bytes)) + L",\"size\":" + std::to_wstring(st.size) +
         L",\"mtime\":" + JsonStr(st.mtime) + L"}");
    return;
  }

  if (op == L"write") {
    if (IsBuiltinPath(path)) return error(L"READONLY", L"内置模板只读，请另存为一份新皮肤");
    const std::wstring expect = Field(json, L"expectMtime");
    if (!expect.empty()) {
      const FileStat st = StatOf(path);
      if (st.exists && st.mtime != expect) {
        return error(L"EXTERNAL_MODIFIED", L"文件已被外部修改");
      }
    }
    const std::string bytes = Base64Decode(Field(json, L"bytes"));
    std::wstring why;
    if (!WriteBytes(path, bytes, &why)) return error(L"WRITE_FAILED", why);
    const FileStat st = StatOf(path);
    Push(head + L"\"type\":\"saved\",\"path\":" + JsonStr(path) + L",\"size\":" +
         std::to_wstring(st.size) + L",\"mtime\":" + JsonStr(st.mtime) + L"}");
    return;
  }

  if (op == L"dialog") {
    const std::wstring kind = Field(json, L"kind");
    const std::wstring name = FromUtf8(Base64Decode(Field(json, L"name")));
    bool cancel = false;
    const std::wstring picked = PickFile(kind == L"save", name, &cancel);
    Push(head + L"\"type\":\"dialog\",\"kind\":" + JsonStr(kind) + L",\"cancel\":" +
         (cancel ? L"true" : L"false") + L",\"path\":" + JsonStr(picked) + L"}");
    return;
  }

  error(L"UNKNOWN_OP", L"未知操作：" + op);
}

/* ---------------- 页面通道 ---------------- */

/* 路径推给页面。页面拿它去列皮肤、拼标题、出错时显示。 */
void PushHello() {
  if (!g_ready || !g_web) return;
  const std::wstring json = L"{\"type\":\"hello\",\"web\":" + JsonStr(g_webDir) +
                            L",\"skins\":" + JsonStr(g_skinsDir) +
                            L",\"exe\":" + JsonStr(g_exeDir) +
                            L",\"ms\":" + std::to_wstring(static_cast<long long>(MsSinceStart())) +
                            L"}";
  const HRESULT hr = g_web->PostWebMessageAsJson(json.c_str());
  Log(L"PUSH hello hr=0x%08lX web=%s skins=%s", static_cast<unsigned long>(hr), g_webDir.c_str(),
      g_skinsDir.c_str());
}

/* 导航失败时不留白屏：把解析到的路径与原因铺在页面上。 */
void ShowFailPage(const std::wstring& reason) {
  if (!g_web) return;
  std::wstring html =
      L"<!doctype html><meta charset=\"utf-8\"><style>"
      L"body{margin:0;padding:24px;font:14px/1.7 'Segoe UI','Microsoft YaHei UI',sans-serif;"
      L"background:#fff;color:#1f2328}"
      L"h1{font-size:16px;margin:0 0 12px}code{background:#f6f8fa;padding:2px 6px;"
      L"border-radius:4px;font-family:Consolas,monospace}"
      L"li{margin:4px 0}</style><h1>编辑器页面没能加载</h1><ul><li>原因：<code>" +
      reason + L"</code></li><li>资产目录：<code>" + g_webDir +
      L"</code></li><li>皮肤目录：<code>" + g_skinsDir +
      L"</code></li><li>程序目录：<code>" + g_exeDir + L"</code></li></ul>";
  g_web->NavigateToString(html.c_str());
}

/* ---------------- WebView2 ---------------- */

void ApplySettings() {
  ComPtr<ICoreWebView2Settings> settings;
  if (FAILED(g_web->get_Settings(&settings)) || !settings) return;
  // 开发工具留着（这是开发者工具），浏览器自带的右键菜单、状态栏、缩放不要
  settings->put_AreDefaultContextMenusEnabled(FALSE);
  settings->put_AreDevToolsEnabled(TRUE);
  settings->put_IsStatusBarEnabled(FALSE);
  settings->put_IsZoomControlEnabled(FALSE);
}

void ResizeWeb() {
  if (!g_controller || !g_hwnd) return;
  RECT rc = {};
  ::GetClientRect(g_hwnd, &rc);
  g_controller->put_Bounds(rc);
}

void StartWebView() {
  const std::wstring dataDir = Join(g_exeDir, L"WebView2");
  ::CreateDirectoryW(dataDir.c_str(), nullptr);

  const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, dataDir.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
              Log(L"ENV 失败 hr=0x%08lX", static_cast<unsigned long>(result));
              ::MessageBoxW(g_hwnd, L"WebView2 运行时不可用，无法启动。", kTitle, MB_ICONERROR);
              ::PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
              return S_OK;
            }
            g_env = env;
            Log(L"ENV 就绪 %.0f ms", MsSinceStart());
            env->CreateCoreWebView2Controller(
                g_hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT {
                      if (FAILED(r2) || !ctrl) {
                        Log(L"CONTROLLER 失败 hr=0x%08lX", static_cast<unsigned long>(r2));
                        return S_OK;
                      }
                      g_controller = ctrl;
                      if (FAILED(ctrl->get_CoreWebView2(&g_web)) || !g_web) return S_OK;
                      Log(L"CONTROLLER 就绪 %.0f ms", MsSinceStart());
                      ResizeWeb();
                      ctrl->put_IsVisible(TRUE);
                      ApplySettings();

                      // 资产目录映射成虚拟主机：页面与字体、皮肤都从这一台同源取
                      ComPtr<ICoreWebView2_3> web3;
                      if (SUCCEEDED(g_web.As(&web3)) && web3) {
                        const HRESULT mapHr = web3->SetVirtualHostNameToFolderMapping(
                            kHost, g_webDir.c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                        Log(L"MAP %s → %s hr=0x%08lX", kHost, g_webDir.c_str(),
                            static_cast<unsigned long>(mapHr));
                      } else {
                        Log(L"MAP 跳过：拿不到 ICoreWebView2_3");
                      }

                      g_web->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args)
                                  -> HRESULT {
                                BOOL ok = FALSE;
                                if (args) args->get_IsSuccess(&ok);
                                Log(L"NAV 完成 ok=%d %.0f ms", ok ? 1 : 0, MsSinceStart());
                                if (!ok) {
                                  COREWEBVIEW2_WEB_ERROR_STATUS st =
                                      COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                  if (args) args->get_WebErrorStatus(&st);
                                  ShowFailPage(L"导航失败，WebErrorStatus=" +
                                               std::to_wstring(static_cast<int>(st)));
                                  return S_OK;
                                }
                                g_ready = true;
                                PushHello();
                                return S_OK;
                              })
                              .Get(),
                          nullptr);

                      g_web->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args)
                                  -> HRESULT {
                                LPWSTR raw = nullptr;
                                if (args && SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                                  const std::wstring msg(raw);
                                  ::CoTaskMemFree(raw);
                                  // 日志只留短前缀：写皮肤的 base64 有几十 KB，全记下来没意义。
                                  const std::wstring brief = msg.size() > 200
                                      ? msg.substr(0, 200) + L"…" : msg;
                                  Log(L"MSG %s", brief.c_str());
                                  if (msg.find(L"\"op\":") != std::wstring::npos) HandleMessage(msg);
                                }
                                return S_OK;
                              })
                              .Get(),
                          nullptr);

                      g_web->Navigate(kStartPage);
                      Log(L"NAV 开始 %s", kStartPage);
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    Log(L"ENV 创建调用失败 hr=0x%08lX", static_cast<unsigned long>(hr));
  }
}

/* ---------------- 窗口 ---------------- */

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      ResizeWeb();
      return 0;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
}

bool RegisterWindowClass(HINSTANCE inst) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  if (::RegisterClassExW(&wc)) return true;
  // 上一次没卸载干净：先注销再注册，避免拿到指向旧代码的窗口过程
  if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    ::UnregisterClassW(kClassName, inst);
    return ::RegisterClassExW(&wc) != 0;
  }
  return false;
}

/* ---------------- 命令行 ---------------- */

/* 取 <开关> <值>：值可以带引号，也可以直接跟。
   开关同时接受 /web 与 --web：Git Bash 这类 shell 会把以 "/" 开头的参数当成
   路径改写成 C:...web，双横线不会被改写，两种都认省得踩。 */
bool TakeArg(int argc, wchar_t** argv, const wchar_t* name, std::wstring* out) {
  for (int i = 1; i < argc; ++i) {
    const wchar_t* arg = argv[i];
    while (*arg == L'/' || *arg == L'-') ++arg;
    if (::CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, arg, -1, name, -1) != CSTR_EQUAL) {
      continue;
    }
    if (i + 1 >= argc) return false;
    *out = argv[i + 1];
    Log(L"ARG %s = %s", name, out->c_str());
    return true;
  }
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
  ::QueryPerformanceFrequency(&g_tickFreq);
  ::QueryPerformanceCounter(&g_tickStart);

  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  g_exeDir = ExeDir();
  if (g_exeDir.empty()) {
    ::MessageBoxW(nullptr, L"取不到程序目录。", kTitle, MB_ICONERROR);
    return 2;
  }
  g_logPath = Join(g_exeDir, L"skinedit.log");

  int argc = 0;
  wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

  std::wstring webArg;
  std::wstring skinsArg;
  if (argv) {
    Log(L"ARGS 收到 %d 个", argc - 1);
    TakeArg(argc, argv, L"web", &webArg);
    TakeArg(argc, argv, L"skins", &skinsArg);
    ::LocalFree(argv);
  }

  g_webDir = webArg.empty() ? Join(g_exeDir, L"assets") : webArg;
  g_skinsDir = skinsArg.empty() ? DefaultSkinsDir() : skinsArg;

  Log(L"START 程序目录=%s 资产=%s 皮肤=%s", g_exeDir.c_str(), g_webDir.c_str(),
      g_skinsDir.c_str());

  std::wstring why;
  if (!ExeDirWritable(&why)) {
    Log(L"ABORT 目录不可写：%s", why.c_str());
    const std::wstring msg = L"皮肤编辑器需要能写程序所在文件夹（WebView2 缓存与设置都放在那里）。\n"
                             L"请把它移到可写目录后再运行。\n\n" + why;
    ::MessageBoxW(nullptr, msg.c_str(), kTitle, MB_ICONERROR);
    return 3;
  }
  Log(L"CHECK 目录可写 ok");

  if (!FileExists(Join(g_webDir, L"shell.html"))) {
    Log(L"ABORT 缺 shell.html：%s", g_webDir.c_str());
    const std::wstring msg = L"找不到编辑器页面：\n" + Join(g_webDir, L"shell.html") +
                             L"\n\n用 /web <目录> 指定资产目录，或把 assets 放在程序旁边。";
    ::MessageBoxW(nullptr, msg.c_str(), kTitle, MB_ICONERROR);
    return 4;
  }

  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  if (!RegisterWindowClass(inst)) {
    Log(L"ABORT 窗口类注册失败 err=%lu", ::GetLastError());
    return 5;
  }

  g_hwnd = ::CreateWindowExW(0, kClassName, L"皮肤编辑器 — DopusMdViewer", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, nullptr, nullptr, inst,
                             nullptr);
  if (!g_hwnd) {
    Log(L"ABORT 建窗口失败 err=%lu", ::GetLastError());
    ::UnregisterClassW(kClassName, inst);
    return 6;
  }
  Log(L"WINDOW %.0f ms", MsSinceStart());

  StartWebView();
  ::ShowWindow(g_hwnd, SW_SHOW);
  ::UpdateWindow(g_hwnd);

  MSG msg = {};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  if (g_controller) g_controller->Close();
  g_web.Reset();
  g_controller.Reset();
  g_env.Reset();
  ::UnregisterClassW(kClassName, inst);
  Log(L"EXIT %.0f ms", MsSinceStart());
  ::CoUninitialize();
  return 0;
}
