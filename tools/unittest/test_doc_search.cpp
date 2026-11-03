// test_doc_search.cpp - 字节搜索器（doc_search.cpp）的单元测试。
//
// 多线程分段扫、命中上限、整词与 ASCII 折叠都跑在临时文件上；
// 契约见 doc_search.h：offsets 升序、total 可大于 offsets.size()、
// 大小写不敏感只折叠 ASCII。

#include "doc_search.h"

#include <atomic>
#include <string>

#include "test.h"
#include "util.h"

using namespace unittest;

namespace {

// 把 bytes 写进临时文件，返回路径。写失败时不打断流程：测试里的
// REQUIRE 断言会自然落空并报失败。
std::wstring TempFile(const char* tag, const std::string& bytes) {
  const std::wstring dir = MakeTempDir(tag);
  const std::wstring path = dir + L"\\doc.txt";
  CHECK_MSG(WriteBytes(path, bytes), "temp file write failed");
  return path;
}

}  // namespace

TEST(search, basic_offsets) {
  const std::wstring path = TempFile("search", "abc X abc\nabc");
  const doc::SearchHits h = doc::SearchFileBytes(path, "abc", 4);
  CHECK_EQ_INT(static_cast<long long>(h.total), 3);
  REQUIRE(h.offsets.size() == 3);
  CHECK_EQ_INT(static_cast<long long>(h.offsets[0]), 0);
  CHECK_EQ_INT(static_cast<long long>(h.offsets[1]), 6);
  CHECK_EQ_INT(static_cast<long long>(h.offsets[2]), 10);
  CHECK(!h.truncated);
}

TEST(search, case_fold_ascii_only) {
  const std::string bytes = "Abc abc ABC";
  const std::wstring path = TempFile("search", bytes);
  const doc::SearchHits cs = doc::SearchFileBytes(path, "abc", 2, nullptr, true);
  CHECK_EQ_INT(static_cast<long long>(cs.total), 1);
  const doc::SearchHits ci = doc::SearchFileBytes(path, "abc", 2, nullptr, false);
  CHECK_EQ_INT(static_cast<long long>(ci.total), 3);
}

TEST(search, whole_word) {
  // cat 命中：cat / cat. / 结尾 cat；cat2、xcat、_cat 不算
  // （偏移：cat@0 cat2@4 xcat@9 _cat@14 cat.@19 cat@24）
  const std::wstring path = TempFile("search", "cat cat2 xcat _cat cat. cat");
  const doc::SearchHits ww = doc::SearchFileBytes(path, "cat", 2, nullptr, true, true);
  CHECK_EQ_INT(static_cast<long long>(ww.total), 3);
  REQUIRE(ww.offsets.size() == 3);
  CHECK_EQ_INT(static_cast<long long>(ww.offsets[0]), 0);
  CHECK_EQ_INT(static_cast<long long>(ww.offsets[1]), 19);
  CHECK_EQ_INT(static_cast<long long>(ww.offsets[2]), 24);

  // 关整词 → 全部命中
  const doc::SearchHits plain = doc::SearchFileBytes(path, "cat", 2, nullptr, true, false);
  CHECK_EQ_INT(static_cast<long long>(plain.total), 6);
}

TEST(search, hits_spanning_segment_boundaries) {
  // 真开多线程（每段 64 KB 以下只给 1 线程，所以文件得够大）：256 KB、4 线程，
  // 段边界在 65536 / 131072 / 196608。每条边界压三种位置——贴边内侧、跨界、
  // 压线起点——外加段内常规位与文件尾，全部必须恰好记一次。
  // 注意布点不能重叠：相邻 needle 会互相覆写字节，等于少写了一个。
  constexpr size_t kSize = 256 * 1024;
  std::string bytes(kSize, 'x');
  const std::string needle = "find";
  const size_t kSpots[] = {
      65528,   // 边界1 内侧
      65536,   // 边界1 起点恰好压线（归下一段记）
      65544,   // 边界1 外侧
      131064,  // 边界2 内侧
      131070,  // 边界2 跨界（131070..131073 跨过 131072）
      131077,  // 边界2 外侧
      196600,  // 边界3 内侧
      196606,  // 边界3 跨界
      196613,  // 边界3 外侧
      kSize - 4,  // 文件尾
  };
  for (size_t off : kSpots) {
    for (size_t k = 0; k < needle.size(); ++k) bytes[off + k] = needle[k];
  }
  const std::wstring path = TempFile("search", bytes);
  const doc::SearchHits h = doc::SearchFileBytes(path, needle, 4);
  CHECK_EQ_INT(static_cast<long long>(h.total), 10);
  CHECK_EQ_INT(static_cast<long long>(h.offsets.size()), 10);
  for (size_t i = 0; i < h.offsets.size() && i < 10; ++i) {
    CHECK_EQ_INT(static_cast<long long>(h.offsets[i]),
                 static_cast<long long>(kSpots[i]));
  }
}

TEST(search, cap_and_truncated) {
  // 12000 个命中：offsets 截到 10000，total 保留真实数
  std::string bytes;
  for (int i = 0; i < 12000; ++i) bytes += "ab ";
  const std::wstring path = TempFile("search", bytes);
  const doc::SearchHits h = doc::SearchFileBytes(path, "ab", 4);
  CHECK_EQ_INT(static_cast<long long>(h.total), 12000);
  CHECK_EQ_INT(static_cast<long long>(h.offsets.size()), 10000);
  CHECK(h.truncated);
}

TEST(search, degenerate_inputs) {
  const std::wstring path = TempFile("search", "content here");
  // 空 needle：直接返回，无错误
  std::wstring err;
  doc::SearchHits h = doc::SearchFileBytes(path, "", 4, &err);
  CHECK_EQ_INT(static_cast<long long>(h.total), 0);
  CHECK(err.empty());
  // needle 比文件长
  h = doc::SearchFileBytes(path, "much longer than the file contents", 4);
  CHECK_EQ_INT(static_cast<long long>(h.total), 0);
  // 文件不存在：error 给话
  h = doc::SearchFileBytes(TempRoot() + L"\\no-such-file.txt", "x", 4, &err);
  CHECK_EQ_INT(static_cast<long long>(h.total), 0);
  CHECK(!err.empty());
}

TEST(search, cancel_requested) {
  std::string bytes;
  for (int i = 0; i < 100000; ++i) bytes += "needle ";
  const std::wstring path = TempFile("search", bytes);
  std::atomic<bool> cancel(true);  // 一开工就收手
  const doc::SearchHits h = doc::SearchFileBytes(path, "needle", 4, nullptr, true, false, &cancel);
  // 结果只是已扫区段的并集，不保证完整，但必须是空或前缀
  CHECK(h.offsets.empty() || h.offsets.front() == 0);
}
