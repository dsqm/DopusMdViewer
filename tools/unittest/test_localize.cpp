// test_localize.cpp - 文案表（localize.cpp）的单元测试。
//
// 表用 index 直达，「漏配时返回空串」——那就把每个 id 都摸一遍，
// 空串即漏配。格式串（%s / %lu）是设置页拼句子的契约，也一并锁住。

#include "localize.h"

#include "test.h"

using namespace unittest;

namespace {

// 数 %s 的出现次数（%lu 不算 %s）。
int CountSubstr(const std::wstring& text, const wchar_t* needle) {
  int n = 0;
  for (size_t pos = text.find(needle); pos != std::wstring::npos;
       pos = text.find(needle, pos + 1)) {
    ++n;
  }
  return n;
}

}  // namespace

TEST(localize, every_id_has_text) {
  for (int i = 0; i <= static_cast<int>(loc::Str::OpenFolderFailed); ++i) {
    const wchar_t* text = loc::T(static_cast<loc::Str>(i));
    const bool ok = text && *text != L'\0';
    CHECK_MSG(ok, "missing entry");
    if (!ok) printf("      loc::Str id %d has no text\n", i);
  }
}

TEST(localize, known_phrases) {
  CHECK_CONTAINS(loc::T(loc::Str::SettingsTitle), L"\u8bbe\u7f6e");  // 「设置」
  CHECK_EQ_STR(loc::T(loc::Str::TabAppearance), L"\u5916\u89c2");    // 「外观」
  CHECK_EQ_STR(loc::T(loc::Str::TabFont), L"\u5b57\u4f53");          // 「字体」
}

TEST(localize, format_placeholders) {
  // PreviewMissing 带一个 %s（字体名）
  CHECK_EQ_INT(CountSubstr(loc::T(loc::Str::PreviewMissing), L"%s"), 1);
  // ConfigFolderHelp：别名名 + 当前路径
  CHECK_EQ_INT(CountSubstr(loc::T(loc::Str::ConfigFolderHelp), L"%s"), 2);
  // WebViewDirHelp：默认目录、实际目录、清理上限
  CHECK_EQ_INT(CountSubstr(loc::T(loc::Str::WebViewDirHelp), L"%s"), 3);
  // SaveFailedError：%lu（错误码）+ %s（系统消息）
  CHECK_EQ_INT(CountSubstr(loc::T(loc::Str::SaveFailedError), L"%lu"), 1);
  CHECK_EQ_INT(CountSubstr(loc::T(loc::Str::SaveFailedError), L"%s"), 1);
  // SaveFailedPath 只有一个 %s
  CHECK_EQ_INT(CountSubstr(loc::T(loc::Str::SaveFailedPath), L"%s"), 1);
}
