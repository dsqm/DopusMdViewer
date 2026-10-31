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
  // 网络图片的确认粒度：0 = 不确认直接加载，1 = 每主机每文档确认，2 = 每文档确认。
  int netImageMode;
  // 图片是否允许引用文档目录之外的路径（../ 与绝对路径）。默认关闭。
  bool allowOutsideImages;
};

// config.json 的绝对路径：<数据目录>\config.json
const wchar_t* ConfigPath(void);

// 读一次并缓存。文件缺失或损坏时保持默认值，不弹窗、不写回。
const Config& ConfigLoad(void);

// 当前生效的配置（未调用 ConfigLoad 时先加载）。
const Config& ConfigGet(void);

// 线程安全的配置快照（拷贝）。查看器线程推皮肤/字体时用它，避免与写盘线程并发读结构。
Config ConfigSnapshot(void);

// 网络图片确认粒度（线程安全）。设置对话框改它，查看器线程读它。
int ConfigNetImageMode(void);

// 是否允许图片引用文档目录外的路径（../ 与绝对路径）。
bool ConfigAllowOutsideImages(void);

// 一次性写入设置并更新内存值；原子写（.tmp + MoveFileEx）。
bool ConfigSaveSettings(int netImageMode, bool allowOutsideImages);
