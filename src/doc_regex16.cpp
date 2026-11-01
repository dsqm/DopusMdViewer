// doc_regex16.cpp - UTF-16 与 Legacy 源文件上的 PCRE2 16 位扫描。
//
// UTF-16 直接按文件字节序拼成 UTF-16 单元；Legacy（GB18030 / Shift_JIS / Big5 /
// EUC / ISO-2022 / 单字节页）先按块解码成 UTF-16，再把命中的单元偏移映射回源字节
// 偏移（命中升序，映射游标只向前走）。

#define PCRE2_CODE_UNIT_WIDTH 16
#include "pcre2.h"

#include <cstring>

#include "doc_regex_common.h"
#include "doc_regex_internal.h"

namespace doc {
namespace {

using namespace regex_detail;

std::wstring CompileMessage(int code, PCRE2_SIZE offset) {
  PCRE2_UCHAR16 buf[256];
  const int n = pcre2_get_error_message(code, buf, 256);
  std::wstring msg = L"正则无效";
  if (n > 0) {
    msg += L"：";
    for (int i = 0; i < n; ++i) msg += static_cast<wchar_t>(buf[i]);
  } else {
    msg += L"（错误码 ";
    msg += std::to_wstring(code);
    msg += L"）";
  }
  if (offset != PCRE2_UNSET) {
    msg += L"（位置 ";
    msg += std::to_wstring(static_cast<unsigned long long>(offset));
    msg += L"）";
  }
  return msg;
}

bool IsResourceLimit(int rc) {
  return rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
         rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_JIT_STACKLIMIT;
}

size_t NextUnit(const uint16_t* p, size_t len, size_t at) {
  size_t next = at + 1;
  if (p[at] >= 0xD800 && p[at] <= 0xDBFF && at + 1 < len && p[at + 1] >= 0xDC00 &&
      p[at + 1] <= 0xDFFF) {
    next = at + 2;
  }
  return next;
}

// Legacy 源字节 → UTF-16 单元的前进量。GB18030 另认 4 字节序列，ISO-2022 按 ESC
// 移位序列步进（ESC 三步不产出单元）。命中的单元偏移转回源字节偏移。
struct LegacyMap {
  UINT cp = 0;
  bool iso2022 = false;
  bool twoByte = false;
  const unsigned char* src = nullptr;
  size_t srcLen = 0;
  const uint16_t* wide = nullptr;
  size_t wideLen = 0;
  size_t byteIdx = 0;
  size_t unitIdx = 0;

  uint64_t ToByte(size_t unitOff) {
    while (unitIdx < unitOff && byteIdx < srcLen) Step();
    return byteIdx;
  }

  void Step() {
    size_t len = 1;
    if (iso2022) {
      if (src[byteIdx] == 0x1B) {
        if (byteIdx + 1 < srcLen) {
          if (src[byteIdx + 1] == '$') {
            twoByte = true;
          } else if (src[byteIdx + 1] == '(' || src[byteIdx + 1] == '.') {
            twoByte = false;
          }
        }
        byteIdx += std::min<size_t>(3, srcLen - byteIdx);
        return;
      }
      if (twoByte && byteIdx + 1 < srcLen) len = 2;
    } else if (cp == 54936) {
      if (src[byteIdx] >= 0x81 && src[byteIdx] <= 0xFE && byteIdx + 1 < srcLen) {
        len = (src[byteIdx + 1] >= 0x30 && src[byteIdx + 1] <= 0x39) ? 4 : 2;
      }
    } else if (::IsDBCSLeadByteEx(cp, src[byteIdx]) && byteIdx + 1 < srcLen) {
      len = 2;
    }
    if (byteIdx + len > srcLen) len = srcLen - byteIdx;
    const bool pair = unitIdx + 1 < wideLen && wide[unitIdx] >= 0xD800 &&
                      wide[unitIdx] <= 0xDBFF;
    unitIdx += pair ? 2 : 1;
    byteIdx += len;
  }
};

}  // namespace

bool RegexScanUtf16(const std::wstring& path, uint64_t startOffset, bool bigEndian,
                    const std::wstring& pattern, bool caseless, const std::atomic<bool>* cancel, RegexHits* out,
                    std::wstring* error) {
  HANDLE h = INVALID_HANDLE_VALUE;
  uint64_t fileSize = 0;
  if (!OpenForScan(path, &h, &fileSize, error)) return false;

  const uint32_t opts =
      PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE | (caseless ? PCRE2_CASELESS : 0);
  int err = 0;
  PCRE2_SIZE erroff = 0;
  pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                 static_cast<PCRE2_SIZE>(pattern.size()), opts, &err,
                                 &erroff, nullptr);
  if (!re) {
    if (error) *error = CompileMessage(err, erroff);
    ::CloseHandle(h);
    return false;
  }
  pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
  pcre2_match_context* mctx = pcre2_match_context_create(nullptr);
  if (mctx) {
    pcre2_set_match_limit(mctx, 10000000);
    pcre2_set_depth_limit(mctx, 1000);
  }
  pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);

  std::vector<unsigned char> raw;
  std::vector<uint16_t> units;
  uint64_t chunkStart = std::min<uint64_t>(startOffset, fileSize);
  bool lineStart = true;
  bool ok = true;

  while (chunkStart < fileSize) {
    if (cancel && cancel->load(std::memory_order_relaxed)) break;

    size_t got = 0;
    const uint64_t remain = fileSize - chunkStart;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kWindowBytes, remain));
    if (!ReadWindow(h, chunkStart, want, &raw, &got)) break;
    if (got < 2) break;

    const uint64_t rawEnd = std::min<uint64_t>(chunkStart + kChunkBytes, fileSize);
    uint64_t chunkEnd = rawEnd;
    bool chunkLineStart = true;
    if (rawEnd < fileSize) {
      const LineCut cut = FindLineEnd16(raw.data(), got, rawEnd - chunkStart, kLineSlack,
                                        bigEndian);
      chunkEnd = chunkStart + cut.pos;
      chunkLineStart = cut.atLineStart;
    }
    if (chunkEnd > chunkStart + got) {
      chunkEnd = chunkStart + got;
      chunkLineStart = false;
    }
    if (chunkEnd <= chunkStart) chunkEnd = chunkStart + got;
    chunkEnd = chunkStart + ((chunkEnd - chunkStart) & ~1ull);

    uint64_t subjectEnd = fileSize;
    if (chunkEnd < fileSize) {
      const uint64_t target = std::min<uint64_t>(chunkEnd + kLookahead, fileSize);
      if (target < fileSize) {
        const LineCut cut = FindLineEnd16(raw.data(), got, target - chunkStart, kLineSlack,
                                          bigEndian);
        subjectEnd = chunkStart + cut.pos;
      }
    }
    if (subjectEnd > chunkStart + got) subjectEnd = chunkStart + got;
    if (subjectEnd < chunkEnd) subjectEnd = chunkEnd;
    subjectEnd = chunkStart + ((subjectEnd - chunkStart) & ~1ull);

    const size_t subjectUnits = static_cast<size_t>((subjectEnd - chunkStart) / 2);
    const size_t chunkUnits = static_cast<size_t>((chunkEnd - chunkStart) / 2);
    if (subjectUnits == 0) break;

    units.resize(subjectUnits);
    const size_t unitsInWindow = std::min<size_t>(subjectUnits, got / 2);
    if (bigEndian) {
      for (size_t i = 0; i < unitsInWindow; ++i) {
        units[i] = static_cast<uint16_t>((raw[2 * i] << 8) | raw[2 * i + 1]);
      }
    } else {
      ::memcpy(units.data(), raw.data(), unitsInWindow * 2);
    }

    uint32_t flags = 0;
    if (!lineStart) flags |= PCRE2_NOTBOL;
    if (subjectEnd < fileSize) flags |= PCRE2_NOTEOL;

    PCRE2_SIZE start = 0;
    bool validated = false;
    bool canceled = false;
    while (start < chunkUnits) {
      if ((out->total & 0xFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed)) {
        canceled = true;
        break;
      }
      const uint32_t callFlags = flags | (validated ? PCRE2_NO_UTF_CHECK : 0);
      const int rc = pcre2_match(re, units.data(), subjectUnits, start, callFlags, md, mctx);
      if (rc == PCRE2_ERROR_NOMATCH) break;
      if (rc < 0) {
        if (IsResourceLimit(rc)) {
          // 触上限就整篇停下（同 8 位实现）。
          out->limited = true;
          canceled = true;
        } else if (!validated && error) {
          *error = L"文件含无效的 UTF-16 序列（偏移 " +
                   std::to_wstring(static_cast<unsigned long long>(chunkStart + start * 2)) +
                   L"）";
          ok = false;
        }
        break;
      }
      validated = true;
      const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
      const PCRE2_SIZE s = ov[0];
      if (s >= chunkUnits) break;
      ++out->total;
      if (out->offsets.size() < kMaxStored) {
        out->offsets.push_back(chunkStart + static_cast<uint64_t>(s) * 2);
      }
      if (ov[1] > s) {
        start = ov[1];
      } else {
        start = NextUnit(units.data(), subjectUnits, s);
      }
    }
    if (!ok || canceled) break;
    if (cancel && cancel->load(std::memory_order_relaxed)) break;

    lineStart = chunkLineStart;
    chunkStart = chunkEnd;
  }

  if (md) pcre2_match_data_free(md);
  if (mctx) pcre2_match_context_free(mctx);
  pcre2_code_free(re);
  ::CloseHandle(h);
  out->truncated = out->total > out->offsets.size() || out->limited;
  return ok;
}

bool RegexScanLegacy(const std::wstring& path, uint64_t startOffset, uint32_t codepage,
                     const std::wstring& pattern, bool caseless,
                     const std::atomic<bool>* cancel, RegexHits* out, std::wstring* error) {
  const UINT cp = static_cast<UINT>(codepage);
  HANDLE h = INVALID_HANDLE_VALUE;
  uint64_t fileSize = 0;
  if (!OpenForScan(path, &h, &fileSize, error)) return false;

  const uint32_t opts =
      PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE | (caseless ? PCRE2_CASELESS : 0);
  int err = 0;
  PCRE2_SIZE erroff = 0;
  pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                 static_cast<PCRE2_SIZE>(pattern.size()), opts, &err,
                                 &erroff, nullptr);
  if (!re) {
    if (error) *error = CompileMessage(err, erroff);
    ::CloseHandle(h);
    return false;
  }
  pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
  pcre2_match_context* mctx = pcre2_match_context_create(nullptr);
  if (mctx) {
    pcre2_set_match_limit(mctx, 10000000);
    pcre2_set_depth_limit(mctx, 1000);
  }
  pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);

  std::vector<unsigned char> raw;
  std::vector<uint16_t> wide;
  uint64_t chunkStart = std::min<uint64_t>(startOffset, fileSize);
  bool lineStart = true;
  bool ok = true;

  while (chunkStart < fileSize) {
    if (cancel && cancel->load(std::memory_order_relaxed)) break;

    size_t got = 0;
    const uint64_t remain = fileSize - chunkStart;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kWindowBytes, remain));
    if (!ReadWindow(h, chunkStart, want, &raw, &got)) break;
    if (got == 0) break;

    const uint64_t rawEnd = std::min<uint64_t>(chunkStart + kChunkBytes, fileSize);
    uint64_t chunkEnd = rawEnd;
    bool chunkLineStart = true;
    if (rawEnd < fileSize) {
      const LineCut cut = FindLineEnd8(raw.data(), got, rawEnd - chunkStart, kLineSlack);
      chunkEnd = chunkStart + cut.pos;
      chunkLineStart = cut.atLineStart;
    }
    if (chunkEnd > chunkStart + got) {
      chunkEnd = chunkStart + got;
      chunkLineStart = false;
    }
    if (chunkEnd <= chunkStart) chunkEnd = chunkStart + got;

    uint64_t subjectEnd = fileSize;
    if (chunkEnd < fileSize) {
      const uint64_t target = std::min<uint64_t>(chunkEnd + kLookahead, fileSize);
      if (target < fileSize) {
        const LineCut cut = FindLineEnd8(raw.data(), got, target - chunkStart, kLineSlack);
        subjectEnd = chunkStart + cut.pos;
      }
    }
    if (subjectEnd > chunkStart + got) subjectEnd = chunkStart + got;
    if (subjectEnd < chunkEnd) subjectEnd = chunkEnd;

    const int srcBytes = static_cast<int>(subjectEnd - chunkStart);
    wide.resize(static_cast<size_t>(srcBytes) + 1);
    const int wideLen = ::MultiByteToWideChar(
        cp, 0, reinterpret_cast<LPCCH>(raw.data()), srcBytes,
        reinterpret_cast<LPWSTR>(wide.data()), static_cast<int>(wide.size()));
    if (wideLen <= 0) {
      if (error) *error = L"解码失败（代码页 " + std::to_wstring(codepage) + L"，偏移 " +
                          std::to_wstring(static_cast<unsigned long long>(chunkStart)) + L"）";
      ok = false;
      break;
    }
    const size_t subjectUnits = static_cast<size_t>(wideLen);

    // 单元偏移 → 块内源字节偏移的起点；chunkEnd 在字节上，换算成单元。
    LegacyMap map;
    map.cp = cp;
    map.iso2022 = IsIso2022CodePage(codepage);
    map.src = raw.data();
    map.srcLen = static_cast<size_t>(subjectEnd - chunkStart);
    map.wide = wide.data();
    map.wideLen = subjectUnits;

    uint32_t flags = 0;
    if (!lineStart) flags |= PCRE2_NOTBOL;
    if (subjectEnd < fileSize) flags |= PCRE2_NOTEOL;

    PCRE2_SIZE start = 0;
    bool canceled = false;
    while (start < subjectUnits) {
      if ((out->total & 0xFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed)) {
        canceled = true;
        break;
      }
      // 解码结果一定是合法 UTF-16，不必再校验。
      const int rc = pcre2_match(re, wide.data(), subjectUnits, start,
                                 flags | PCRE2_NO_UTF_CHECK, md, mctx);
      if (rc == PCRE2_ERROR_NOMATCH) break;
      if (rc < 0) {
        if (IsResourceLimit(rc)) {
          out->limited = true;
          canceled = true;
        }
        break;
      }
      const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
      const PCRE2_SIZE s = ov[0];
      const uint64_t byteOff = map.ToByte(static_cast<size_t>(s));
      if (chunkStart + byteOff >= chunkEnd) break;  // 起点在下一块
      ++out->total;
      if (out->offsets.size() < kMaxStored) out->offsets.push_back(chunkStart + byteOff);
      if (ov[1] > s) {
        start = ov[1];
      } else {
        start = NextUnit(wide.data(), subjectUnits, s);
      }
    }
    if (!ok || canceled) break;
    if (cancel && cancel->load(std::memory_order_relaxed)) break;

    lineStart = chunkLineStart;
    chunkStart = chunkEnd;
  }

  if (md) pcre2_match_data_free(md);
  if (mctx) pcre2_match_context_free(mctx);
  pcre2_code_free(re);
  ::CloseHandle(h);
  out->truncated = out->total > out->offsets.size() || out->limited;
  return ok;
}

}  // namespace doc
