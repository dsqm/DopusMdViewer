// doc_regex.cpp - 正则搜索入口：按编码分派到 8 位或 16 位扫描器。

#include "doc_regex.h"

#include <windows.h>

#include "doc_regex_internal.h"

namespace doc {
namespace {

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (len <= 0) return std::string();
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len,
                        nullptr, nullptr);
  return out;
}

}  // namespace

bool RegexSearchFile(const std::wstring& path, Encoding encoding, uint32_t bomBytes,
                     const std::wstring& pattern, bool caseSensitive,
                     const std::atomic<bool>* cancel, RegexHits* out,
                     std::wstring* error) {
  if (!out) return false;
  out->offsets.clear();
  out->total = 0;
  out->truncated = false;
  out->limited = false;
  if (error) error->clear();

  if (pattern.empty()) {
    if (error) *error = L"正则不能为空";
    return false;
  }
  if (pattern.size() > 1024) {
    if (error) *error = L"正则过长（上限 1024 字符）";
    return false;
  }

  const bool caseless = !caseSensitive;
  const uint64_t start = bomBytes;
  switch (encoding) {
    case Encoding::Utf8: {
      const std::string p8 = WideToUtf8(pattern);
      return RegexScanUtf8(path, start, p8, caseless, cancel, out, error);
    }
    case Encoding::Utf16LE:
      return RegexScanUtf16(path, start, false, pattern, caseless, cancel, out, error);
    case Encoding::Utf16BE:
      return RegexScanUtf16(path, start, true, pattern, caseless, cancel, out, error);
    case Encoding::Gb18030:
      return RegexScanGb18030(path, start, pattern, caseless, cancel, out, error);
  }
  if (error) *error = L"不支持的编码";
  return false;
}

}  // namespace doc
