// config.cpp - config.json 的读写。**唯一 include nlohmann/json 的编译单元**
// （票 11：单头两万多行，扩散出去每个 TU 多付 2–4 秒编译）。

#include "config.h"

#include <windows.h>

#include <exception>

#include <json/json.hpp>  // build.bat 把 %PROJ%third_party 放进 INCLUDE

#include "log.h"
#include "util.h"

namespace {

const size_t kMaxConfigBytes = 256 * 1024;

Config g_config;
bool g_loaded = false;

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
}

int ClampInt(int v, int lo, int hi, int fallback) {
  if (v < lo || v > hi) return fallback;
  return v;
}

double ClampDouble(double v, double lo, double hi, double fallback) {
  if (!(v >= lo && v <= hi)) return fallback;  // 同时挡住 NaN
  return v;
}

FontStack ReadStack(const nlohmann::json& obj, const char* key, const FontStack& fallback) {
  if (!obj.contains(key) || !obj[key].is_array()) return fallback;

  FontStack out;
  for (const auto& item : obj[key]) {
    if (!item.is_string()) continue;
    std::string name = item.get<std::string>();
    if (name.empty()) continue;
    out.push_back(std::wstring(name.begin(), name.end()));
  }
  return out.empty() ? fallback : out;
}

void ApplyJson(const nlohmann::json& root, Config* c) {
  if (root.contains("skin") && root["skin"].is_string()) {
    const std::string id = root["skin"].get<std::string>();
    if (!id.empty()) c->skin.assign(id.begin(), id.end());
  }

  if (!root.contains("font") || !root["font"].is_object()) return;
  const nlohmann::json& font = root["font"];

  c->font.ui = ReadStack(font, "ui", c->font.ui);
  c->font.mono = ReadStack(font, "mono", c->font.mono);
  c->font.body = ReadStack(font, "body", FontStack());  // 空数组就是"继承界面"

  if (font.contains("size") && font["size"].is_number_integer()) {
    c->font.sizePx = ClampInt(font["size"].get<int>(), 9, 48, 15);
  }
  if (font.contains("lineHeight") && font["lineHeight"].is_number()) {
    c->font.lineHeight = ClampDouble(font["lineHeight"].get<double>(), 1.0, 3.0, 1.65);
  }
  if (font.contains("measure") && font["measure"].is_number_integer()) {
    const int m = font["measure"].get<int>();
    c->font.measureEm = (m == 0) ? 0 : ClampInt(m, 20, 200, 40);
  }
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
  SetDefaults(&g_config);
  g_loaded = true;

  const wchar_t* path = ConfigPath();
  if (!path || !*path) {
    LogLine(L"CONFIG: 数据目录不可用，全部用默认值");
    return g_config;
  }

  std::string bytes;
  if (!UtilReadFileBytes(path, kMaxConfigBytes, &bytes)) {
    LogLine(L"CONFIG: 没有 %s，用默认值", path);
    return g_config;
  }

  try {
    const nlohmann::json root = nlohmann::json::parse(bytes);
    if (!root.is_object()) {
      LogLine(L"CONFIG: 顶层不是对象，用默认值");
      return g_config;
    }
    ApplyJson(root, &g_config);
    LogLine(L"CONFIG: 已读取 %s skin=\"%s\" size=%dpx lh=%.2f measure=%dem", path,
            g_config.skin.c_str(), g_config.font.sizePx, g_config.font.lineHeight,
            g_config.font.measureEm);
  } catch (const std::exception& e) {
    // 读坏就用默认值，不打扰用户（票 09）
    LogLine(L"CONFIG: 解析失败（%hs），用默认值", e.what());
  }

  return g_config;
}

const Config& ConfigGet(void) {
  if (!g_loaded) return ConfigLoad();
  return g_config;
}
