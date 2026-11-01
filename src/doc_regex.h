// doc_regex.h - 页内正则搜索（PCRE2 8/16 位）。
//
// 匹配跑在**源文本**上：UTF-8 用 8 位引擎，UTF-16 直接按单元拼，Legacy（GB18030 /
// Shift_JIS / Big5 / EUC / ISO-2022 / 单字节西欧与西里尔）先解码成 UTF-16 再匹配。
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
bool RegexSearchFile(const std::wstring& path, const EncodingInfo& encoding,
                     const std::wstring& pattern, bool caseSensitive,
                     const std::atomic<bool>* cancel, RegexHits* out,
                     std::wstring* error);

// 字面量搜索的 Unicode 大小写折叠：needle 里存在有大／小写变体的非 ASCII 字符时返回 true。
// 字节扫描器（doc_search）与宽扫描器只折叠 ASCII，只有这种情况必须走 PCRE2 才正确；
// 纯 ASCII 或纯中日韩（无大小写）的 needle 一律返回 false，继续用快路径。
bool LiteralNeedsUnicodeFold(const std::wstring& needle);

// 大小写不敏感的字面量搜索：内部拼成 \Q…\E（wholeWord 再补 ASCII 词边界断言）交给
// RegexSearchFile。偏移与编码处理与正则完全一致，用于两种快路径覆盖不了的情况：
// LiteralNeedsUnicodeFold 为真，或编码是 ISO-2022 这类状态化 7 位编码。
bool LiteralSearchFile(const std::wstring& path, const EncodingInfo& encoding,
                       bool caseSensitive, const std::wstring& needle, bool wholeWord,
                       const std::atomic<bool>* cancel, RegexHits* out, std::wstring* error);

}  // namespace doc
