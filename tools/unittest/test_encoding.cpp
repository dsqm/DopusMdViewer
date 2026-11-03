// test_encoding.cpp - 判码链（encoding_detect.cpp）的单元测试。
//
// 判定顺序：4 字节 BOM → 无 BOM UTF-16 零字节启发式 → 跳过前导 ASCII 取样本 →
// 纯 ASCII / 合法 UTF-8 → CED → GB18030 兜底。CED 的返回值原则上不再锁死
// （第三方判定可能随版本微调），这里只锁 BOM / 启发式 / UTF-8 校验这几条
// 自己实现的路径，CED 路径只断言「落进 Legacy 且是预期的代码页族」。

#include "encoding_detect.h"

#include <string>

#include "doc_index.h"
#include "test.h"

using namespace unittest;

namespace {

doc::EncodingInfo Detect(const std::string& bytes) {
  return doc::DetectEncoding(bytes.data(), bytes.size());
}

}  // namespace

TEST(encoding, empty_input) {
  const doc::EncodingInfo info = doc::DetectEncoding(nullptr, 0);
  CHECK(info.encoding == doc::Encoding::Utf8);
  CHECK_EQ_INT(info.bomBytes, 0);
  CHECK_EQ_INT(info.CodePage(), 65001);
}

TEST(encoding, ascii) {
  const doc::EncodingInfo info = Detect("plain ascii text\n123\n");
  CHECK(info.encoding == doc::Encoding::Utf8);
  CHECK_EQ_INT(info.bomBytes, 0);
}

TEST(encoding, bom_utf8) {
  const doc::EncodingInfo info = Detect(std::string("\xef\xbb\xbf") + "# 你好\n");
  CHECK(info.encoding == doc::Encoding::Utf8);
  CHECK_EQ_INT(info.bomBytes, 3);
}

TEST(encoding, bom_utf16) {
  // 注意 std::string 里带 \0 时必须用长度构造，否则按 C 字符串截断
  const std::string leBytes("\xff\xfe\x61\x00\x62\x00", 6);
  const doc::EncodingInfo le = Detect(leBytes);
  CHECK(le.encoding == doc::Encoding::Utf16LE);
  CHECK_EQ_INT(le.bomBytes, 2);
  CHECK(le.IsUtf16());

  const std::string beBytes("\xfe\xff\x00\x61\x00\x62", 6);
  const doc::EncodingInfo be = Detect(beBytes);
  CHECK(be.encoding == doc::Encoding::Utf16BE);
  CHECK_EQ_INT(be.bomBytes, 2);
}

TEST(encoding, bom_utf32_not_swallowed_by_utf16) {
  // FF FE 00 00 是 UTF-32LE：必须先于 2 字节 FF FE 分支命中
  const doc::EncodingInfo le = Detect(std::string("\xff\xfe\x00\x00", 4));
  CHECK(le.encoding == doc::Encoding::Legacy);  // UTF-32 不支持，但 BOM 不能被吞
  CHECK_EQ_INT(le.bomBytes, 4);
  CHECK(doc::IsUtf32CodePage(le.CodePage()));

  const doc::EncodingInfo be = Detect(std::string("\x00\x00\xfe\xff", 4));
  CHECK(be.encoding == doc::Encoding::Legacy);
  CHECK(doc::IsUtf32CodePage(be.CodePage()));
}

TEST(encoding, utf16_nobom_heuristic) {
  // ≥64 字节、偶数位全是 ASCII（奇数位全 0）→ UTF-16LE
  std::string bytes;
  for (int i = 0; i < 100; ++i) {
    bytes += static_cast<char>('a' + (i % 26));
    bytes += '\0';
  }
  const doc::EncodingInfo info = Detect(bytes);
  CHECK(info.encoding == doc::Encoding::Utf16LE);
  CHECK_EQ_INT(info.bomBytes, 0);

  // 反过来 → UTF-16BE
  std::string be;
  for (int i = 0; i < 100; ++i) {
    be += '\0';
    be += static_cast<char>('a' + (i % 26));
  }
  CHECK(Detect(be).encoding == doc::Encoding::Utf16BE);
}

TEST(encoding, valid_utf8_no_bom) {
  const doc::EncodingInfo info = Detect(std::string("abc\xe4\xb8\xad\xe6\x96\x87"
                                                    "def\n"));
  CHECK(info.encoding == doc::Encoding::Utf8);
  CHECK_EQ_INT(info.bomBytes, 0);
}

TEST(encoding, legacy_gb18030) {
  // "中文" 的 GB18030：非合法 UTF-8 → CED → GB 系列
  std::string bytes;
  for (int i = 0; i < 8; ++i) bytes += "\xd6\xd0\xce\xc4";
  const doc::EncodingInfo info = Detect(bytes);
  CHECK(info.encoding == doc::Encoding::Legacy);
  CHECK(info.codepage == 936 || info.codepage == 54936);
}

TEST(encoding, legacy_shift_jis) {
  // "日本語" 的 Shift_JIS
  const std::string bytes("\x93\xfa\x96\x7b\x8c\xea"
                          "\x0a");
  const doc::EncodingInfo info = Detect(bytes);
  CHECK(info.encoding == doc::Encoding::Legacy);
  CHECK_EQ_INT(info.codepage, 932);
}

TEST(encoding, iso2022_stateful) {
  // ISO-2022-JP：ESC 序列，样本含 0x1B 时不能走 UTF-8 短路
  const std::string bytes("\x1b$B$3$s$K$A$O\x1b(B\n");
  const doc::EncodingInfo info = Detect(bytes);
  CHECK(info.encoding == doc::Encoding::Legacy);
  CHECK(doc::IsIso2022CodePage(info.CodePage()));
}

TEST(encoding, encoding_from_code_page) {
  doc::EncodingInfo info;
  CHECK(doc::EncodingFromCodePage(65001, &info));
  CHECK(info.encoding == doc::Encoding::Utf8);
  CHECK(doc::EncodingFromCodePage(1200, &info));
  CHECK(info.encoding == doc::Encoding::Utf16LE);
  CHECK(doc::EncodingFromCodePage(1201, &info));
  CHECK(info.encoding == doc::Encoding::Utf16BE);
  CHECK(doc::EncodingFromCodePage(936, &info));
  CHECK(info.encoding == doc::Encoding::Legacy && info.codepage == 936);
  CHECK(!doc::EncodingFromCodePage(0, &info));
  CHECK(!doc::EncodingFromCodePage(54936, nullptr));
}
