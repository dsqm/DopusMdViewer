// doc_search.h - 页内搜索（票 04：不建倒排索引，多线程线性扫）。
//
// 只做字节匹配，服务于 UTF-8 / GB18030 源文件。UTF-16 源在搜索前由调用方
// 决定是否转成宽字符模式（M6 的搜索 UI 再定大小写与整词选项）。

#pragma once

#include <atomic>
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
// caseSensitive=false 时只折叠 ASCII 字母，多字节编码（UTF-8/GB18030）按原字节比较。
// needle 里含带大小写变体的非 ASCII 字符时，这一层给不出正确结果，由调用方改走
// doc::LiteralSearchFile（PCRE2 的 Unicode 简单折叠），见 doc_regex.h。
// wholeWord=true 时要求命中前后都不是 ASCII 词字符（字母、数字、下划线）；
// 对中文这类没有词边界的文字不生效，查询本身就等于整段。
// cancel 非空且置位时尽快返回（每读 1 MB 查一次），结果只保证是前缀。
SearchHits SearchFileBytes(const std::wstring& path, const std::string& needle,
                           int threads, std::wstring* error = nullptr,
                           bool caseSensitive = true, bool wholeWord = false,
                           const std::atomic<bool>* cancel = nullptr);

}  // namespace doc
