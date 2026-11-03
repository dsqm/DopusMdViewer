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

// 按编码挑宽度。起点是 BOM 之后的第一个字节（enc.bomBytes）。
bool ScanPattern(const std::wstring& path, const EncodingInfo& enc,
                 const std::wstring& pattern, bool caseless,
                 const std::atomic<bool>* cancel, RegexHits* out, std::wstring* error) {
  switch (enc.encoding) {
    case Encoding::Utf8: {
      const std::string p8 = WideToUtf8(pattern);
      return RegexScanUtf8(path, enc.bomBytes, p8, caseless, cancel, out, error);
    }
    case Encoding::Utf16LE:
      return RegexScanUtf16(path, enc.bomBytes, false, pattern, caseless, cancel, out, error);
    case Encoding::Utf16BE:
      return RegexScanUtf16(path, enc.bomBytes, true, pattern, caseless, cancel, out, error);
    case Encoding::Legacy:
      return RegexScanLegacy(path, enc.bomBytes, enc.codepage, pattern, caseless, cancel, out,
                             error);
  }
  if (error) *error = L"不支持的编码";
  return false;
}

// 大小写映射，与语言无关（LOCALE_NAME_INVARIANT）。注意它做的是**全折叠**
// （ß → SS，长度会变），比 PCRE2 的简单折叠宽，所以只能用来判断"有没有大小写
// 变体"，不能拿它和原文做等价比较。
std::wstring MapCase(const std::wstring& s, DWORD flags) {
  if (s.empty()) return std::wstring();
  const int srcLen = static_cast<int>(s.size());
  const int cap = srcLen * 2 + 16;  // 映射可能变长（ß → SS）
  std::wstring out(static_cast<size_t>(cap), L'\0');
  const int n = ::LCMapStringEx(LOCALE_NAME_INVARIANT, flags, s.data(), srcLen, &out[0], cap,
                                nullptr, nullptr, 0);
  if (n <= 0) return s;
  out.resize(static_cast<size_t>(n));
  return out;
}

// \Q…\E 里的 \E 必须断开：先结束引用、写一个转义的反斜杠与 E、再重新开始引用。
std::wstring QuoteLiteral(const std::wstring& needle) {
  std::wstring out;
  out.reserve(needle.size() + 8);
  out += L"\\Q";
  size_t i = 0;
  for (;;) {
    const size_t p = needle.find(L"\\E", i);
    if (p == std::wstring::npos) {
      out.append(needle, i, std::wstring::npos);
      break;
    }
    out.append(needle, i, p - i);
    out += L"\\E\\\\E\\Q";
    i = p + 2;
  }
  out += L"\\E";
  return out;
}

}  // namespace

bool RegexSearchFile(const std::wstring& path, const EncodingInfo& encoding,
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

  return ScanPattern(path, encoding, pattern, !caseSensitive, cancel, out, error);
}

bool LiteralNeedsUnicodeFold(const std::wstring& needle) {
  // 只对非 ASCII 部分做判定：拿整串去映射时，只要有一个英文字母就会变，
  // 非 ASCII 部分到底有没有大小写反而看不出来（中日韩文本夹个英文单词就白走慢路径）。
  std::wstring nonAscii;
  for (wchar_t c : needle) {
    if (static_cast<uint32_t>(c) >= 0x80) nonAscii += c;
  }
  if (nonAscii.empty()) return false;  // 纯 ASCII 由调用方自己折叠
  // 有大／小写变体才需要 Unicode 折叠；中日韩这类无大小写的文字继续走快路径。
  return MapCase(nonAscii, LCMAP_UPPERCASE) != nonAscii ||
         MapCase(nonAscii, LCMAP_LOWERCASE) != nonAscii;
}

bool LiteralSearchFile(const std::wstring& path, const EncodingInfo& encoding,
                       bool caseSensitive, const std::wstring& needle, bool wholeWord,
                       const std::atomic<bool>* cancel, RegexHits* out,
                       std::wstring* error) {
  if (!out) return false;
  out->offsets.clear();
  out->total = 0;
  out->truncated = false;
  out->limited = false;
  if (error) error->clear();
  if (needle.empty()) return true;

  std::wstring pattern;
  pattern.reserve(needle.size() + 48);
  if (wholeWord) pattern += L"(?<![A-Za-z0-9_])";
  pattern += QuoteLiteral(needle);
  if (wholeWord) pattern += L"(?![A-Za-z0-9_])";

  return ScanPattern(path, encoding, pattern, !caseSensitive, cancel, out, error);
}

}  // namespace doc
