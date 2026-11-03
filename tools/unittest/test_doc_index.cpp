// test_doc_index.cpp - 块扫描状态机与 BlockIndex 的单元测试。
//
// 关键契约（见 doc_index.h 与实现）：
//   · 块 Len 不含行尾换行（\r 也不算）；空行不产出块，只关块。
//   · Feed 的字节按文件顺序、可任意切块——不同切块必须得到同一个索引。
//   · 偏移都是源文件字节坐标（UTF-16 按单元 ×2，BOM 跳过）。
//
// 预期值全部以实现为准先推一遍再锁死；改动扫描规则时这些断言就是要一起改的口径。

#include "doc_index.h"

#include <cstring>
#include <string>

#include "test.h"

using namespace unittest;

namespace {

// 内存里喂一个 UTF-8 文档。chunk=0 表示一次喂完。
bool BuildUtf8(const std::string& bytes, size_t chunk, uint64_t limit, doc::IndexBuilder* out) {
  doc::EncodingInfo enc;  // Utf8
  if (!out->Begin(enc, bytes.size(), limit)) return false;
  if (chunk == 0) {
    out->Feed(bytes.data(), bytes.size());
  } else {
    for (size_t i = 0; i < bytes.size(); i += chunk) {
      const size_t n = (chunk < bytes.size() - i) ? chunk : bytes.size() - i;
      out->Feed(bytes.data() + i, n);
    }
  }
  out->Finish();
  return true;
}

uint64_t Fnv(const doc::BlockIndex& ix) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < ix.BlockCount(); ++i) {
    const uint64_t v[3] = {ix.Start(i), ix.Len(i), ix.KindFlags(i)};
    for (uint64_t x : v) {
      h ^= x;
      h *= 1099511628211ull;
    }
  }
  return h;
}

}  // namespace

TEST(docindex, chunk_invariance) {
  const std::string doc =
      "# Title\n"
      "\n"
      "para one\n"
      "para two\n"
      "\n"
      "## Sec\n"
      "- item\n"
      "- item2\n"
      "```js\n"
      "code()\n"
      "```\n"
      "| a | b |\n"
      "|---|---|\n"
      "| 1 | 2 |\n"
      "\n"
      "> quote\n"
      "\n"
      "***\n"
      "[ref]: http://x\n"
      "---\n"
      "## End\n";

  doc::IndexBuilder whole;
  REQUIRE(BuildUtf8(doc, 0, 0, &whole));
  const uint64_t wantHash = Fnv(whole.index());
  const uint32_t wantCount = whole.index().BlockCount();
  REQUIRE(wantCount > 10);

  const size_t kChunks[] = {1, 2, 3, 7, 64, 4096};
  for (size_t c : kChunks) {
    doc::IndexBuilder b;
    REQUIRE(BuildUtf8(doc, c, 0, &b));
    const bool sameCount = b.index().BlockCount() == wantCount;
    CHECK_MSG(sameCount, "block count differs");
    if (sameCount) {
      CHECK_EQ_INT(static_cast<long long>(Fnv(b.index())), static_cast<long long>(wantHash));
    }
  }
}

TEST(docindex, kinds_and_offsets) {
  // 与 chunk_invariance 同一篇，逐块锁种类与字节区间。
  const std::string doc =
      "# Title\n"       // 0..7    H1（len 不含 \n）
      "\n"              // 空行：关块
      "para one\n"      // 8..16   Para 起始
      "para two\n"      // 懒续行并入同一段
      "\n"
      "## Sec\n"        // H2
      "- item\n"        // 列表（两项一块）
      "- item2\n"
      "```js\n"         // Fence 三行
      "code()\n"
      "```\n"
      "| a | b |\n"     // 表头
      "|---|---|\n"     // 分隔行 → 定格为 Table
      "| 1 | 2 |\n"
      "\n"
      "> quote\n"       // Quote
      "\n"
      "***\n"           // Hr
      "[ref]: http://x\n"  // 链接引用定义：hidden Para
      "---\n"           // Hr
      "## End\n";       // H2

  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  const doc::BlockIndex& ix = b.index();
  const uint32_t n = ix.BlockCount();
  CHECK_EQ_INT(n, 12);
  if (n != 12) return;

  CHECK(ix.KindOf(0) == doc::Kind::H1);
  CHECK_EQ_INT(static_cast<long long>(ix.Start(0)), 0);
  CHECK_EQ_INT(static_cast<long long>(ix.Len(0)), 7);

  CHECK(ix.KindOf(1) == doc::Kind::Para);
  CHECK_EQ_INT(static_cast<long long>(ix.Start(1)), 9);
  // 一段覆盖 "para one\npara two"（不含最后的 \n）→ 9 + 17
  CHECK_EQ_INT(static_cast<long long>(ix.Len(1)), 17);

  CHECK(ix.KindOf(2) == doc::Kind::H2);
  // 列表每一项是独立块（同种列表项不合并）
  CHECK(ix.KindOf(3) == doc::Kind::ListItem);
  CHECK(!ix.IsLoose(3));
  CHECK(ix.KindOf(4) == doc::Kind::ListItem);
  CHECK(!ix.IsLoose(4));
  CHECK(ix.KindOf(5) == doc::Kind::Fence);
  CHECK(ix.KindOf(6) == doc::Kind::Table);
  CHECK(ix.KindOf(7) == doc::Kind::Quote);
  CHECK(ix.KindOf(8) == doc::Kind::Hr);
  CHECK(ix.KindOf(9) == doc::Kind::Para);
  CHECK(ix.IsHidden(9) && !ix.IsFootnoteDef(9));  // 链接引用定义
  CHECK(ix.KindOf(10) == doc::Kind::Hr);
  CHECK(ix.KindOf(11) == doc::Kind::H2);
}

// CRLF：\r 不算内容，块长与 LF 文件一致。
TEST(docindex, crlf) {
  const std::string doc = "# T\r\npara\r\n";
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 2);
  CHECK_EQ_INT(static_cast<long long>(ix.Len(0)), 3);  // "# T"
  CHECK_EQ_INT(static_cast<long long>(ix.Len(1)), 4);  // "para"
}

TEST(docindex, setext_heading) {
  const std::string doc = "T\n===\n\ntext\n\nB\n---\n";
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 3);
  CHECK(ix.KindOf(0) == doc::Kind::H1);
  CHECK_EQ_INT(static_cast<long long>(ix.Len(0)), 5);  // "T\n==="（不含行尾 \n）
  CHECK(ix.KindOf(1) == doc::Kind::Para);
  CHECK(ix.KindOf(2) == doc::Kind::H2);  // "B\n---"
  CHECK_EQ_INT(static_cast<long long>(ix.Len(2)), 5);

  // 大纲：Setext 也进大纲，文本区间在源字节坐标里
  REQUIRE(b.outline().size() == 2);
  CHECK_EQ_INT(b.outline()[0].level, 1);
  CHECK_EQ_INT(static_cast<long long>(b.outline()[0].start), 0);
  CHECK_EQ_INT(b.outline()[0].textOff, 0);
  CHECK_EQ_INT(b.outline()[0].textLen, 1);
  CHECK_EQ_INT(b.outline()[1].level, 2);
  CHECK_EQ_INT(static_cast<long long>(b.outline()[1].start), 13);
  CHECK_EQ_INT(b.outline()[1].textOff, 0);
  CHECK_EQ_INT(b.outline()[1].textLen, 1);
}

TEST(docindex, atx_outline_offsets) {
  const std::string doc = "# A\n\ntext\n\n## B b\n";
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  REQUIRE(b.outline().size() == 2);
  CHECK_EQ_INT(b.outline()[0].level, 1);
  CHECK_EQ_INT(static_cast<long long>(b.outline()[0].start), 0);
  CHECK_EQ_INT(b.outline()[0].textOff, 2);  // "# " 之后
  CHECK_EQ_INT(b.outline()[0].textLen, 1);
  CHECK_EQ_INT(b.outline()[1].level, 2);
  CHECK_EQ_INT(static_cast<long long>(b.outline()[1].start), 11);
  CHECK_EQ_INT(b.outline()[1].textOff, 3);
  CHECK_EQ_INT(b.outline()[1].textLen, 3);  // "B b"
}

TEST(docindex, front_matter_and_defs) {
  const std::string doc = "---\ntitle: x\n---\nbody\n\n[^1]: note\n";
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 3);
  CHECK(ix.KindOf(0) == doc::Kind::FrontMatter);
  CHECK(ix.IsHidden(0));
  CHECK_EQ_INT(static_cast<long long>(ix.Len(0)), 16);  // 到闭合 --- 行（不含 \n）
  CHECK(ix.KindOf(1) == doc::Kind::Para);
  CHECK(!ix.IsHidden(1));
  // 紧贴普通段落的定义行不做懒续行会并进上一段（见实现）；空行隔开后
  // 才独立成 hidden 块——这里顺带把脚注标志锁住。
  CHECK(ix.KindOf(2) == doc::Kind::Para);
  CHECK(ix.IsHidden(2) && ix.IsFootnoteDef(2));
}

// 定义行紧贴普通段落：并入上一段，不单独成块（实现里的既定取舍）。
TEST(docindex, def_line_merges_into_para) {
  const std::string doc = "body\n[^1]: note\n";
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 1);
  CHECK(ix.KindOf(0) == doc::Kind::Para);
  CHECK(!ix.IsHidden(0));
  CHECK_EQ_INT(static_cast<long long>(ix.Len(0)), 15);  // "body\n[^1]: note"
}

TEST(docindex, loose_list) {
  // 空行后接同级项：两侧列表项各自成块、都标 loose
  const std::string doc = "- a\n\n- b\n";
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 2);
  CHECK(ix.KindOf(0) == doc::Kind::ListItem);
  CHECK(ix.IsLoose(0));
  CHECK(ix.KindOf(1) == doc::Kind::ListItem);
  CHECK(ix.IsLoose(1));
}

TEST(docindex, block_at_offset) {
  const std::string doc = "aaa\n\nbbb\n\nccc\n";
  // 块：0 Para(aaa) 1 Para(bbb) 2 Para(ccc)，空行落在块间
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 0, 0, &b));
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 3);
  CHECK_EQ_INT(static_cast<long long>(ix.Start(0)), 0);
  CHECK_EQ_INT(static_cast<long long>(ix.Start(1)), 5);
  CHECK_EQ_INT(static_cast<long long>(ix.Start(2)), 10);

  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(0)), 0);
  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(2)), 0);
  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(3)), 0);  // 行尾 \n 归当前块
  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(4)), 0);  // 空行 → 前一块
  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(5)), 1);  // 块起点精确命中
  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(7)), 1);
  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(9)), 1);
  CHECK_EQ_INT(static_cast<long long>(ix.BlockAtOffset(12)), 2); // 最后一块内
}

TEST(docindex, big_fence_lines) {
  std::string doc = "```\n";
  for (int i = 0; i < 2100; ++i) doc += "line\n";
  doc += "```\n";
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 1 << 20, 0, &b));
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 1);
  CHECK(ix.KindOf(0) == doc::Kind::Fence);
  CHECK_EQ_INT(ix.BigLines(0), 2102);  // 开栏 + 2100 行 + 收栏
  // 块覆盖开栏首字节到收栏末字节（不含行尾 \n）
  CHECK_EQ_INT(static_cast<long long>(ix.Len(0)), static_cast<long long>(doc.size() - 1));
  CHECK(!ix.overflowed());
}

TEST(docindex, limit_prefix_scan) {
  const std::string doc(4000, 'x');  // 4000 个字节的普通段落
  doc::IndexBuilder b;
  REQUIRE(BuildUtf8(doc, 512, 1000, &b));
  CHECK_EQ_INT(static_cast<long long>(b.BytesScanned()), 1000);
  CHECK(!b.CanFeedMore());
  CHECK_EQ_INT(static_cast<long long>(b.index().BlockCount()), 1);  // 1000 字节的一段
}

TEST(docindex, page_boundary_no_false_overflow) {
  // 页表按 Begin 的 fileSize 预留（pageCount = fileSize/4096 + 1）。
  // 落在最后有效页里的块必须保留，不许误报 overflow。
  std::string doc(4096 - 10, 'a');
  doc += "\n\n";                       // 第一段结束在页 0
  doc.append(4096, 'b');               // 第二段横跨页 1，起点 4088
  doc::IndexBuilder b;
  doc::EncodingInfo enc;  // Utf8
  REQUIRE(b.Begin(enc, doc.size(), 0));
  b.Feed(doc.data(), doc.size());
  b.Finish();
  CHECK(!b.index().overflowed());
  CHECK_EQ_INT(static_cast<long long>(b.index().BlockCount()), 2);

  // Feed 的字节量被 Begin 声明的 fileSize 截住：声明 64 字节就只扫 64 字节，
  // 多喂的部分忽略（页表溢出的丢弃分支因此从公共 API 不可达，纯防御）。
  doc::IndexBuilder small;
  REQUIRE(small.Begin(enc, 64, 0));
  small.Feed(doc.data(), doc.size());
  small.Finish();
  CHECK(!small.index().overflowed());
  CHECK_EQ_INT(static_cast<long long>(small.BytesScanned()), 64);
  CHECK_EQ_INT(static_cast<long long>(small.index().BlockCount()), 1);
}

TEST(docindex, empty_document) {
  doc::IndexBuilder b;
  doc::EncodingInfo enc;
  REQUIRE(b.Begin(enc, 0, 0));
  b.Finish();
  CHECK(b.index().Empty());
  CHECK(b.finished());
  CHECK(b.outline().empty());
}

TEST(docindex, utf16le_units) {
  // UTF-16LE（带 BOM）：块种类与 UTF-8 相同，偏移按 2 字节单元，BOM 跳过。
  const std::wstring text = L"## H\npara\n";
  std::string bytes = "\xff\xfe";
  for (wchar_t c : text) {
    bytes += static_cast<char>(c & 0xFF);
    bytes += static_cast<char>((c >> 8) & 0xFF);
  }
  doc::IndexBuilder b;
  doc::EncodingInfo enc;
  enc.encoding = doc::Encoding::Utf16LE;
  enc.bomBytes = 2;
  REQUIRE(b.Begin(enc, bytes.size(), 0));
  b.Feed(bytes.data(), bytes.size());
  b.Finish();
  const doc::BlockIndex& ix = b.index();
  REQUIRE(ix.BlockCount() == 2);
  CHECK(ix.KindOf(0) == doc::Kind::H2);
  CHECK(ix.KindOf(1) == doc::Kind::Para);
  // BOM 2 字节之后，偏移按 2 字节/单元：H2 "## H" 在 2..10，para "para" 在 12..20
  CHECK_EQ_INT(static_cast<long long>(ix.Start(0)), 2);
  CHECK_EQ_INT(static_cast<long long>(ix.Len(0)), 8);
  CHECK_EQ_INT(static_cast<long long>(ix.Start(1)), 12);
  CHECK_EQ_INT(static_cast<long long>(ix.Len(1)), 8);
}
