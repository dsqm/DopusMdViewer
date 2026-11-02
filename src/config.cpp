// config.cpp - config.json 的读写。**唯一 include nlohmann/json 的编译单元**
// （单头两万多行，扩散出去每个 TU 多付 2–4 秒编译）。

#include "config.h"

#include <windows.h>

#include <exception>
#include <mutex>

#include <json/json.hpp>  // build.bat 把 %PROJ%third_party 放进 INCLUDE

#include "log.h"
#include "util.h"

namespace {

const size_t kMaxConfigBytes = 256 * 1024;

Config g_config;
bool g_loaded = false;
std::mutex g_modeMutex;

FontStack DefaultUi() {
  return {L"Segoe UI", L"Microsoft YaHei UI", L"system-ui", L"sans-serif"};
}

FontStack DefaultMono() {
  return {L"Cascadia Mono", L"Consolas", L"Courier New", L"monospace"};
}

void SetDefaults(Config* c) {
  c->skin = L"light";
  c->font.ui = DefaultUi();
  c->font.body.clear();
  c->font.mono = DefaultMono();
  c->font.sizePx = 15;
  c->font.lineHeight = 1.65;
  c->font.measureEm = 40;
  c->netImageMode = 1;  // 每主机每文档确认
  c->allowOutsideImages = false;
  c->renderMermaid = true;
  c->renderKatex = true;
  c->renderHighlight = true;
  c->renderInlineHtml = true;
  c->renderLocalImages = true;
  c->copyCodeBlock = true;
  c->copyTable = true;
  c->codeLineNumbers = true;
  c->tableCopyMode = 0;
  c->showFindButton = true;
  c->showOutlineButton = true;
  c->enableLog = false;
  c->webviewBaseDir.clear();
  c->prewarm = false;
  c->idleReleaseMinutes = 3;
}

int ClampInt(int v, int lo, int hi, int fallback) {
  if (v < lo || v > hi) return fallback;
  return v;
}

double ClampDouble(double v, double lo, double hi, double fallback) {
  if (!(v >= lo && v <= hi)) return fallback;  // 同时挡住 NaN
  return v;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
  return out;
}

FontStack ReadStack(const nlohmann::json& obj, const char* key, const FontStack& fallback) {
  if (!obj.contains(key) || !obj[key].is_array()) return fallback;

  FontStack out;
  for (const auto& item : obj[key]) {
    if (!item.is_string()) continue;
    const std::string name = item.get<std::string>();
    if (name.empty()) continue;
    out.push_back(Utf8ToWide(name));
  }
  return out.empty() ? fallback : out;
}

void ApplyJson(const nlohmann::json& root, Config* c) {
  if (root.contains("netImageConfirm") && root["netImageConfirm"].is_string()) {
    const std::string v = root["netImageConfirm"].get<std::string>();
    if (v == "off") {
      c->netImageMode = 0;
    } else if (v == "doc") {
      c->netImageMode = 2;
    } else {
      c->netImageMode = 1;
    }
  }

  if (root.contains("allowOutsideImages") && root["allowOutsideImages"].is_boolean()) {
    c->allowOutsideImages = root["allowOutsideImages"].get<bool>();
  }

  if (root.contains("enableLog") && root["enableLog"].is_boolean()) {
    c->enableLog = root["enableLog"].get<bool>();
  }

  if (root.contains("webviewBaseDir") && root["webviewBaseDir"].is_string()) {
    c->webviewBaseDir = Utf8ToWide(root["webviewBaseDir"].get<std::string>());
  }

  if (root.contains("prewarm") && root["prewarm"].is_boolean()) {
    c->prewarm = root["prewarm"].get<bool>();
  }

  if (root.contains("idleReleaseMinutes") && root["idleReleaseMinutes"].is_number_integer()) {
    c->idleReleaseMinutes =
        ClampInt(root["idleReleaseMinutes"].get<int>(), 0, 1440, 3);
  }

  if (root.contains("render") && root["render"].is_object()) {
    const nlohmann::json& render = root["render"];
    auto readBool = [&render](const char* key, bool* out) {
      if (render.contains(key) && render[key].is_boolean()) *out = render[key].get<bool>();
    };
    readBool("mermaid", &c->renderMermaid);
    readBool("katex", &c->renderKatex);
    readBool("highlight", &c->renderHighlight);
    readBool("inlineHtml", &c->renderInlineHtml);
    readBool("localImages", &c->renderLocalImages);
    readBool("copyCodeBlock", &c->copyCodeBlock);
    readBool("copyTable", &c->copyTable);
    readBool("lineNumbers", &c->codeLineNumbers);
    readBool("showFindButton", &c->showFindButton);
    readBool("showOutlineButton", &c->showOutlineButton);
  }

  if (root.contains("tableCopyFormat") && root["tableCopyFormat"].is_string()) {
    const std::string v = root["tableCopyFormat"].get<std::string>();
    c->tableCopyMode = (v == "markdown") ? 1 : 0;
  }

  if (root.contains("skin") && root["skin"].is_string()) {
    const std::string id = root["skin"].get<std::string>();
    if (!id.empty()) c->skin = Utf8ToWide(id);
  }

  if (!root.contains("font") || !root["font"].is_object()) return;
  const nlohmann::json& font = root["font"];

  c->font.ui = ReadStack(font, "ui", c->font.ui);
  c->font.mono = ReadStack(font, "mono", c->font.mono);
  c->font.body = ReadStack(font, "body", FontStack());  // 空数组就是"继承界面"

  if (font.contains("size") && font["size"].is_number_integer()) {
    c->font.sizePx = ClampInt(font["size"].get<int>(), 8, 48, 15);
  }
  if (font.contains("lineHeight") && font["lineHeight"].is_number()) {
    c->font.lineHeight = ClampDouble(font["lineHeight"].get<double>(), 1.2, 2.2, 1.65);
  }
  if (font.contains("measure") && font["measure"].is_number_integer()) {
    const int m = font["measure"].get<int>();
    c->font.measureEm = (m == 0) ? 0 : ClampInt(m, 8, 200, 40);
  }
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr,
                        nullptr);
  return out;
}

nlohmann::json StackToJson(const FontStack& stack) {
  nlohmann::json arr = nlohmann::json::array();
  for (const std::wstring& name : stack) arr.push_back(WideToUtf8(name));
  return arr;
}

// 失败时把 Win32 错误码带出去：调用方要拿它拼弹窗，光一个 bool 只能靠猜。
bool WriteConfigAtomically(const wchar_t* path, const std::string& bytes, DWORD* outError) {
  const std::wstring tmp = std::wstring(path) + L".tmp";
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (outError) *outError = ::GetLastError();
    return false;
  }
  DWORD written = 0;
  const BOOL ok = ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr);
  if (!ok || written != bytes.size()) {
    if (outError) *outError = ok ? ERROR_WRITE_FAULT : ::GetLastError();
    ::CloseHandle(h);
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  ::CloseHandle(h);
  if (!::MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
    if (outError) *outError = ::GetLastError();
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace

const wchar_t* ConfigPath(void) {
  static std::wstring path;
  if (path.empty()) {
    const std::wstring dataDir = UtilDataDir();
    if (dataDir.empty()) return L"";
    path = UtilJoin(dataDir, L"config.json");
  }
  return path.c_str();
}

const Config& ConfigLoad(void) {
  std::lock_guard<std::mutex> lock(g_modeMutex);
  SetDefaults(&g_config);
  g_loaded = true;

  const wchar_t* path = ConfigPath();
  // 读不到配置的几条路（目录不可用 / 文件缺失 / 格式坏）都无从得知日志开关的
  // 意图，用 LogLineAlways 绕过开关记下来；否则首次运行连一条现场都没有。
  if (!path || !*path) {
    LogLineAlways(L"CONFIG: 数据目录不可用，全部用默认值");
    return g_config;
  }

  std::string bytes;
  if (!UtilReadFileBytes(path, kMaxConfigBytes, &bytes)) {
    LogLineAlways(L"CONFIG: 没有 %s，用默认值", path);
    return g_config;
  }

  try {
    const nlohmann::json root = nlohmann::json::parse(bytes);
    if (!root.is_object()) {
      LogLineAlways(L"CONFIG: 顶层不是对象，用默认值");
      return g_config;
    }
    ApplyJson(root, &g_config);
    LogSetEnabled(g_config.enableLog);
    LogLine(L"CONFIG: 已读取 %s skin=\"%s\" size=%dpx lh=%.2f measure=%dem log=%d", path,
            g_config.skin.c_str(), g_config.font.sizePx, g_config.font.lineHeight,
            g_config.font.measureEm, g_config.enableLog ? 1 : 0);
  } catch (const std::exception& e) {
    // 读坏就用默认值，不打扰用户
    LogLineAlways(L"CONFIG: 解析失败（%hs），用默认值", e.what());
  }

  return g_config;
}

const Config& ConfigGet(void) {
  // ConfigLoad 内部自持锁，这里不能再包着它；加载后上锁返回，避免与
  // ConfigSave 的写并发。惰性双载无害（结果一致）。
  if (!g_loaded) ConfigLoad();
  std::lock_guard<std::mutex> lock(g_modeMutex);
  return g_config;
}

Config ConfigSnapshot(void) {
  if (!g_loaded) ConfigLoad();
  std::lock_guard<std::mutex> lock(g_modeMutex);
  return g_config;
}

int ConfigNetImageMode(void) {
  if (!g_loaded) ConfigLoad();
  std::lock_guard<std::mutex> lock(g_modeMutex);
  return g_config.netImageMode;
}

bool ConfigAllowOutsideImages(void) {
  if (!g_loaded) ConfigLoad();
  std::lock_guard<std::mutex> lock(g_modeMutex);
  return g_config.allowOutsideImages;
}

bool ConfigPrewarm(void) {
  if (!g_loaded) ConfigLoad();
  std::lock_guard<std::mutex> lock(g_modeMutex);
  return g_config.prewarm;
}

int ConfigIdleReleaseMinutes(void) {
  if (!g_loaded) ConfigLoad();
  std::lock_guard<std::mutex> lock(g_modeMutex);
  return g_config.idleReleaseMinutes;
}

Config ConfigDefaults(void) {
  Config c;
  SetDefaults(&c);
  return c;
}

bool ConfigSave(const Config& input, ConfigSaveFailure* failure) {
  if (failure) {
    failure->code = 0;
    failure->path.clear();
  }

  Config c = input;
  if (c.netImageMode < 0 || c.netImageMode > 2) c.netImageMode = 1;
  if (c.font.ui.empty()) c.font.ui = DefaultUi();
  if (c.font.mono.empty()) c.font.mono = DefaultMono();
  c.font.sizePx = ClampInt(c.font.sizePx, 8, 48, 15);
  c.font.lineHeight = ClampDouble(c.font.lineHeight, 1.2, 2.2, 1.65);
  if (c.font.measureEm != 0) c.font.measureEm = ClampInt(c.font.measureEm, 8, 200, 40);
  c.idleReleaseMinutes = ClampInt(c.idleReleaseMinutes, 0, 1440, 3);
  if (c.tableCopyMode != 1) c.tableCopyMode = 0;

  const wchar_t* path = ConfigPath();
  if (!g_loaded) ConfigLoad();

  // 日志开关按「本次设置」生效，且必须在任何可能失败的写盘之前：
  // 否则「勾上记录日志 → 应用 → 写盘失败」这条最需要日志的路径反而什么都不记。
  // 关闭时 LogLine 依旧一个字节都不写。
  LogSetEnabled(c.enableLog);

  if (!path || !*path) {
    LogLine(L"CONFIG: 数据目录不可用，无法写盘");
    return false;
  }
  if (failure) failure->path = path;

  // 数据目录不一定存在：全新安装、或 Folder Alias 指到一个刚建的文件夹时都没有。
  // 日志关闭时没有任何代码会建它，而原子写不建父目录 —— 不补这一步，第一次
  // 保存就必然失败，且失败原因无从查起。
  const std::wstring dataDir = UtilDataDir();
  if (!dataDir.empty() && !UtilEnsureDir(dataDir)) {
    const DWORD err = ::GetLastError();
    if (failure) failure->code = err ? err : ERROR_ACCESS_DENIED;
    LogLine(L"CONFIG: 建目录失败 %s err=%lu", dataDir.c_str(), err);
    return false;
  }

  // 读旧文件并保留未知键，避免设置一次就把将来的配置项抹掉
  nlohmann::json root = nlohmann::json::object();
  std::string existing;
  if (UtilReadFileBytes(path, kMaxConfigBytes, &existing)) {
    try {
      nlohmann::json parsed = nlohmann::json::parse(existing);
      if (parsed.is_object()) root = std::move(parsed);
    } catch (const std::exception&) {
      // 旧文件坏了就按已知字段重写
    }
  }

  root["version"] = 1;
  root["skin"] = WideToUtf8(c.skin);
  nlohmann::json font = root.contains("font") && root["font"].is_object()
                            ? root["font"]
                            : nlohmann::json::object();
  font["ui"] = StackToJson(c.font.ui);
  font["body"] = StackToJson(c.font.body);
  font["mono"] = StackToJson(c.font.mono);
  font["size"] = c.font.sizePx;
  font["lineHeight"] = c.font.lineHeight;
  font["measure"] = c.font.measureEm;
  root["font"] = font;
  root["netImageConfirm"] =
      c.netImageMode == 0 ? "off" : (c.netImageMode == 2 ? "doc" : "host");
  root["allowOutsideImages"] = c.allowOutsideImages;
  root["enableLog"] = c.enableLog;
  root["webviewBaseDir"] = WideToUtf8(c.webviewBaseDir);
  root["prewarm"] = c.prewarm;
  root["idleReleaseMinutes"] = c.idleReleaseMinutes;
  nlohmann::json render;
  render["mermaid"] = c.renderMermaid;
  render["katex"] = c.renderKatex;
  render["highlight"] = c.renderHighlight;
  render["inlineHtml"] = c.renderInlineHtml;
  render["localImages"] = c.renderLocalImages;
  render["copyCodeBlock"] = c.copyCodeBlock;
  render["copyTable"] = c.copyTable;
  render["lineNumbers"] = c.codeLineNumbers;
  render["showFindButton"] = c.showFindButton;
  render["showOutlineButton"] = c.showOutlineButton;
  root["render"] = render;
  root["tableCopyFormat"] = c.tableCopyMode == 1 ? "markdown" : "tsv";

  const std::string bytes = root.dump(2);
  DWORD err = 0;
  if (!WriteConfigAtomically(path, bytes, &err)) {
    if (failure) failure->code = err;
    LogLine(L"CONFIG: 写入失败 %s err=%lu", path, err);
    return false;  // 内存保持旧值：磁盘没写成功，读者就不该看到新设置
  }

  {
    std::lock_guard<std::mutex> lock(g_modeMutex);
    g_config = c;
  }
  LogLine(L"CONFIG: 已写入 %s skin=\"%s\" size=%dpx lh=%.2f measure=%dem "
          L"net=%d outside=%d render=%d%d%d%d%d copy=%d%d/%d lines=%d tools=%d%d log=%d "
          L"prewarm=%d idle=%d",
          path, c.skin.c_str(), c.font.sizePx, c.font.lineHeight, c.font.measureEm,
          c.netImageMode, c.allowOutsideImages ? 1 : 0, c.renderMermaid ? 1 : 0,
          c.renderKatex ? 1 : 0, c.renderHighlight ? 1 : 0, c.renderInlineHtml ? 1 : 0,
          c.renderLocalImages ? 1 : 0, c.copyCodeBlock ? 1 : 0, c.copyTable ? 1 : 0,
          c.codeLineNumbers ? 1 : 0, c.tableCopyMode, c.showFindButton ? 1 : 0,
          c.showOutlineButton ? 1 : 0, c.enableLog ? 1 : 0, c.prewarm ? 1 : 0,
          c.idleReleaseMinutes);
  return true;
}
