// config.h - 插件自己的 config.json（不走 Opus 的配置存储，因此也不导出
// DVP_USBSafe）。
//
// 只有 config.cpp 允许 include nlohmann/json；其余模块只看到这个纯 std 结构。

#pragma once

#include <string>
#include <vector>

// 一条有序回退链（每槽一条链，Chromium 逐字符回退，不按脚本分组）。
using FontStack = std::vector<std::wstring>;

struct FontSettings {
  FontStack ui;
  FontStack body;  // 空 = 继承 ui（Obsidian 形态）；落盘原样写空数组，推给页面之前由
                   // msg::MakeFx 解析成 ui 的实际值
  FontStack mono;
  int sizePx;          // 字号，px
  double lineHeight;   // 行高倍数
  int measureEm;       // 行宽上限，em；0 = 不限宽
};

struct Config {
  std::wstring skin;  // 皮肤 id（文件名去扩展名）
  FontSettings font;
  // 网络图片的确认粒度（两个档位的答复都只对当前文档有效，换文件全部重问）：
  //   0 = 不确认直接加载；
  //   1 = 每主机一次：一篇里每个主机各问一次，各自记各自的；
  //   2 = 每文档一次：整篇只问一次，一个答复管所有来源。
  int netImageMode;
  // 图片是否允许引用文档目录之外的路径（../ 与绝对路径）。默认关闭。
  bool allowOutsideImages;
  // 渲染开关（设置里的“渲染”页）。默认全开。
  bool renderMermaid;
  bool renderKatex;
  bool renderHighlight;
  // 界面叫「渲染原始 HTML」；JSON 键沿用 render.inlineHtml，正名会让已有配置
  // 里那一项静默回落默认值。
  bool renderInlineHtml;
  bool renderLocalImages;
  // 表格长内容自动换行。默认开；关掉后单元格保持单行，表格在正文栏内横向滚动。
  bool renderTableWrap;
  // 代码块 / 表格右上角的复制按钮。默认开。
  bool copyCodeBlock;
  bool copyTable;
  // 代码块左侧的行号列。默认开。
  bool codeLineNumbers;
  // 代码块里的空白符标记（空格画点、制表符画箭头）。
  // codeWsOn 关掉整个功能；codeWsRange 决定范围，按常显的多少递增：
  //   1 = 仅选中：一个都不常显，只有选中的部分显示；
  //   2 = 边界：行首缩进、行尾空白、行内连续 2 个以上的空格；
  //   3 = 全部：所有空格与制表符。
  // 仅选中档与边界档在有选区时，选区内的空白按「全部」显示。
  bool codeWsOn;
  int codeWsRange;
  // 缩进式代码块（4 空格缩进、非围栏）也套用行号与空白符。默认关。
  bool codeIndentedBlocks;
  // 表格复制格式：0 = TSV 纯文本，1 = Markdown 源码。
  int tableCopyMode;
  // 右上角工具条上的两个按钮。默认开；两个都关掉时整条工具条不显示。
  bool showFindButton;
  bool showOutlineButton;
  // 记录诊断日志。默认关闭。
  bool enableLog;
  // WebView2 缓存的父目录；空 = %LOCALAPPDATA%\DopusMdViewer。实际目录是 <此项>\WebView2。
  std::wstring webviewBaseDir;
  // 页面里的本地文件链接交给 Opus 独立查看器（dopusrt /show）打开，而不是系统
  // 默认程序。默认开；目录链接不受影响（仍走系统），Opus 查看器不支持的类型也回退。
  bool openLinksInOpusViewer;
  // 交给 Opus 查看器的额外扩展名：分号/逗号/空格分隔，可带点可不带；
  // `-` 前缀表示排除（能把内置名单里的项摘掉）。空 = 只用内置名单。
  std::wstring viewerExts;
  // 启动后在后台预热 WebView2（Opus 窗口关闭时释放）。默认关闭。
  bool prewarm;
  // 预热关闭时，最后一次预览结束后空闲多少分钟释放 WebView2；0 = 不释放。
  int idleReleaseMinutes;
};

// 内置默认值（设置界面的“恢复默认”与外置 JSON 缺失时共用）。
Config ConfigDefaults(void);

// config.json 的绝对路径：<数据目录>\config.json
const wchar_t* ConfigPath(void);

// 读一次并缓存。文件缺失或损坏时保持默认值，不弹窗、不写回。
const Config& ConfigLoad(void);

// 当前生效的配置（未调用 ConfigLoad 时先加载）。给的是内部结构的引用：
// 锁只保护「取到引用」那一刻，拿住之后别的线程仍可能改写同一份结构，
// 所以只能当场用完就丢——跨线程一律用 ConfigSnapshot()。
const Config& ConfigGet(void);

// 线程安全的配置快照（拷贝）。查看器线程推皮肤/字体时用它，避免与写盘线程并发读结构。
Config ConfigSnapshot(void);

// 网络图片确认粒度（线程安全）。设置对话框改它，查看器线程读它。
int ConfigNetImageMode(void);

// 是否允许图片引用文档目录外的路径（../ 与绝对路径）。
bool ConfigAllowOutsideImages(void);

// 启动时是否预热 WebView2。
bool ConfigPrewarm(void);

// 预热关闭时的空闲释放分钟数；0 = 不释放。
int ConfigIdleReleaseMinutes(void);

// ConfigSave 失败时的现场：给设置页拼弹窗用，不落库。
struct ConfigSaveFailure {
  // 失败处的 Win32 错误码（DWORD 的等宽整数，免得本头文件拖进 windows.h）；
  // 0 = 没有可给的系统错误（数据目录整个不可用）。
  unsigned long code;
  // 目标 config.json 的完整路径；空 = 路径本身不可用。
  std::wstring path;
};

// 写入整份设置，成功后才更新内存值；原子写（.tmp + MoveFileEx），保留未知键。
// 数据目录不存在时先建出来。失败时把原因填进 failure（可为 nullptr），
// 内存配置保持旧值，日志开关按本次设置生效（不回滚）。
bool ConfigSave(const Config& config, ConfigSaveFailure* failure = nullptr);
