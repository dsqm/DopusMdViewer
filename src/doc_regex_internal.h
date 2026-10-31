// doc_regex_internal.h - 各宽度扫描器的内部接口（doc_regex8/16.cpp 实现）。
#pragma once

#include <atomic>
#include <string>

#include "doc_regex.h"

namespace doc {

bool RegexScanUtf8(const std::wstring& path, uint64_t startOffset, const std::string& pattern,
                   bool caseless,
                   const std::atomic<bool>* cancel, RegexHits* out, std::wstring* error);

bool RegexScanUtf16(const std::wstring& path, uint64_t startOffset, bool bigEndian,
                    const std::wstring& pattern, bool caseless, const std::atomic<bool>* cancel, RegexHits* out,
                    std::wstring* error);

bool RegexScanGb18030(const std::wstring& path, uint64_t startOffset,
                      const std::wstring& pattern, bool caseless,
                      const std::atomic<bool>* cancel, RegexHits* out, std::wstring* error);

}  // namespace doc
