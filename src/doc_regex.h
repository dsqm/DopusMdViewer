// doc_regex.h - 页内正则搜索（PCRE2 8/16 位，票 14）。
//
// 匹配跑在**源文本**上：UTF-8 用 8 位引擎，UTF-16 与 GB18030（解码后）用 16 位。
// 按行对齐分块，块与块之间保留 64 KB 前瞻；只在后台线程调用。

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "doc_index.h"

namespace doc {

struct RegexHits {
  std::vector<uint64_t> offsets;  // 源文件字节偏移，升序，最多 10000 条
  uint64_t total = 0;             // 精确命中数（受匹配上限影响时可能偏小）
  bool truncated = false;         // offsets 被截断
  bool limited = false;           // 触发过 PCRE2 的匹配/深度上限，结果不完整
};

// bomBytes：跳过文件头的 BOM（正则从第一个真实字符开始，^ 才能命中首行）。
// 编译失败、文件含非法编码序列等返回 false，error 给一句面向用户的原因。
// caseSensitive=false 时加 PCRE2_CASELESS；始终 PCRE2_MULTILINE | PCRE2_UTF | PCRE2_UCP。
bool RegexSearchFile(const std::wstring& path, Encoding encoding, uint32_t bomBytes,
                     const std::wstring& pattern, bool caseSensitive,
                     const std::atomic<bool>* cancel, RegexHits* out,
                     std::wstring* error);

}  // namespace doc
