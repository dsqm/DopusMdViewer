// test_doc_regex.cpp - PCRE2 页内搜索（doc_regex）的单元测试。
//
// 匹配跑在源文本上：UTF-8 用 8 位引擎，Legacy 先解码成 UTF-16 再匹配，
// 命中偏移一律是源文件字节偏移。UTF-8 路径锁几何，Legacy 路径锁「解码后
// 还能映射回源字节」；LiteralNeedsUnicodeFold 锁路由判定。

#include "doc_regex.h"

#include <string>

#include "doc_index.h"
#include "test.h"

using namespace unittest;

namespace {

std::wstring TempFile(const char* tag, const std::string& bytes) {
  const std::wstring dir = MakeTempDir(tag);
  const std::wstring path = dir + L"\\doc.md";
  CHECK_MSG(WriteBytes(path, bytes), "temp file write failed");
  return path;
}

doc::EncodingInfo Utf8Enc(uint16_t bom = 0) {
  doc::EncodingInfo info;
  info.encoding = doc::Encoding::Utf8;
  info.bomBytes = bom;
  return info;
}

doc::EncodingInfo LegacyEnc(uint16_t cp) {
  doc::EncodingInfo info;
  info.encoding = doc::Encoding::Legacy;
  info.codepage = cp;
  return info;
}

}  // namespace

TEST(regex, literal_needs_unicode_fold) {
  // 纯 ASCII / 纯中日韩：走快路径
  CHECK(!doc::LiteralNeedsUnicodeFold(L"hello"));
  CHECK(!doc::LiteralNeedsUnicodeFold(L"\u4e2d\u6587"));
  CHECK(!doc::LiteralNeedsUnicodeFold(L"abc123_\u3042"));
  CHECK(!doc::LiteralNeedsUnicodeFold(L""));
  // 带大小写变体的非 ASCII：必须走 PCRE2
  CHECK(doc::LiteralNeedsUnicodeFold(L"caf\u00e9"));
  CHECK(doc::LiteralNeedsUnicodeFold(L"\u00c9L\u00c8VE"));
  CHECK(doc::LiteralNeedsUnicodeFold(L"\u0430\u0431\u0432"));  // 西里尔
}

TEST(regex, utf8_basic_offsets) {
  const std::wstring path = TempFile("regex", "aa 12 bb 345");
  doc::RegexHits hits;
  std::wstring err;
  REQUIRE(doc::RegexSearchFile(path, Utf8Enc(), L"\\d+", true, nullptr, &hits, &err));
  CHECK(err.empty());
  CHECK_EQ_INT(static_cast<long long>(hits.total), 2);
  REQUIRE(hits.offsets.size() == 2);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[0]), 3);   // "12"
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[1]), 9);   // "345"
  CHECK(!hits.truncated && !hits.limited);
}

TEST(regex, utf8_caseless_and_multiline) {
  const std::wstring path = TempFile("regex", "ABC abc\none\ntwo\nthree");
  doc::RegexHits hits;
  std::wstring err;
  REQUIRE(doc::RegexSearchFile(path, Utf8Enc(), L"abc", false, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 2);

  hits = doc::RegexHits();
  REQUIRE(doc::RegexSearchFile(path, Utf8Enc(), L"^t", true, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 2);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[0]), 12);  // "two"
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[1]), 16);  // "three"
}

TEST(regex, utf8_bom_skipped) {
  // BOM 不算文本：^ 命中的是第一个真实行首
  const std::wstring path = TempFile("regex", std::string("\xef\xbb\xbf") + "abc\nbcd");
  doc::RegexHits hits;
  std::wstring err;
  REQUIRE(doc::RegexSearchFile(path, Utf8Enc(3), L"^b", true, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 1);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[0]), 7);
}

TEST(regex, utf8_unicode_pattern) {
  const std::string bytes("a\xe4\xb8\xad" "b\xe4\xb8\xad" "c");  // a中b中c
  const std::wstring path = TempFile("regex", bytes);
  doc::RegexHits hits;
  std::wstring err;
  REQUIRE(doc::RegexSearchFile(path, Utf8Enc(), L"\u4e2d", true, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 2);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[0]), 1);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[1]), 5);
}

TEST(regex, utf8_cap_and_truncated) {
  // 20000 个单字符：offsets 截到 10000，total 保留真实数
  const std::wstring path = TempFile("regex", std::string(20000, 'x'));
  doc::RegexHits hits;
  std::wstring err;
  REQUIRE(doc::RegexSearchFile(path, Utf8Enc(), L"x", true, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 20000);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets.size()), 10000);
  CHECK(hits.truncated);
}

TEST(regex, invalid_pattern) {
  const std::wstring path = TempFile("regex", "abc");
  doc::RegexHits hits;
  std::wstring err;
  CHECK(!doc::RegexSearchFile(path, Utf8Enc(), L"[a-", true, nullptr, &hits, &err));
  CHECK(!err.empty());  // 面向用户的原因（设置页直出这句）
}

TEST(regex, missing_file) {
  doc::RegexHits hits;
  std::wstring err;
  CHECK(!doc::RegexSearchFile(TempRoot() + L"\\no-such.md", Utf8Enc(), L"a", true, nullptr,
                              &hits, &err));
  CHECK(!err.empty());
}

TEST(regex, literal_gb18030_fold) {
  // GB18030 源 + 大小写折叠：é 按双字节编码，命中起点是 ASCII 区，字节偏移无歧义
  const std::string bytes = "this caf" "\xa8\xa6" " there";
  const std::wstring path = TempFile("regex", bytes);

  doc::EncodingInfo enc = LegacyEnc(54936);
  doc::RegexHits hits;
  std::wstring err;
  REQUIRE(doc::LiteralSearchFile(path, enc, false, L"caf\u00e9", false, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 1);
  REQUIRE(hits.offsets.size() == 1);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[0]), 5);

  // 大小写敏感：源里是小写 café，大写 needle 不命中
  hits = doc::RegexHits();
  REQUIRE(doc::LiteralSearchFile(path, enc, true, L"Caf\u00e9", false, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 0);
}

TEST(regex, literal_whole_word) {
  const std::wstring path = TempFile("regex", "cat cat2 xcat cat.");
  doc::EncodingInfo enc = Utf8Enc();
  doc::RegexHits hits;
  std::wstring err;
  REQUIRE(doc::LiteralSearchFile(path, enc, true, L"cat", true, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 2);  // 开头与 "cat."
  REQUIRE(hits.offsets.size() == 2);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[0]), 0);
  CHECK_EQ_INT(static_cast<long long>(hits.offsets[1]), 14);  // "cat."（xcat@9 的 cat 不算）
}

TEST(regex, literal_empty_needle) {
  const std::wstring path = TempFile("regex", "abc");
  doc::RegexHits hits;
  std::wstring err;
  CHECK(doc::LiteralSearchFile(path, Utf8Enc(), true, L"", false, nullptr, &hits, &err));
  CHECK_EQ_INT(static_cast<long long>(hits.total), 0);
  CHECK(!doc::LiteralSearchFile(TempRoot() + L"\\no-such.md", Utf8Enc(), true, L"a", false,
                                nullptr, &hits, &err));
}
