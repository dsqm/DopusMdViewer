// test_config.cpp - config.json 读写链路的单元测试。
//
// 依赖进程级隔离（main 已把 APPDATA 指到 %TEMP%\mdv-unittest-<pid>\appdata），
// 所有读写都发生在隔离区。ConfigLoad 是单例缓存，进程内只发生一次，所以
// 「文件缺失 → 写盘 → 落盘内容」按固定顺序串进测试；解析越界/坏 JSON 的
// 回退路径在单进程里没法重载，留给 hosttest 与真机（见票内说明）。

#include "config.h"

#include <string>

#include "test.h"
#include "util.h"

using namespace unittest;

namespace {

// 落盘 JSON 的子串断言（root.dump(2) 的键值格式固定为 "key": value）。
bool JsonHas(const std::string& bytes, const char* needle) {
  return bytes.find(needle) != std::string::npos;
}

}  // namespace

TEST(config, defaults) {
  const Config c = ConfigDefaults();
  CHECK_EQ_STR(c.skin.c_str(), L"light");
  CHECK_EQ_INT(c.font.sizePx, 15);
  CHECK(c.font.lineHeight == 1.65);
  CHECK_EQ_INT(c.font.measureEm, 40);
  CHECK(c.font.ui.size() == 4 && c.font.ui[0] == L"Segoe UI");
  CHECK(c.font.body.empty());  // 空 = 继承界面
  CHECK(c.font.mono.size() == 4 && c.font.mono[0] == L"Cascadia Mono");
  CHECK_EQ_INT(c.netImageMode, 1);  // 每主机一次
  CHECK(!c.allowParentImages && !c.allowAbsoluteImages);  // 目录外图片默认都关
  CHECK(c.renderMermaid && c.renderKatex && c.renderHighlight && c.renderInlineHtml);
  CHECK(c.renderLocalImages && c.renderTableWrap);
  CHECK(c.copyCodeBlock && c.copyTable);
  CHECK(c.codeLineNumbers);
  CHECK(c.codeWsOn && c.codeWsRange == 2);
  CHECK(!c.codeIndentedBlocks);
  CHECK_EQ_INT(c.tableCopyMode, 0);
  CHECK(c.showFindButton && c.showOutlineButton);
  CHECK(!c.enableLog);
  CHECK(c.openLinksInOpusViewer);  // 链接默认交给 Opus 查看器
  CHECK(c.viewerExts.empty());     // 额外名单默认空，只用内置
  CHECK(c.linkConfirmWait);        // 外部链接默认要等 3 秒才能点打开
  CHECK(c.webviewBaseDir.empty());
  CHECK(!c.prewarm);
  CHECK_EQ_INT(c.idleReleaseMinutes, 3);
}

// 前提：隔离 APPDATA 里还没有 config.json。本测试触发进程内唯一一次
// ConfigLoad；后续测试都建立在这次加载之后。
TEST(config, load_missing_file) {
  const Config& c = ConfigLoad();
  const Config d = ConfigDefaults();
  CHECK_EQ_INT(c.font.sizePx, d.font.sizePx);
  CHECK_EQ_INT(c.netImageMode, d.netImageMode);
  CHECK(c.renderTableWrap == d.renderTableWrap);
  CHECK_EQ_STR(c.skin.c_str(), L"light");
}

// ConfigSave 后内存立即生效，落盘内容与结构一致，且未知键保留。
TEST(config, save_roundtrip) {
  const std::wstring dataDir = UtilDataDir();
  REQUIRE(!dataDir.empty());
  const wchar_t* pathPtr = ConfigPath();
  REQUIRE(pathPtr && *pathPtr);
  const std::wstring path(pathPtr);
  REQUIRE(WriteBytes(path, std::string("{\"futureKey\": 42}")));

  Config c = ConfigDefaults();
  c.skin = L"dark";
  c.font.sizePx = 20;
  c.font.lineHeight = 1.8;
  c.font.measureEm = 0;  // 不限宽
  c.font.ui = {L"Arial", L"Microsoft YaHei UI"};
  c.font.body = {L"Source Han Serif"};  // 非空：落盘成自己的数组
  c.netImageMode = 2;
  c.allowParentImages = true;
  c.allowAbsoluteImages = false;
  c.renderMermaid = false;
  c.renderTableWrap = false;
  c.copyTable = false;
  c.codeWsOn = false;
  c.tableCopyMode = 1;
  c.showFindButton = false;
  c.enableLog = false;  // 不产生日志文件
  c.openLinksInOpusViewer = false;
  c.viewerExts = L".epub;-.html";
  c.webviewBaseDir = L"D:\\cache";
  c.prewarm = true;
  c.idleReleaseMinutes = 30;

  REQUIRE(ConfigSave(c));

  // 内存立即生效
  const Config now = ConfigSnapshot();
  CHECK_EQ_STR(now.skin.c_str(), L"dark");
  CHECK_EQ_INT(now.font.sizePx, 20);
  CHECK(now.prewarm);
  CHECK_EQ_STR(now.viewerExts.c_str(), L".epub;-.html");
  CHECK_EQ_INT(now.netImageMode, 2);

  // 落盘内容：自定义值、布尔与枚举字符串、未知键三路都验到
  std::string bytes;
  REQUIRE(UtilReadFileBytes(path, 256 * 1024, &bytes));
  CHECK(JsonHas(bytes, "\"futureKey\": 42"));
  CHECK(JsonHas(bytes, "\"skin\": \"dark\""));
  CHECK(JsonHas(bytes, "\"size\": 20"));
  CHECK(JsonHas(bytes, "\"measure\": 0"));
  CHECK(JsonHas(bytes, "\"netImageConfirm\": \"doc\""));
  CHECK(JsonHas(bytes, "\"tableWrap\": false"));
  CHECK(JsonHas(bytes, "\"copyTable\": false"));
  CHECK(JsonHas(bytes, "\"codeWhitespace\": false"));
  CHECK(JsonHas(bytes, "\"tableCopyFormat\": \"markdown\""));
  CHECK(JsonHas(bytes, "\"webviewBaseDir\": \"D:\\\\cache\""));
  CHECK(JsonHas(bytes, "\"openLinksInOpusViewer\": false"));
  CHECK(JsonHas(bytes, "\"viewerExts\": \".epub;-.html\""));
  // 目录外图片拆成两个键，同时留一个旧版能读的总开关
  CHECK(JsonHas(bytes, "\"allowParentImages\": true"));
  CHECK(JsonHas(bytes, "\"allowAbsoluteImages\": false"));
  CHECK(JsonHas(bytes, "\"allowOutsideImages\": true"));
  CHECK(JsonHas(bytes, "\"prewarm\": true"));
  CHECK(JsonHas(bytes, "\"idleReleaseMinutes\": 30"));
  CHECK(JsonHas(bytes, "\"version\": 1"));
  // body 槽非空时写自己的数组，不解析成 ui（不去猜 dump 的缩进，只认内容）
  CHECK(JsonHas(bytes, "\"Source Han Serif\""));
  CHECK(!JsonHas(bytes, "\"body\": []"));
}

// ConfigSave 的收敛：越界值一律回默认，不写进文件。
TEST(config, save_clamps) {
  Config c = ConfigDefaults();
  c.netImageMode = 7;            // → 1（host）
  c.font.sizePx = 999;           // → 15
  c.font.lineHeight = 9.0;       // → 1.65
  c.font.measureEm = -3;         // → 40（0 是合法的"不限宽"，负数不是）
  c.idleReleaseMinutes = -5;     // → 3
  c.tableCopyMode = 9;           // → 0（tsv）
  c.font.ui.clear();             // → 默认链
  c.font.mono.clear();           // → 默认链

  REQUIRE(ConfigSave(c));

  const Config now = ConfigSnapshot();
  CHECK_EQ_INT(now.netImageMode, 1);
  CHECK_EQ_INT(now.font.sizePx, 15);
  CHECK(now.font.lineHeight == 1.65);
  CHECK_EQ_INT(now.font.measureEm, 40);
  CHECK_EQ_INT(now.idleReleaseMinutes, 3);
  CHECK_EQ_INT(now.tableCopyMode, 0);
  CHECK(now.font.ui.size() == 4 && now.font.ui[0] == L"Segoe UI");
  CHECK(now.font.mono.size() == 4 && now.font.mono[0] == L"Cascadia Mono");

  std::string bytes;
  REQUIRE(UtilReadFileBytes(ConfigPath(), 256 * 1024, &bytes));
  CHECK(JsonHas(bytes, "\"netImageConfirm\": \"host\""));
  CHECK(JsonHas(bytes, "\"size\": 15"));
  CHECK(JsonHas(bytes, "\"tableCopyFormat\": \"tsv\""));
}
