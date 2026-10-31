// doc_search.h - 页内搜索（票 04：不建倒排索引，多线程线性扫）。
//
// 只做字节匹配，服务于 UTF-8 / GB18030 源文件。UTF-16 源在搜索前由调用方
// 决定是否转成宽字符模式（M6 的搜索 UI 再定大小写与整词选项）。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace doc {

struct SearchHits {
  std::vector<uint64_t> offsets;  // 源文件字节偏移，升序
  uint64_t total = 0;             // 实际命中数（可能大于 offsets.size()）
  bool truncated = false;         // 超过上限只留前 10000 条
};

// threads <= 0 时用 4。命中上限 10000（票 04）。
SearchHits SearchFileBytes(const std::wstring& path, const std::string& needle,
                           int threads, std::wstring* error = nullptr);

}  // namespace doc
