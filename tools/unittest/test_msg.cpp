// test_msg.cpp - C++ ↔ 页面控制消息的格式测试。
//
// 帧格式是「类型 + 竖线分隔字段 + 载荷放最后」；前端按下标取字段，
// 所以这里逐字符锁格式，改动格式必须同步改前端时一眼能看出来。

#include "config.h"
#include "msg.h"

#include "test.h"

using namespace unittest;

namespace {

FontSettings TestFont(void) {
  FontSettings f;
  f.ui = {L"Segoe UI", L"Microsoft YaHei UI"};
  f.body.clear();  // 继承 ui
  f.mono = {L"Consolas"};
  f.sizePx = 15;
  f.lineHeight = 1.65;
  f.measureEm = 40;
  return f;
}

}  // namespace

TEST(msg, font_stack_to_css) {
  CHECK_EQ_STR(msg::FontStackToCss({L"Arial", L"Consolas"}), L"Arial, Consolas");
  // 含空格的族名加引号
  CHECK_EQ_STR(msg::FontStackToCss({L"Microsoft YaHei UI"}), L"\"Microsoft YaHei UI\"");
  // 危险字符掐掉（族名里本不该有）
  CHECK_EQ_STR(msg::FontStackToCss({L"a;b\"c"}), L"abc");
  CHECK_EQ_STR(msg::FontStackToCss({L"a|b"}), L"ab");  // 竖线会破帧格式
  CHECK_EQ_STR(msg::FontStackToCss({}), L"");
  CHECK_EQ_STR(msg::FontStackToCss({L""}), L"");
}

TEST(msg, make_fx) {
  FontSettings f = TestFont();
  // 含空格的族名逐个加引号（"Segoe UI" 也不例外）
  // fx|<ui>|<body>|<mono>|<size>|<lineHeight>|<measureEm>
  CHECK_EQ_STR(msg::MakeFx(f),
               L"fx|\"Segoe UI\", \"Microsoft YaHei UI\""
               L"|\"Segoe UI\", \"Microsoft YaHei UI\"|Consolas|15|1.65|40");
  // body 非空时写自己的链
  f.body = {L"Georgia"};
  CHECK_EQ_STR(msg::MakeFx(f),
               L"fx|\"Segoe UI\", \"Microsoft YaHei UI\"|Georgia|Consolas|15|1.65|40");
}

TEST(msg, make_cfg_field_order) {
  Config c = ConfigDefaults();
  c.renderMermaid = false;
  c.renderKatex = false;
  c.renderHighlight = false;
  c.renderInlineHtml = false;
  c.renderLocalImages = false;
  c.copyCodeBlock = false;
  c.copyTable = false;
  c.tableCopyMode = 1;
  c.showFindButton = false;
  c.showOutlineButton = false;
  c.codeLineNumbers = false;
  c.codeWsOn = true;
  c.codeWsRange = 3;
  c.codeIndentedBlocks = false;
  c.renderTableWrap = false;
  // cfg 后 14 个字段与 msg.h 注释一一对应
  CHECK_EQ_STR(msg::MakeCfg(c),
               L"cfg|0|0|0|0|0|0|0|1|0|0|0|3|0|0");

  c.codeWsOn = false;  // 关掉整个功能时档位合成 0
  CHECK_EQ_STR(msg::MakeCfg(c), L"cfg|0|0|0|0|0|0|0|1|0|0|0|0|0|0");
}

TEST(msg, make_misc) {
  CHECK_EQ_STR(msg::MakeClear(), L"clear");
  CHECK_EQ_STR(msg::MakeNetMode(2), L"net|2");
  CHECK_EQ_STR(msg::MakeIndexFull(120, 118), L"index-full|120|118");
  CHECK_EQ_STR(msg::MakeSkin(L"dark", L"body{}"), L"skin|dark|body{}");
  CHECK_EQ_STR(msg::MakeBig(std::string("3:2500")), L"big|3:2500");
  // docerror 的文本是最后一个字段：竖线原样保留
  CHECK_EQ_STR(msg::MakeDocError(L"a|b"), L"docerror|a|b");

  // index 帧的 10 个字段：最后两个是文档键与会话 id（网络图记忆用）
  CHECK_EQ_STR(msg::MakeIndex(L"C:\\doc", 10, 20, 4096, 0, 18, true, L"c:\\a.md*1*2", L"99-ABC"),
               L"index|C:\\doc|10|20|4096|0|18|1|c:\\a.md*1*2|99-ABC");
  CHECK_EQ_STR(msg::MakeIndex(L"", 0, 0, 0, 3, 0, false, L"", L""),
               L"index||0|0|0|3|0|0||");
}

TEST(msg, split_type) {
  std::wstring type;
  std::wstring rest;
  REQUIRE(msg::SplitType(L"event|rendered|12.3", &type, &rest));
  CHECK_EQ_STR(type, L"event");
  CHECK_EQ_STR(rest, L"rendered|12.3");
  CHECK_EQ_STR(msg::SecondField(rest), L"rendered");

  // 没有竖线：整帧是类型，rest 为空；空帧失败
  CHECK(msg::SplitType(L"clear", &type, &rest) && type == L"clear" && rest.empty());
  CHECK(!msg::SplitType(L"", &type, &rest));
  // 首个竖线前的才是类型
  CHECK(msg::SplitType(L"|x", &type, &rest) && type.empty() && rest == L"x");
  // out 参数可为空
  CHECK(msg::SplitType(L"cfg|1", nullptr, nullptr));
}
