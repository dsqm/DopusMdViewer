// config.h - 插件自己的 config.json（票 09：不走 Opus 的配置存储，因此也不导出
// DVP_USBSafe）。
//
// 只有 config.cpp 允许 include nlohmann/json；其余模块只看到这个纯 std 结构。
// 现在只会读，写盘要等设置对话框（M6）。

#pragma once

#include <string>
#include <vector>

// 一条有序回退链（票 06：每槽一条链，Chromium 逐字符回退，不按脚本分组）。
using FontStack = std::vector<std::wstring>;

struct FontSettings {
  FontStack ui;
  FontStack body;  // 空 = 继承 ui（Obsidian 形态），写出时解析成 ui 的实际值
  FontStack mono;
  int sizePx;          // 字号，px
  double lineHeight;   // 行高倍数
  int measureEm;       // 行宽上限，em；0 = 不限宽
};

struct Config {
  std::wstring skin;  // 皮肤 id（文件名去扩展名）
  FontSettings font;
};

// config.json 的绝对路径：<数据目录>\config.json
const wchar_t* ConfigPath(void);

// 读一次并缓存。文件缺失或损坏时保持默认值，不弹窗、不写回。
const Config& ConfigLoad(void);

// 当前生效的配置（未调用 ConfigLoad 时先加载）。
const Config& ConfigGet(void);
